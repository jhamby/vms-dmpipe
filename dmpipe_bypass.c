/*
 * Functions to support the I/O bypass feature of DMPIPE.  Determine if
 * a file descriptor is open on a pipe.
 *
 * Author:   David Jones
 * Date:     19-MAR-2014
 * Revised:  23-MAR-2014	Redesign to allow multiple opens on different 
 *				FDs.
 * Revised:   8-APR-2014	Manage memory address blocks to map to
 *				global sections.
 * Revised:  14-APR-2014	Fix bug in stream-id allocation when negoiating
 *				bypass for bi-directional communication.
 * Revised:  15-APR-2014	Add Support for stderr re-direction.  Allows
 *				child of popen() call to inherit parent's
 *				stderr instead of pipe open as sys$output.
 *				Add min_bytes argument to dm_bypass_read.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stat.h>
#include <string.h>
#include <ctype.h>
#include <unixio.h>
#include <fcntl.h>
#include <errno.h>

#include "dmpipe_bypass.h"
#include "memstream.h"

#include <descrip.h>		/* VMS string descriptors */
#include <starlet.h>
#include <efndef.h>		/* VMS event flag numbers */
#include <ssdef.h>
#include <lckdef.h>		/* VMS lock manager */
#include <lib$routines.h>
#include <jpidef.h>
#include <iodef.h>
#include <dvidef.h>
#include <devdef.h>		/* device characteristics */
#include <dcdef.h>		/* VMS device class numbers */
#include <secdef.h>		/* VMS global sections. */
#include <vadef.h>		/* VMS virtual address space definitions */

#define DMPIPE_ACE_ID 31814     /* ID code for application ACEs */
#define DM_SECVER_MAJOR 1
#define DM_SECVER_MINOR 1
#define DMPIPE_MEMSTREAM_BLK_SIZE 0x10000	/* 64K */
#define DM_NEXUS_NAME_SIZE 32
/*
 * Lock value block is 2 quadword structures.  The 2 comminucating
 * processes claim either one is any order.
 */
static FILE *tty = 0;
static char *tt_logical = "DBG$OUTPUT:";
#define TTYPRINT if(!tty) tty=fopen(tt_logical,"w"); fprintf(tty,
static char tty_name[256];
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
/*
 * Lock status block.  Total size and layout of first quadword is dictated
 * by $ENQ requirements.
 */
struct dm_lksb {
    unsigned short int status;	
    unsigned short int reserved;
    unsigned int id;

    struct dm_lksb_valblk_unit val[2];	/* lock value block */
};

struct dm_lock {
   char resnam[DM_NEXUS_NAME_SIZE];	/* lock name */
   short state;				/* state currently held */
   short target_state;			/* state converting to */
   int ef;
   unsigned int lock_id;		/* copy of lksb.id */
   unsigned int parent_id;		/* lock ID of parent (0) */
   int stream_id;
   int wait_timed_out;
   struct dm_lksb_valblk_unit *my_val;
   struct dm_lksb_valblk_unit *peer_val;
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
 * Nexus blocks control a single pair of streams (read/write) for each
 * mailbox being bypassed.  This permits stdout, stderr, to be open
 * on different file descriptors but still access the same write stream.
 */
struct dm_stream_data {
    struct dm_stream_data *next;/* for free list */
    int size;			/* size of block, integral number of pages */
    void *blk;			/* Address of shared memory */
    struct memstream_stats stats;
};
struct dm_nexus {
    struct dm_nexus *next;	/* global list for finding */

    pid_t self;			/* Current process's PID */
    pid_t parent; 
    int ref_count;		/* number of contexts using */
    int startup_hints;		/* startup negotiation flags */
    unsigned short dtype, chan;	/* Channel assigned to device */
    struct dm_devinfo dvi;	/* device info */
    struct dm_lock lock;	/* enqueued lock */
    struct {			/* accessed at AST level */
	int func;		/* QIO function code */
	int is_blocking_lock;
	int mailbox_active;
	struct { unsigned short status, count; pid_t pid; } iosb;
    } mbx_watch;

    struct dm_stream_data *rstream_mem, *wstream_mem;

    memstream rstream;		/* stream for reading */
    memstream wstream;		/* stream for writing */
    char name[DM_NEXUS_NAME_SIZE];	/* zero-terminated string */
};

/*
 * Top level context structure for bypass.
 */
struct dm_bypass_ctx {
    int related_fd;		/* File descriptor opened on bypassed device */

    struct dm_nexus *nexus;

    int used;
    char buffer[DM_BYPASS_BUFSIZE];	/* buffer for stdio operations */
};
/*
 * Allocate and free stream data handles.
 */
static struct dm_stream_data *free_sdata = 0;

static struct dm_stream_data *alloc_stream_data ( int blk_size )
{
    struct dm_stream_data *sdata;

    if ( free_sdata && (free_sdata->size >= blk_size) ) {
	sdata = free_sdata;
	free_sdata = sdata->next;
	sdata->next = 0;
    } else {
	sdata = calloc ( sizeof(struct dm_stream_data), 1 );
	if ( sdata ) sdata->size = blk_size;
    }
    return sdata;
}
static int free_stream_data ( struct dm_stream_data *sdata, int unmap )
{
    int status;
    /*
     * Delete virtual memory addresses mapping global section used by stream.
     */
    if ( unmap ) {
	unsigned long long region_id, start_va, va_size, del_va, del_cnt;

	region_id = VA$C_P0;
	start_va = (unsigned long long) sdata->blk;
	va_size = sdata->size;

	status = SYS$DELTVA_64 ( &region_id, start_va, va_size, 0,
		&del_va, &del_cnt );
    } else {
	sdata->blk = 0;
	status = 1;
    }
    /*
     * Place sdata block on free list so it, along with address range
     * can be reused.
     */
    sdata->next = free_sdata;
    free_sdata = sdata;

    return status;
}
/**************************************************************************/
/* Wrapper for $CRMPSC call to create a page file section.
 */
static int sys_crmpsc_gpfile ( struct dm_lock *lock,
	int flags, struct dm_stream_data *sdata )
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
    unsigned long long section_size, region_id, offset, sect_va, sect_length;
    unsigned long long sect_va_in;
    int i, status, prot;
    /*
     * Create global section from lock and and sub_id in lock->lksb.
     */
    if ( strlen ( lock->resnam ) > (sizeof(sect_name)-8) ) return SS$_BADPARAM;
    sprintf ( sect_name, "%s.%d", lock->resnam, lock->stream_id );
    sect_name_dx.dsc$w_length = strlen ( sect_name );
    /*
     * Setup the various parameters for this page file section:
     *    sect_id   - Version 1.1, match all.
     *    prot      - Access by system and owner only.
     *    region_id - Map pages in P0 (low 30 bits).
     *    flags     - Section is demand zero, shared, writable, at 
     *		      new address (sdata->blk==0) or existing address.
     */
    sect_id.match = SEC$K_MATALL;
    sect_id.ver.minor = DM_SECVER_MAJOR;
    sect_id.ver.major = DM_SECVER_MINOR;
    prot = 0xff00;		/* system and owner access only */
    region_id = VA$C_P0;	/* program region 0 (low 30 bits) */
    sect_va_in = (unsigned long long) sdata->blk;

    flags |= (SEC$M_DZRO|SEC$M_GBL|SEC$M_PAGFIL|SEC$M_WRT);
    if ( sect_va_in == 0 ) flags |= SEC$M_EXPREG;

    section_size = sdata->size;
    /*
     * Create section.  We hold a lock that any other people trying to
     * create this section should acquire, so there should be no race
     * to initialize it.
     */
    status = SYS$CRMPSC_GPFILE_64 ( &sect_name_dx, &sect_id, prot, 
	section_size, &region_id, 0, 0, flags, &sect_va, &sect_length, 
	sect_va_in, 0 );
    if ( (status&1) == 0 ) printf ( "/bypass/ crmpsc fail: %d '%s'\n",
		status, lock->resnam );
    if ( (status&1) == 0 ) return status;
    if ( (sect_va & 0x7fffffff) != sect_va ) {
	/* Bugcheck, returned address is outside range for 32-bit pointers */
	return SS$_PAGNOTINREG;
    }
    /*
     * Return address and size of created section to caller.
     */
    sdata->size = sect_length;
    sdata->blk = (void *) sect_va;		/* change pointer size */
    return status;
}
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
	struct dm_lock *lock, pid_t pid )
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
	lock->resnam[resnam_len++] = core_name[core_len];
    }
    lock->resnam[resnam_len] = 0;	/* terminate string */
    lock->ef = EFN$C_ENF;  		/* non event flag */
    /*
     * Get initial lock in PW mode and claim one of the slots.
     * Set pointers into lock value block for readability.
     */
    status = sys_enq ( LCK$K_PWMODE, lock, 0, 0, 0 );
    if ( (status & 1) == 0 ) return status;

    if ( lock->lksb.val[0].pid == 0 ) {
	lock->my_val = &lock->lksb.val[0];
	lock->peer_val = &lock->lksb.val[1];
    } else if ( lock->lksb.val[1].pid == 0 ) {
	lock->my_val = &lock->lksb.val[1];
	lock->peer_val = &lock->lksb.val[0];
    } else {
	    /*
	     * No free slots, check for ourselves.
	     */
	lock->my_val = &lock->lksb.val[1];
	lock->peer_val = &lock->lksb.val[0];
    }
    lock->my_val->pid = pid;
    lock->my_val->flags.mask = 0;
    /*
     * Lower lock to write back updaetd value block.
     */
    status = sys_enq ( LCK$K_CRMODE, lock, 0, 0, 0 );

    return status;
}
/*
 * Wake peer if its value block entry requests a wake, resetting the flag.
 */
static void wake_peer_if_waiting ( struct dm_lksb_valblk_unit *peer_val )
{
    if ( peer_val->flags.bit.wake_request && peer_val->pid ) {
        peer_val->flags.bit.wake_request = 0;
        SYS$WAKE ( &peer_val->pid, 0 );
    }
}
/*
 * Set wake_request bit and wait for peer.
 */
static void wait_for_peer_timeout ( struct dm_lock *lock )
{
    lock->wait_timed_out = 1;
    SYS$WAKE ( 0, 0 );
}
static int wait_for_peer ( struct dm_lock *lock, int timeout_sec )
{
    struct dm_lksb_valblk_unit *my_val;
    long long timeout_delta;
    int status, timed_out;

    my_val = lock->my_val;
    lock->wait_timed_out = 0;
    if ( timeout_sec > 0 ) {
	timeout_delta = -10000000;	/* 100-nsec ticks/second */
	timeout_delta *= timeout_sec;
	lock->wait_timed_out = 0;
	status = SYS$SETIMR ( EFN$C_ENF, &timeout_delta, 
		wait_for_peer_timeout, lock, 0 );
    }
    my_val->flags.bit.wake_request = 1;
    do {
	status = sys_enq ( LCK$K_CRMODE, lock, 0, 0, 0 );
	if ( (status&1) == 0 ) break;
	SYS$HIBER();
	status = sys_enq(LCK$K_PWMODE, lock, LCK$M_QUECVT|LCK$M_SYNCSTS, 0, 0);
    } while ( (status&1) && 
	!lock->wait_timed_out && my_val->flags.bit.wake_request );

    if ( timeout_sec > 0 ) {
	/* Clean up timer and check if it fired */
	SYS$CANTIM ( lock, 0 );
	SYS$SETAST(0);
	timed_out = lock->wait_timed_out;
	SYS$SETAST(1);
	if ( timed_out ) {
	    lock->peer_val->pid = 0;
	}
    }
    return status;
}

/****************************************************************************/
/* Manage dm_nexus list.
 *    find_nexus()		Find nexus block for device, creating new one
 *				if not found (reference count is zero).
 *
 *    unlink_nexus()		Decrement reference count and delete when
 *			 	reaches zero.
 *
 *    delete_nexus()		Free resources used by nexus and delete.
 *
 * Miscellaneous:
 *    deassign_nexus_chan()
 */
static struct {
    struct dm_nexus *first, *last;
    struct dm_nexus *free;
} nexus_list = { 0, 0, 0 };

static struct dm_nexus *find_nexus ( char *device_name, 
	struct dm_devinfo *info_if, unsigned short chan )
{
    struct dm_nexus *nexus;
    int status, code;
    /*
     * Walk list looking for match.  Return null if not found and no info_if.
     */
    for ( nexus = nexus_list.first; nexus; nexus = nexus->next ) {
	if ( strcmp ( nexus->name, device_name ) == 0 ) return nexus;
    }
    if ( !info_if ) return nexus;
    /*
     * Create a new nexus block for the supplied device name.
     */
    if ( nexus_list.free ) {
	nexus = nexus_list.free;
	nexus_list.free = nexus->next;
	memset ( nexus, 0, sizeof(struct dm_nexus) );
    } else {
        nexus = calloc ( sizeof (struct dm_nexus), 1 );
    }
    if ( !nexus ) return 0;
    strcpy ( nexus->name, device_name );
    nexus->dvi = *info_if;
    nexus->dtype = info_if->devtype;
    nexus->chan = chan;
    /*
     * Get lock used to negotiate use of bypass.
     */
    code = JPI$_OWNER;
    nexus->self = 0;
    status = LIB$GETJPI ( &code, &nexus->self, 0, &nexus->parent, 0, 0 );
    if ( (status&1) == 0 ) { free ( nexus ); return 0; }

    status = create_lock ( "DMPIPE_", device_name, &nexus->lock, nexus->self );
    if ( (status&1) == 0 ) {
	free ( nexus );
	return 0;
    }
    /*
     * Append to end of list.
     */
    if ( !nexus_list.first ) nexus_list.first = nexus;
    else nexus_list.last->next = nexus;
    nexus_list.last = nexus;
    return nexus;
}

static void deassign_nexus_chan ( struct dm_nexus *nexus )
{
    int status;
    if ( nexus->chan ) {
        status = SYS$DASSGN ( nexus->chan );
	nexus->chan = 0;
    }
}

static int delete_nexus ( struct dm_nexus *nexus )
{
    int status;
    unsigned long long region_id, start_va, va_size, del_va, del_cnt;
    if ( nexus->lock.state != LCK$K_NLMODE && nexus->lock.lksb.id ) {
	sys_enq ( LCK$K_PWMODE, &nexus->lock, 0, 0, 0 );
	nexus->lock.my_val->flags.bit.shutdown = 1;
	wake_peer_if_waiting ( nexus->lock.peer_val );
	if ( nexus->lock.lksb.id ) status = SYS$DEQ ( nexus->lock.lksb.id, 
		&nexus->lock.lksb.val,	/* value block inside lksb */
		0, 0 );
	nexus->lock.state = LCK$K_NLMODE;
	nexus->lock.lksb.id = 0;
    }
    if ( nexus->rstream ) {
	memstream_close ( nexus->rstream );
	status = free_stream_data ( nexus->rstream_mem, 1 );
	if ( (status&1) == 0 ) fprintf(stderr, 
		"deltva error on rstream: %d\n", status );
    }
    if ( nexus->wstream ) {
	memstream_close ( nexus->wstream );
	status = free_stream_data ( nexus->wstream_mem, 1 );
	if ( (status&1) == 0 ) fprintf(stderr, 
		"deltva error on wstream: %d\n", status );
    }
    deassign_nexus_chan ( nexus );

    return 0;
}


static int unlink_nexus ( struct dm_nexus *nexus )
{
    struct dm_nexus *prev, *cur;
    /*
     * Find on list.
     */
    prev = 0;
    for ( cur = nexus_list.first; cur; cur = cur->next ) {
	if ( cur == nexus ) {
	    /* Found block, delete if last reference. */
	    cur->ref_count--;
	    if ( cur->ref_count > 0 ) return 0;

	    if ( prev ) prev->next = cur->next;
	    else nexus_list.first = cur->next;
	    cur->next = 0;
	    nexus->next = nexus_list.free;
	    nexus_list.free = nexus;

	    return delete_nexus ( nexus );
	}
	prev = cur;
    }
    return 0;
}

/**************************************************************************/
/* Assign a channel to device open on file desc. and get device information via 
 * SYS$GETDVI: devnam, devclass, devtype, devchar, bufsize.
 *
 * Information is only returned for files opened on record-oriented
 * devices (character special), otherwise SS$_DEVNOTMBX returned.
 * This prevents potential problems with files open on network links (st_dev
 * in that case is a logical that may refer to a local device).
 */
static int get_device_chan_and_info ( int fd, char
	device_name[DM_NEXUS_NAME_SIZE],
	unsigned short *chan, struct dm_devinfo *info, int skip )
{
    struct stat finfo;
    static $DESCRIPTOR(st_dev_dx,"");
    unsigned short devnam_len;
    int status, required_devchar;
    struct { unsigned short status, count; long ext; } iosb;
    struct {
	unsigned short length, code;
	void *buffer;
	unsigned short *retlen;
    } item[] = {
	{ 31, DVI$_DEVNAM, device_name, &devnam_len },
	{ sizeof(info->devclass), DVI$_DEVCLASS, &info->devclass, 0 }, 
	{ sizeof(info->devtype), DVI$_DEVTYPE, &info->devtype, 0 },
	{ sizeof(info->devchar), DVI$_DEVCHAR, &info->devchar, 0 }, 
	{ sizeof(info->bufsize), DVI$_DEVBUFSIZ, &info->bufsize, 0 },
	{ sizeof(info->owner), DVI$_PID, &info->owner, 0 },
	{ sizeof(info->refcnt), DVI$_REFCNT, &info->refcnt, 0 },
	{ 0, 0, 0, 0 }
    };
    if ( skip == 0 ) {
	memset ( info, 0, sizeof(struct dm_devinfo) );
	info->devclass = 0;
        info->devtype = 0;
	device_name[0] = 0;
	*chan = 0;
	/*
	 * Have CRTL tell us device name, which may be device alone or
	 * include hostname and/or underscores.
	 */
	if ( fstat ( fd, &finfo ) < 0 ) return SS$_BADPARAM;
	st_dev_dx.dsc$a_pointer = finfo.st_dev;
	st_dev_dx.dsc$w_length = strlen ( finfo.st_dev );
	/*
 	 * Assign channel.  If fd open on a network link, device name
         * is invalid.  ($define staff$disk mba0: may cause mischief).
     	 */
	status = SYS$ASSIGN ( &st_dev_dx, chan, 0, 0, 0 );
	if ( (status&1) == 0 ) { *chan = 0; return status; }
    } else {
	if ( *chan == 0 ) return SS$_DEVNOTMBX;
    }
    /*
     * Make GETDVI call to get uniform device name as well set device
     * type and characteristics.
     */
    status = SYS$GETDVIW(EFN$C_ENF, *chan, 0, &item[skip], &iosb, 0, 0, 0, 0);
    if ( status & 1 ) status = iosb.status;
    if ( (status&1) == 0 ) {
	printf ( "/bypass/ getdvi err on '%s': %d\n", finfo.st_dev,status);
	return status;
    }
    if ( skip == 0 ) device_name[devnam_len] = 0;	/* terminate string */
    /*
     * If device class unknown, force a mailbox type if it appears to
     * be the pipe driver used by DCL.
     */
#ifdef DEBUG
if ( !tty ) tty = fopen ( tt_logical, "w" );
    fprintf (tty, "/bypass/ fd[%d] info: nam=%s, class=%d/%d, refcnt=%d/%x (%s)\n",
	fd, device_name, info->devclass, info->devtype, info->refcnt,
	info->owner, getname(fileno(tty),tty_name) );
#endif

    required_devchar = DEV$M_REC | DEV$M_AVL | DEV$M_IDV | DEV$M_ODV;
    if ( (info->devclass == 0) && (info->devtype == 0) &&
	((info->devchar & required_devchar) == required_devchar) ) {
	char *devnam;
	for ( devnam = device_name; *devnam == '_'; devnam++ );
	if ( strncmp ( devnam, "MPA", 3 ) == 0 ) {
#ifdef DEBUG
	    fprintf ( tty,"/bypass/ deeming %s a mailbox...\n", devnam );
#endif
	    info->devclass = DC$_MAILBOX;
	    info->devtype = DT$_PIPE;
	}
    }
    return status;
}
/**************************************************************************/
/*
 * Callback function for C$DOPRINT family of routines.
 */
int dm_bypass_doprint_cb ( dm_bypass bp, void *buffer, int len )
{
    return -1;
}
/*
 * Initialize function gets properties on file to see if it eligible for bypass.
 * If not eligible, a null pointer is returned.  If it's elibible, a bypass
 * handle (opaque pointer) is returned, though a bypass is not yet established.
 */
dm_bypass dm_bypass_init ( int fd, int *flags )
{
    int status, code, devclass, devtype, devchar, proc_index;
    dm_bypass bp;
    char device_name[DM_NEXUS_NAME_SIZE];
    unsigned short chan;
    struct dm_devinfo info;
    /*
     * Get device class, only proceed if it looks like a mailbox that we
     * can attach metadata to (via ACL).
     */
    *flags = 0;
    status = get_device_chan_and_info (fd, device_name, &chan, &info, 0 );
    if ( (status&1) == 0 ) return 0;		/* error */

    if ( ((status&1) == 0) || (info.devclass != DC$_MAILBOX) ) {
	if ( chan ) SYS$DASSGN ( chan );
	return 0;
    }
    /*
     * Allocate context and initialize.
     */
    bp = calloc ( sizeof(struct dm_bypass_ctx), 1 );
    if ( !bp ) return 0;
    bp->related_fd = fd;
    /*
     * Create existing nexus or create new one.  Remove redundant channel.
     */
    bp->nexus = find_nexus ( device_name, &info, chan );
    if ( !bp->nexus ) {
	if ( chan ) SYS$DASSGN ( chan );
	*flags = 0;
	free ( bp );
	return 0;
    }
    bp->nexus->ref_count++;
    if ( chan != bp->nexus->chan ) SYS$DASSGN ( chan );
    *flags = 1;			/* allow negotiation */

    return bp;
}
static int begin_stream ( int flags, int is_writer, struct dm_nexus *nexus,
	int fcntl_flags )
{
    int status, blk_size, stream_flags;
    void *blk;
    memstream stream;
    struct dm_stream_data *sdata;
    /*
     * Create memory section.
     */
    sdata = alloc_stream_data ( DMPIPE_MEMSTREAM_BLK_SIZE );
    if ( sdata ) status = sys_crmpsc_gpfile ( &nexus->lock, 0, sdata );
    else return (flags&0xfffe);		/* allocation failure */

    if ( status & 1 ) {
	/*
	 * Initilize uni-directional stream.
         */
	stream = memstream_create ( sdata->blk, sdata->size, is_writer );
	if ( !stream ) return (flags&0xfffe);   /* disable negotiations */
	if ( fcntl_flags & O_NONBLOCK ) {
	    stream_flags = MEMSTREAM_ATTR_NONBLOCK;
	    memstream_control ( stream, &stream_flags, 0 );
	}

	if ( is_writer ) {
	    memstream_assign_statistics ( stream, &sdata->stats );
	    nexus->wstream_mem = sdata;
	    nexus->wstream = stream;
	    flags |= 4;			/* start bypassing device */

	} else {
	    memstream_assign_statistics ( stream, &sdata->stats );
	    nexus->rstream_mem = sdata;
	    nexus->rstream = stream;
	    flags |= 2;			/* start bypassing */
	}
    } else return (flags&0xfffe);

    return flags;
}
/*
 * The negotiate_bypass function is called while lock is held in PW mode, 
 * any changes to the lock value block will be written back and thus 
 * communicated to the partner process when it elevates its lock on
 * lock->resnam.
 */
static char *format_valblk ( struct dm_lksb_valblk_unit *val, int bufndx )
{
    static struct { char s[80]; } buffer[4];
    sprintf ( buffer[bufndx].s, "{pid: %x s:%d c:%d%d %d p:%d%d id:%d}",
	val->pid, val->flags.bit.shutdown, val->flags.bit.connect_request,
	val->flags.bit.will_write, val->flags.bit.wake_request,
	val->flags.bit.peer_ack, val->flags.bit.peer_nak,
	val->flags.bit.stream_id );
    return buffer[bufndx].s;
}
static int negotiate_bypass ( int flags, dm_bypass bp, const char *op,
	int fcntl_flags )
{
    struct dm_lock *lock;
    struct dm_lksb *lksb;
    struct dm_nexus *nexus;
    struct dm_lksb_valblk_unit *my_val, *peer_val;
    int status, action, will_write;

    nexus = bp->nexus;
    lock = &nexus->lock;
    lksb = &lock->lksb;
    my_val = lock->my_val;
    peer_val = lock->peer_val;
    /*
     * Analyze state of flag bits and determine action:
     *    1 - make new request to start stream in my_val;
     *    2 - accept existing stream create request by peer.
     *    3 - accept existing stream request, make new stream request.
     *    4 - negotiation failed.
     */
    action = 0;
    will_write = (*op == 'w') ? 1 : 0;
    /*
     * Prospectively create shared memory block.
     */
    if ( peer_val->pid == 0 ) {
	action = 1;
    } else if ( peer_val->flags.bit.shutdown ) {
	action = 4;
    } else if ( peer_val->flags.bit.connect_request ) {
	/*
	 * We will accept request, but it may be for a stream going
	 * the other way so a request of our own may be needed.
	 */
	action = 2;
	if ( will_write == peer_val->flags.bit.will_write ) action = 3;
    } else {
	action = 1;
    }
#ifdef DEBUG
    TTYPRINT "/bypass/ proc %x bypass negotiate action: %d, streams: %d %d\n",
	my_val->pid, action, my_val->flags.bit.stream_id,
	peer_val->flags.bit.stream_id );
#endif
    /*
     * Execute actions.  Accept peer's request before making our own.
     */
    if ( action & 4 ) {
	my_val->flags.bit.shutdown = 1;
	peer_val->flags.bit.peer_nak = 1;
    } else if ( action & 2 ) {
	if ( (peer_val->flags.bit.will_write && nexus->rstream) ||
	     (!peer_val->flags.bit.will_write && nexus->wstream) ) {
	    /* already have the stream */
	    peer_val->flags.bit.peer_ack = 1;

	} else {
	    /*
	     * Create stream to complement peer's request
	     */
	    lock->stream_id = peer_val->flags.bit.stream_id;
	    flags = begin_stream ( flags, 
		1^peer_val->flags.bit.will_write, /* flip 1->0, 0->1 */
		nexus, fcntl_flags );

	    peer_val->flags.bit.peer_ack = 1;
	}
    }
    if ( action & 1 ) {
	/*
	 * Prospectively create a stream and solicit a peer.
         */
	if ( my_val->flags.bit.stream_id < peer_val->flags.bit.stream_id )
	    my_val->flags.bit.stream_id = peer_val->flags.bit.stream_id+1;
	else my_val->flags.bit.stream_id++;
	lock->stream_id = my_val->flags.bit.stream_id;

	flags = begin_stream ( flags, will_write, nexus, fcntl_flags );
	my_val->flags.bit.connect_request = 1;
	my_val->flags.bit.will_write = will_write;
	/*
	 * Stall to let peer show up and/or answer request.
	 */
	wake_peer_if_waiting ( peer_val );
	status = wait_for_peer ( lock, 10 );	/* 10 second timeout */

	if ( (peer_val->pid == 0) || peer_val->flags.bit.shutdown ) {
	    /* nobody showed up */
	    my_val->flags.bit.shutdown = 1;
	    status = sys_enq ( LCK$K_CRMODE, lock, 0, 0, 0 );
	    return 0;
	}

	if ( my_val->flags.bit.peer_nak ) {
	     /* partner had trouble setting up stream */
	     flags = flags &0x7ff8;
	} else if ( !my_val->flags.bit.peer_ack ) {
	     /* solitication was neither acked nor naked! */
	     flags = flags &0x7ffa;
	}

    } else if ( action == 0 ) {
	/*
	 * Unexpected mode.
	 */
	flags = flags&0x7ffe;
	my_val->flags.bit.shutdown = 1;
	peer_val->flags.bit.peer_nak = 1;
    }
    wake_peer_if_waiting ( peer_val );

    sys_enq ( LCK$K_CRMODE, lock, 0, 0, 0 );

    return flags;
}
/*
 * Get name of current stderr file and put in logical name DMPIPE_STDERR_'pid'
 * Return value is VMS condition code.
 */
static char *stderr_filename ( char buffer[256] )
{
    char *rtl_errfile;
    int status;
    unsigned short int length, equivlen;
    static char logname[40];
    static char equiv[256];
    static $DESCRIPTOR(logname_dx,"");
    static $DESCRIPTOR(logvalue_dx,equiv);
    /*
     * Get the name the CRTL is using and see if it matches SYS$ERROR,
     * returning getname() result to caller if not.
     */
    rtl_errfile = getname(2,buffer);
    if ( !rtl_errfile ) return 0;
    length = strcspn ( rtl_errfile, ":.;" );
    if ( (length!=9) || strncasecmp(rtl_errfile,"SYS$ERROR",9) )
	return buffer;
    /*
     * Translate sys$error since this logical isn't inherited by child.
     */
    logname_dx.dsc$a_pointer = buffer;
    logname_dx.dsc$w_length = length;
    logvalue_dx.dsc$w_length = sizeof(equiv)-1;

    status = LIB$GET_LOGICAL ( &logname_dx, &logvalue_dx, &equivlen );
#ifdef DEBUG
fprintf(tty,"/bypass/ status of SYS$ERROR translate %d, l=%d, [0..4] = %x %x %x %d\n", 
status,equivlen, (unsigned) equiv[0], (unsigned) equiv[1], (unsigned) equiv[2], (unsigned) equiv[3] );
#endif
    if ( (status&1) == 0 ) return buffer;
    equiv[equivlen] = 0;	/* terminate string */
    /*
     * Strip process permanent file info. (do later).
     */
    if ( (equiv[0] == 27) && (equiv[1] == 0) && (equivlen >= 4) ) {
	strcpy ( buffer, &equiv[4] );
    } else {
       strcpy ( buffer, equiv );
    }
    return buffer;
}

int dm_bypass_stderr_propagate ( void )
{
    int code, status, proc_index;
    pid_t self;
    static char logname[40];
    static char errfile[256];
    static $DESCRIPTOR(logname_dx,logname);
    static $DESCRIPTOR(logvalue_dx,errfile);
#ifdef DEBUG
    if ( !tty ) tty = fopen ( tt_logical, "w" );
#endif
    code = JPI$_PROC_INDEX;
    self = 0;
    status = LIB$GETJPI ( &code, &self, 0, &proc_index, 0, 0 );
    if ( status & 1 ) {
	/*
	 * Construct logical name and get current stderr filename as
	 * equivalence name.
	 */
	sprintf ( logname, "DMPIPE_STDERR_%08X", self );
	logname_dx.dsc$w_length = strlen ( logname );

	logvalue_dx.dsc$a_pointer = stderr_filename ( errfile );
	if ( !logvalue_dx.dsc$a_pointer ) return SS$_ABORT;

	logvalue_dx.dsc$w_length = strlen ( logvalue_dx.dsc$a_pointer );
	if ( logvalue_dx.dsc$w_length == 0 ) return SS$_ABORT;
	/*
	 * Create logical name that will be inherited by child.
	 */
	status = LIB$SET_LOGICAL ( &logname_dx, &logvalue_dx, 0, 0, 0 );
#ifdef DEBUG
	fprintf (tty, "/bypass/ fd[stderr] set logical %s status: %d (%s)\n",
		logname, status, errfile );
#endif
	if ( (status&1) == 0 ) return status;
	/*
	 * Force stderr to shared access.  Using empty string reuses old name.
	 */
	stderr = freopen ( getname(2,errfile), "a+", stderr, "shr=put,upi" ); 
#ifdef DEBUG
	fprintf(tty, "/bypass/ parent freopen result: %x\n", stderr );
    } else {
	fprintf ( tty, "/bypass/ getjpi error in propagate(): %d\n", status );
#endif
    }
    return status;
}

int dm_bypass_stderr_recover ( pid_t parent )
{
    static char logname[40];
    static char errfile[256];
    static $DESCRIPTOR(logname_dx,logname);
    static $DESCRIPTOR(logvalue_dx,errfile);
    static int recovered = 0;
    int status;
    short int length, length2;

    if ( recovered ) return 1;		/* already redirected */
    recovered = 1;
    /*
     * Construct logical name.
     */
#ifdef DEBUG
    if ( !tty ) tty = fopen ( tt_logical, "w" );
#endif
    sprintf ( logname, "DMPIPE_STDERR_%08X", parent );
    logname_dx.dsc$w_length = strlen ( logname );
    logvalue_dx.dsc$w_length = sizeof(errfile)-1;
    /*
     * See if parent used popen.  Should really only check process table.
     */
    status = LIB$GET_LOGICAL ( &logname_dx, &logvalue_dx, &length );
    if ( status & 1 ) {
	/*
         * Reopen stderr.
	 */
	char old_errfile[256], tmp[256];
	int i;
	for ( i = 0; i < length; i++ ) 
	    if ( !isprint(errfile[i]) ) tmp[i] = '.'; else tmp[i]=errfile[i];
	tmp[i] = 0;
#ifdef DEBUG
	fprintf(tty, "/bypass/ stderr of parent: '%s'\n", tmp );
#endif

	errfile[length] = '\0';
	stderr = freopen ( errfile, "a+", stderr, "shr=upi,put" );
	if ( !stderr ) {
#ifdef DEBUG
	fprintf(tty,"/bypass/ freopen error: %d/%d = '%s'\n", errno, vaxc$errno,
		strerror(errno) );
#endif
	}
#ifdef DEBUG
	fprintf (tty, "/bypass/ redirecting stderr result=%x\n", stderr );
#endif
    } else status = 1;
    return status;
}
/************************************************************************/
/* Master routine to perform handshake with peer via lock manager.
 */
static void stall_mbx_ast ( void *bp_vp )
{
    dm_bypass bp;
    bp = bp_vp;
    bp->nexus->mbx_watch.mailbox_active = 1;
    SYS$WAKE ( 0, 0 );   /* wake ouselves */
}
static void stall_lock_ast ( void *bp_vp )
{
    dm_bypass bp;
    bp = bp_vp;
    bp->nexus->mbx_watch.is_blocking_lock = 1;
    SYS$WAKE ( 0, 0 );   /* wake ouselves */
}
int dm_bypass_startup_stall ( int initial_flags, dm_bypass bp, char *op,
	int fcntl_flags )
{
    int flags, status;
    struct dm_nexus *nexus;
    struct dm_lksb_valblk_unit *my_val, *peer_val;
    /*
     * Bail out if we've stopped negotiating and cleared bit 0 in flags.
     */
    flags = initial_flags;
    if ( (flags & 1) == 0 ) return flags;	/* negotiation over */
    if ( (flags & 2) && (*op == 'r') ) return flags;
    if ( (flags & 4) && (*op == 'w') ) return flags;
    /*
     * If we already have the corresponding stream, we are done.
     */
    nexus = bp->nexus;
    my_val = nexus->lock.my_val;
    peer_val = nexus->lock.peer_val;
    if ( my_val->flags.bit.shutdown || peer_val->flags.bit.shutdown ) {
	return 0;
    }

    if ( (*op == 'w') && (nexus->wstream) ) {
	flags |= 4;
	if ( flags == 7 ) flags = 6;
	return flags;
    } else if ( nexus->rstream ) {
	flags |= 2;
	if ( flags == 7 ) flags = 6;
	return flags;
    }
    /*
     * Method of stall varies with device type.
     */
#ifdef DEBUG
if ( !tty ) tty = fopen ( tt_logical, "w" );
   fprintf (tty, "/bypass/ %x-fd[%d] STARTUP_STALL, initial op '%s'\n",
		bp->nexus->self, bp->related_fd, op  );
#endif
    if ( (nexus->dtype == DT$_MBX) || (nexus->dtype== DT$_PIPE) ) {
	/*
	 * Raise lock to retrieve current lock value block.
	 */
	nexus->mbx_watch.is_blocking_lock = 0;
	nexus->mbx_watch.mailbox_active = 0;
	status = sys_enq ( LCK$K_PWMODE, &bp->nexus->lock, LCK$M_NOQUEUE,
		bp, stall_lock_ast );

	if ( (status&1) == 0 ) {
	    /* $ENQ failed, stall loop below will exit immediately */

	} else if ( peer_val->pid ) {
	   /*
	    * Presence of peer in value block means we can negotiate.
	    * No longer need a channel assigned to the mailbox.
	    */
	   nexus->mbx_watch.is_blocking_lock = 1;
	   deassign_nexus_chan ( nexus );

	} else if ( nexus->dtype == DT$_MBX ) {
	   /*
	    * No peer seen in lock value block, set attention AST on mailbox.
	    */
	    nexus->mbx_watch.func = IO$_SETMODE;
	    nexus->mbx_watch.func |= (*op == 'w') ? IO$M_READATTN : IO$M_WRTATTN;

	    status = SYS$QIOW ( EFN$C_ENF, nexus->chan, nexus->mbx_watch.func,
		&nexus->mbx_watch.iosb, 0, 0, stall_mbx_ast, bp, 0, 0, 0, 0 );
 	    if ( (status&1) == 1 ) status = nexus->mbx_watch.iosb.status;
	} else if ( nexus->dtype == DT$_PIPE ) {
	    /*
	     * DCL pipe driver doesn't have attention ASTs, just stall
	     * for 200 milliseconds.
	     */
	    long long delay;
	    delay =  200 * -10000;	/* 100-nanosecond ticks */
	    status = SYS$SETIMR ( EFN$C_ENF, &delay, stall_mbx_ast, bp, 0 );
	}
	/*
	 * Stall loop.
	 */
	while ( status & 1 ) {
	    SYS$SETAST ( 0 );
	    if ( nexus->mbx_watch.is_blocking_lock ) status = 0;
	    if ( nexus->mbx_watch.mailbox_active ) status = 0;
	    SYS$SETAST ( 1 );
	    if ( status ) {
	        SYS$HIBER();
	    }
	}
#ifdef DEBUG
fprintf ( tty,"/bypass/ %x-fd[%d] wait complete(sts=%d), lock block: %d, mbx attn: %d\n",
		bp->nexus->self, bp->related_fd, status,
		nexus->mbx_watch.is_blocking_lock, nexus->mbx_watch.mailbox_active );
#endif
	/*
	 * Wait done, cleanup.  Kill pending attention/timer AST.
	 */
	if ( !nexus->mbx_watch.mailbox_active ) {
	    if ( nexus->dtype == DT$_MBX ) {
	      status = SYS$QIOW ( EFN$C_ENF, nexus->chan, nexus->mbx_watch.func,
	      		&nexus->mbx_watch.iosb, 0, 0, 0, 0, 0, 0, 0, 0 );
	    } else {
		status = SYS$CANTIM ( bp, 0 );
	    }
	} else if ( (nexus->dtype == DT$_MBX) && (*op == 'r') ) {
	    /*
	     * Assign a readonly channel to the mailbox and set new
	     * attention AST to notify us when no writers
	     */
	}

	if ( nexus->mbx_watch.is_blocking_lock ) {
	    /*
	     * Another process has lock.  setup streams if we can.
	     */
	    flags = negotiate_bypass ( flags, bp, op, fcntl_flags );
	    deassign_nexus_chan ( nexus );
	} else {
	    /*
	     * No conflicting locks seen, give up.
	     */
	    my_val->flags.bit.shutdown = 1;
	    status = sys_enq ( LCK$K_CRMODE, &bp->nexus->lock, LCK$M_NOQUEUE,
		0, 0 );
	    flags = (flags&0x7ffe);
	    deassign_nexus_chan ( nexus );
	}
    } else {
	/*
	 * Unsupported device type.
	 */
	flags = (flags&0x7ffe);
	    sys$dassgn ( nexus->chan );
	    nexus->chan = 0;
    }
    if ( (flags&1) == 0 ) {
	/* Startup done, deassign channel so CRTL closes down */
	deassign_nexus_chan ( nexus );
    }
    return flags;
}

/*
 * Cleanup resources.  The nexus block may be shared by multiple FDs,
 * the unlink operation will defer deleting the nexus until there are
 * no more references.
 */
int dm_bypass_shutdown ( dm_bypass bp )
{
    if ( bp->nexus ) unlink_nexus ( bp->nexus );
    bp->nexus = 0;

    free ( bp );
    return 1;
}
/*
 * I/O routines, mostly just pass through to memstream layer.
 */
int dm_bypass_read ( dm_bypass bp, void *buffer, size_t nbytes, size_t min_bytes )
{
    return memstream_read ( bp->nexus->rstream, buffer, nbytes, min_bytes );
}
int dm_bypass_write ( dm_bypass bp, const void *buffer, size_t nbytes )
{
    return memstream_write ( bp->nexus->wstream, buffer, nbytes );
}
/*
 * Give direct access to memstream for polling.
 */
int dm_bypass_current_streams ( dm_bypass bp, 
	memstream *rstream, memstream *wstream  )
{
    if ( bp ) {
	*rstream = bp->nexus->rstream;
	*wstream = bp->nexus->wstream;
	return 0;
    }
    return -1;   /* invalid */
}
