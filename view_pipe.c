/*
 * Map to memstream glocal section and print out header contents.
 * Usages:
 *    view_pipe mailbox
 */
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <jpidef.h>			/* VMS Job/Process Information */
#include <syidef.h>			/* VMS System Information */
#include <starlet.h>			/* VMS system services prototypes */
#include <ssdef.h>			/* VMS system service condition codes*/
#include <descrip.h>
#include <lib$routines.h>		/* VMS RTL functions */
#include <builtins.h>			/* DEC C builtin functions */
#include <jpidef.h>
#include <iodef.h>
#include <efndef.h>
#include <dvidef.h>
#include <devdef.h>		/* device characteristics */
#include <dcdef.h>		/* VMS device class numbers */
#include <secdef.h>		/* VMS global sections. */
#include <vadef.h>		/* VMS virtual address space definitions */
#include <lckdef.h>
#include "memstream.h"
#define DM_NO_CRTL_WRAP
#include "dmpipe.h"

#define DM_SECVER_MAJOR 1
#define DM_SECVER_MINOR 1
#define DMPIPE_MEMSTREAM_BLK_SIZE 0x10000	/* 64K */
#define DM_BYPASS_BUFSIZE 1024

struct dm_lksb_valblk_unit {
    pid_t pid;
    union {
	int mask;
	struct {
	    unsigned int 
		shutdown:        1,   /* no more negotiation */
		connect_request: 1,   /* pid wants to establish stream */
		will_write:      1,   /* pid will be writer of stream */
		wake_request:    1,   /* Peer should wake process */
		peer_ack:        1,   /* Peer acknoleges request */
                peer_nak:        1,   /* peer refused request */
		reserved:       10,
		stream_id:      16;   /* sub-id for stream */
	} bit;
    } flags;
};
#pragma assert non_zero(sizeof(struct dm_lksb_valblk_unit)==8) \
   "LKSB building block wrong size"

struct dm_lksb {
    unsigned short int status;
    unsigned short int reserved;
    unsigned int id;

    struct dm_lksb_valblk_unit val[2];
};

struct dm_lock {
   char resnam[32];			/* lock name */
   short state;				/* state currently held */
   short target_state;			/* state converting to */
   int ef;
   unsigned int lock_id;		/* copy of lksb.id */
   unsigned int parent_id;		/* lock ID of parent (0) */
   /*
    * Lock value block exchanged between processes using lock.
    */
   struct dm_lksb lksb;
};
struct dm_devinfo {
    int devclass;
    int devtype;
    long devchar;
    int bufsize;
    pid_t owner;
    int refcnt;
};

/*
 * Top level context structure for bypass.
 */
struct dm_bypass_ctx {
    int related_fd;		/* File descriptor opened on bypassed device */
    pid_t self;			/* Current process's PID */
    unsigned short dtype, chan;	/* Channel assigned to device */
    struct dm_devinfo dvi;	/* device info */
    struct dm_lock lock;	/* enqueued lock */
    struct {			/* accessed at AST level */
	int is_writer;
	int func;		/* QIO function code */
	int is_blocking_lock;
	int mailbox_active;
	struct { unsigned short status, count; pid_t pid; } iosb;
    } mbx_watch;

    struct {
	int size;		/* size of block */
	void *blk;		/* address mapped to global section */
	struct memstream_stats stats;
    } rstream_mem, wstream_mem; /* shared memory descriptors for memstreams */

    memstream rstream;		/* stream for reading */
    memstream wstream;		/* stream for writing */

    int used;
    char buffer[DM_BYPASS_BUFSIZE];	/* buffer for stdio operations */
};


union lock_state {
    struct {
	long flag;				/* 1 if set */
	pid_t owner;
    } state;
    long long state_qw;			/* for atomic exchange */
};
struct commbuf {
    unsigned short fmt_version;
    unsigned short ipc_version;
    pid_t writer_pid;
    pid_t reader_pid;
    long sequence;			/* Stream ID number */
#pragma member_alignment save
    union lock_state lock;
#pragma member_alignment restore
    int data_limit;			/* amount that can be buffered */
    int state;				/* Communication state. */
    int write_pos;			/* Offset of next byte to write */
    int read_pos;			/* offset of next byte to read */

    char data[8];			/* variable size */
};
#define MEMSTREAM_FMT_VERSION 1
#define MEMSTREAM_IPC_VERSION 1
/*
 * Commbuf states:
 */
#define MEMSTREAM_STATE_IDLE 0		/* not full and space available */
#define MEMSTREAM_STATE_EMPTY 1		/* empty and reader waiting */
#define MEMSTREAM_STATE_FULL 2		/* no space and writer waiting */
#define MEMSTREAM_STATE_WRITER_DONE 3   /* Closed by writer */
#define MEMSTREAM_STATE_READER_DONE 4   /* closed by reader */
#define MEMSTREAM_STATE_CLOSED 5	/* Both sides closed */
/*
 * Status values for put_to_commbuf and get_from_commbuf.
 */
#define COMMBUF_COMPLETED 1		/* Operation completed */
#define COMMBUF_BLOCKED 2		/* Operation incomplete */
#define COMMBUF_DISCARDED 4		/* Data discarded, pipe closed */
#define COMMBUF_ABORT 8			/* Unexpected error */
/*
 * memstream_context structure is created by memstream_create function to
 * hold context.
 */
struct memstream_context {
    struct memstream_context *next;	/* list used by exit handler */
    struct commbuf *buf;		/* Shared buffer */
    int is_writer;			/* Indicates which end of stream */
    struct memstream_stats *stats;      /* Optional. */
};
/**************************************************************************/
/* Wrapper for $ENQW call.
 */
static int sys_enq ( int new_state, struct dm_lock *lock, int flags,
	void *astarg, void (*blkast)(void*) )
{
    static $DESCRIPTOR(resource_dx,"");
    struct dsc$descriptor_s *resnam;
    int status;
    /*
     * If lock is new, create a descriptor, otherwise flag a convert operation.
     */
    resnam = 0;
    if ( lock->lock_id == 0 ) {
	resnam = &resource_dx;
	resource_dx.dsc$a_pointer = lock->resnam;
	resource_dx.dsc$w_length = strlen(lock->resnam);
    } else flags |= LCK$M_CONVERT;

    lock->target_state = new_state;
    status = SYS$ENQW ( lock->ef, new_state, &lock->lksb, 
	flags | LCK$M_VALBLK, resnam,
	lock->parent_id, 0, astarg, blkast, 0, 0, 0 );
    if ( status & 1 ) {
	status = lock->lksb.status;
	if ( status&1 ) {
	    if ( !lock->lock_id ) lock->lock_id = lock->lksb.id;
	    lock->state = new_state;
	}
    }
    return status;
}
/*
 * Support routines for locking and inter-process signalling.
 */
static int create_lock ( const char *prefix, char *device_name, 
	struct dm_lock *lock )
{
    char *core_name;
    int status, core_len, resnam_len, ndx;
    /*
     * Construct resource name, skip leading underscores and terminate at colon.
     */
    core_name = device_name;
    while ( *core_name == '_' ) core_name++;
    strcpy ( lock->resnam, prefix );
    resnam_len = strlen ( prefix );

    for ( core_len = 0; core_name[core_len] &&
	(core_name[core_len] != ':'); core_len++ ) {
	if ( resnam_len >= sizeof(lock->resnam) ) return SS$_INVARG;
	lock->resnam[resnam_len++] = toupper(core_name[core_len]);
    }
    lock->resnam[resnam_len] = 0;	/* terminate string */
    lock->ef = EFN$C_ENF;  		/* non event flag */
    /*
     * Get initial lock in CR mode to fetch value block.
     */
    status = sys_enq ( LCK$K_CRMODE, lock, 0, 0, 0 );
    if ( (status & 1) == 0 ) return status;

    return status;
}

static int sys_crmpsc_gpfile ( struct dm_lock *lock, int stream_id,
	int min_size, int flags, void **blk, int *size)
{
    static char sect_name[44];	/* global section name 1-43 chars */
    static $DESCRIPTOR(sect_name_dx,sect_name);
    struct {
        int match;
        struct {
            unsigned int minor:24, major:8;
        } ver;
    } sect_id;
    union {
        unsigned char *bytes[2];
    } inaddr;
    long long section_size, region_id, offset, sect_va, sect_length;
    int i, status, prot;
    /*
     * Create global section from lock and and stream_id.
     */
    if ( strlen ( lock->resnam ) > (sizeof(sect_name)-8) ) return SS$_BADPARAM;
    sprintf ( sect_name, "%s.%d", lock->resnam, stream_id );
    sect_name_dx.dsc$w_length = strlen ( sect_name );
    /*
     * Setup the various parameters for this page file section:
     *    sect_id   - Version 1.1, match all.
     *    prot      - Access by system and owner only.
     *    region_id - Map pages in P0 (low 30 bits).
     *    flags     - Section is demand zero, shared, writable, at new address.
     */
    sect_id.match = SEC$K_MATALL;
    sect_id.ver.minor = DM_SECVER_MAJOR;
    sect_id.ver.major = DM_SECVER_MINOR;
    prot = 0xff00;		/* system and owner access only */
    region_id = VA$C_P0;	/* program region 0 (low 30 bits) */

    flags |= (SEC$M_DZRO|SEC$M_EXPREG|SEC$M_GBL|SEC$M_PAGFIL|SEC$M_WRT);

    section_size = min_size;
    /*
     * Create section.  We hold a lock that any other people trying to
     * create this section should acquire, so there should be no race
     * to initialize it.
     */
    status = SYS$CRMPSC_GPFILE_64 ( &sect_name_dx, &sect_id, prot, 
	section_size, &region_id, 0, 0, flags, &sect_va, &sect_length, 0,0 );
    if ( (status&1) == 0 ) return status;
    if ( (sect_va & 0x7fffffff) != sect_va ) {
	/* Bugcheck, returned address is outside range for 32-bit pointers */
    }
    /*
     * See if we are creator or second user.
     */
    *size = sect_length;
    *blk = (void *) sect_va;		/* change pointer size */
    return status;
}
static void show_valblk_unit ( char *prefix, struct dm_lksb_valblk_unit *val )
{
    printf ( "%s: stream_id=%d owner=%X, shut=%d, cnxreq=%d, willwrt=%d, wkreq=%d\n",
	prefix, val->flags.bit.stream_id, val->pid, val->flags.bit.shutdown,
	val->flags.bit.connect_request, val->flags.bit.will_write,
	val->flags.bit.wake_request );
    printf ( "              peer_ack=%d, peer_nak=%d\n", val->flags.bit.peer_ack,
	val->flags.bit.peer_nak );
}

int main ( int argc, char **argv )
{
    struct dm_lock lock;

    int status, min_size, i, flags, stream_id, actual_size, pid;
    struct commbuf *buf;

    if ( argc < 2 ) {
	printf ( "Usage: view_pipe device-name\n" );
	return 0;
    }
    memset ( &lock, 0, sizeof(struct dm_lock) );
    status = create_lock ( "DMPIPE_", argv[1], &lock );
    printf ( "Status of lock %s create: %d/%d, id=%X\n", lock.resnam,
	status, lock.lksb.status, lock.lock_id );
    show_valblk_unit ( "  val[0]", &lock.lksb.val[0] );
    show_valblk_unit ( "  val[1]", &lock.lksb.val[1] );

    for ( i = 0; i < 2; i++ ) if ( lock.lksb.val[i].flags.bit.stream_id ) {
	stream_id = lock.lksb.val[i].flags.bit.stream_id;
	status = sys_crmpsc_gpfile ( &lock, stream_id, 
		DMPIPE_MEMSTREAM_BLK_SIZE, 0, (void **) &buf, &actual_size );
	printf ( "Status of crmpsc of %s.%d: %d, buf: %x\n",  lock.resnam, 
		stream_id, status, buf );
        if ( (status&1) == 0 ) break;

	printf ( "   buf->version = %d,%d, writer: %X, reader: %X, seq: %d\n",
	    buf->fmt_version, buf->ipc_version, buf->writer_pid, 
	    buf->reader_pid, buf->sequence );
	printf ( "   buf->lock: flag=%d, owner=%d\n", buf->lock.state.flag,
		buf->lock.state.owner );

	printf ( "   buf->data_limit=%d, state=%d, write_pos=%d, read_pos=%d\n",
		buf->data_limit, buf->state, buf->write_pos, buf->read_pos );
    }

    return 1;
}
