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
 *
 * Revised:  17-APR-2014	Add expedite_flag to dm_bypass_read.
 * Revised:  19-APR-2014	Support special popen(cmd,"r") operation, 
 *				Reopen FILE pointer on null device and let
 *				dmpipe read mailbox via QIO while checking
 *				for no writers condition.
 * Revised:  20-APR-2014	Overhaul stderr re-direction.  When stderr
 *				would redirect to file, have stderr_propagate
 *				create a mailbox, read via AST thread, that
 *				children redirect stderr to.  When not a relay
 *				mailbox, create dmpipe_stderr_xxxxxx logical
 *				name in user so it is destroyed on image exit.
 * Revised:  20-APR-2014	Do not attempt to bypass null device or output
 *				mailbox when it matches DCL$OUTPUT_xxxxx device
 *				created by DCL PIPE command.  This change and
 *				previous allow dmpipe to work in batch mode.
 * Revised:  21-APR-2014	Fix implied_lf processing in alternate_bypass.
 * Revised:  23-APR-2014	Fix pipe detection for MPA devices, previous
 *				change to using ALLDEVNAM DVI code broke it.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stat.h>
#include <string.h>
#include <ctype.h>
#include <unixio.h>
#include <unixlib.h>
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
#include <lnmdef.h>		/* VMS logical names */
#include <iodef.h>
#include <dvidef.h>
#include <devdef.h>		/* device characteristics */
#include <dcdef.h>		/* VMS device class numbers */
#include <secdef.h>		/* VMS global sections. */
#include <stsdef.h>		/* VMS condition value/status definitions */
#include <vadef.h>		/* VMS virtual address space definitions */
#include <agndef.h>
#include <cmbdef.h>

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
static char *action_name[5] = 
   { "0-null", "1-connect", "2-accept", "3-accept+connect", "4-fail" };
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
    /*
     * Alternate read section. Make a special non-memory bypass for 
     * popen(cmd,"r") that can provide proper EOF for input streams when
     * spawned process dies.
     */
    struct {
	unsigned short chan;   		/* move chan here */
	unsigned short implied_lf;	/* add LF to buffer after reads. */
	unsigned int buf_size;	/* Matches device buffer size */
	unsigned int rpos, length;	/* position in buffer */
	struct { unsigned short status, count; pid_t pid; } iosb;
	char *buffer;
    } alt;
};

/*
 * Top level context structure for bypass.
 */
struct dm_bypass_ctx {
    int related_fd;		/* File descriptor opened on bypassed device */

    struct dm_nexus *nexus;

    int is_dcl_out;
    int used;
    char buffer[DM_BYPASS_BUFSIZE];	/* buffer for stdio operations */
};

/* TRACE */
extern char dmpipe_trace;
void dmpipe_trace_output(const char *cp_format, ...);
/* END TRACE */
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
/***************************************************************************/
/* Handle reading of input mailbox from popen() in place of CRTL.  Use
 * directional feature of mailbox to detect absence of writers to mailbox.
 */
static int alternate_read_bypass_init ( struct dm_nexus *nexus )
{
    int status;
    static int nocrlf_index = -1;

    static $DESCRIPTOR(devnam_dx,"");
    /*
     * Open directional channel on mailbox.
     */
    devnam_dx.dsc$a_pointer = nexus->name;
    devnam_dx.dsc$w_length = strlen(nexus->name);
    status = SYS$ASSIGN ( &devnam_dx, &nexus->alt.chan, 0, 0, AGN$M_READONLY );
    if ( (status&1) == 0 ) return 0;	/* give up on error */
    /*
     * Allocate buffer with enough room to append carriage control.
     */
    nexus->alt.buf_size = nexus->dvi.bufsize + 2;
    nexus->alt.length = 0;		/* length of current record */
    nexus->alt.rpos = 0;		/* next read position */
    nexus->alt.buffer = malloc ( nexus->alt.buf_size );
    nexus->alt.iosb.status = 0;
    if ( !nexus->alt.buffer ) {
	SYS$DASSGN ( nexus->alt.chan );
	return 0;
    }
    /*
     * Set implied_lf flag according to POPEN_NO_CRLF_REC_ATTR feature.
     */
    nexus->alt.implied_lf = 0;
/*    status = decc$feature_get ( "DECC$POPEN_NO_CRLF_REC_ATTR", */
    status = decc$feature_get ( "DECC$STREAM_PIPE", 
	__FEATURE_MODE_CURVAL );
    if ( status == 1 ) nexus->alt.implied_lf = 0;
    else if ( status < 0 ) perror ( "feature get in alt_bypass_init" );

    return 1;
}

static int alternate_bypass_read ( struct dm_nexus *nexus, void *buf_vp,
	size_t nbytes, size_t min_bytes, int *expedite_flag )
{
    int count, status, seg, read_modifiers, mbx_eof_pid=0;
    char *buffer;
    /*
     * Move bytes from alt.buffer until min_bytes moved or an exception
     * such as EOF or no writers occurs.
     */
    buffer = buf_vp;
    *expedite_flag = 0;
    count = 0;
    do {
	seg = nexus->alt.length - nexus->alt.rpos;
	if ( seg <= 0 ) {
	    /*
	     * Refill buffer, read from mailbox checking for no writers after
	     * first read completes.
	     */
	    if ((nexus->alt.iosb.status == 0) || (nexus->alt.iosb.status != SS$_ENDOFFILE)) {
                status = SYS$QIOW ( EFN$C_ENF, nexus->alt.chan,
		                  IO$_SENSEMODE | IO$M_WRITERCHECK , &nexus->alt.iosb, 0, 0,
		                  0, 0, 0, 0, 0, 0 );
                if ($VMS_STATUS_SUCCESS(status) && $VMS_STATUS_SUCCESS(nexus->alt.iosb.status))
                   {
                   read_modifiers = nexus->alt.iosb.status ? IO$M_WRITERCHECK : 0;
	          read_modifiers = IO$M_WRITERCHECK;
	          if (!nexus->alt.implied_lf) read_modifiers |= IO$M_STREAM;
                   do
                      {
                      if (nexus->alt.iosb.status == SS$_ENDOFFILE) mbx_eof_pid = nexus->alt.iosb.pid;
	             status = SYS$QIOW ( EFN$C_ENF, nexus->alt.chan,
		                        IO$_READVBLK | read_modifiers, &nexus->alt.iosb, 0, 0,
		                        nexus->alt.buffer, nexus->alt.buf_size-2, 0, 0, 0, 0 );
		                        
		    } while ($VMS_STATUS_SUCCESS(status) && (nexus->alt.iosb.status == SS$_ENDOFFILE) &&
		             nexus->alt.iosb.pid != mbx_eof_pid);
		 }
/* TRACE */
if (!$VMS_STATUS_SUCCESS(status))
   {
/*   dmpipe_trace = 1; */
   dmpipe_trace_output("SYSQIOW in alternate read bypass returned: %d(0x%X)\r\n", status, status);
   }
else
   if (!$VMS_STATUS_SUCCESS(nexus->alt.iosb.status))
      {
/*      dmpipe_trace = 1; */
      dmpipe_trace_output("SYSQIO request in alternate_read_bypass completed with: %d(0x%X)\r\n",
                          nexus->alt.iosb.status, nexus->alt.iosb.status);
      }
/* END TRACE */
	       if ( (status&1) == 1 ) status = nexus->alt.iosb.status;
	       /*
	        * Reset rpos and length according to status of read.
	        */
	       nexus->alt.rpos = 0;
	       if ( status&1 ) {
		 /*
		  * Success, set length and append lf to end of buffer
		  */
		 nexus->alt.length = nexus->alt.iosb.count;
		 if ( nexus->alt.implied_lf ) {
		    nexus->alt.buffer[nexus->alt.iosb.count] = '\n';
		    nexus->alt.length++;
		 }

	       } else if ( status == SS$_NOWRITER ) {
	          nexus->alt.iosb.status = SS$_ENDOFFILE;
		 nexus->alt.length = 0;
		 *expedite_flag = 1;	/* force loop exit */
		 if ( count == 0 ) {
		    count = -1;
		 }
	       } else if ( status == SS$_ENDOFFILE ) {
		 /* Eat the EOF, but complete read short of min_bytes */
		 nexus->alt.length = 0;
		 *expedite_flag = 1;
	       }
	     } else {
	         nexus->alt.length = 0;
		*expedite_flag = 1;
		if ( nexus->alt.rpos != -1) {
		   nexus->alt.rpos = -1;
		   count = -1;
		}
	      else {
	           count = -1;
		  errno = EPIPE;
	         }
	     }	       

	    seg = nexus->alt.length;
	}
	if ( seg > (nbytes-count) ) seg = nbytes-count;
	/*
	 * Add seg bytes from alt.buffer to caller's buffer and update pointers.
	 */
	if ( seg > 0 ) memcpy ( &buffer[count], 
		&nexus->alt.buffer[nexus->alt.rpos], seg );
	nexus->alt.rpos += seg;
	count += seg;
    } while ( (count < min_bytes) && (*expedite_flag == 0));

    return count;
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

    if ( nexus->alt.buffer ) {
	free ( nexus->alt.buffer );
	nexus->alt.buffer = 0;
    }
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
	{ 31, DVI$_ALLDEVNAM, device_name, &devnam_len },
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
if ( !tty ) tty = fopen ( tt_logical, "a", "shr=put" );
    fprintf (tty, "/bypass/ fd[%d] info: nam=%s, class=%d/%d, bfsz=%d, refcnt=%d/%x (%s)\n",
	fd, device_name, info->devclass, info->devtype, info->bufsize, info->refcnt,
	info->owner, getname(fileno(tty),tty_name) );
#endif

    required_devchar = DEV$M_REC | DEV$M_AVL | DEV$M_IDV | DEV$M_ODV;
    if ( (info->devclass == 0) && (info->devtype == 0) &&
	((info->devchar & required_devchar) == required_devchar) ) {
	/* Strip leading underscores and allocation class/node if present */
	char *devnam, *dollar;
	dollar = strrchr ( device_name, '$' );
	if ( dollar ) devnam = dollar+1;
	else for ( devnam = device_name; *devnam == '_'; devnam++ );

	if ( strncmp ( devnam, "MPA", 3 ) == 0 ) {
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
 * If not eligible, a null pointer is returned.  If it's eligible, a bypass
 * handle (opaque pointer) is returned, though a bypass is not yet established.
 */
dm_bypass dm_bypass_init ( int fd, int *flags )
{
    int status, code, devclass, devtype, devchar, parent;
    dm_bypass bp;
    char device_name[DM_NEXUS_NAME_SIZE];
    unsigned short chan;
    struct dm_devinfo info;
    /*
     * Get device class, only proceed if it looks like a mailbox that we
     * can attach metadata to (via ACL).
     */
    *flags = 0;
    device_name[0] = '\0';
    status = get_device_chan_and_info (fd, device_name, &chan, &info, 0 );

/* TRACE */
    if (!dmpipe_trace)
       {
/*       dmpipe_trace = 1; */
       dmpipe_trace_output("fd %d device name = \"%s\"\r\n", fd, device_name);
       dmpipe_trace = 0;
       }
    else
       dmpipe_trace_output("fd %d device name = \"%s\"\r\n", fd, device_name);

/* END TRACE */
    if ( (status&1) == 0 ) return 0;		/* error */

    if ( ((status&1) == 0) || (info.devclass != DC$_MAILBOX) ) {
	if ( chan ) SYS$DASSGN ( chan );
	return 0;
    } else if ( info.devtype == DT$_NULL ) {
	/* Null device has mailbox class but can't be used */
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
     * Retrieve existing nexus or create new one.  Remove redundant channel.
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
    *flags = DM_BYPASS_HINT_STARTING;		/* allow negotiation */
    /*
     * Do special check for standard out fd (1) to see if stall should
     * be disallowed.
     */
    if ( (fd == 1) && bp->nexus->parent ) {
	char *dclout, logname[40], *a, *b;
	sprintf ( logname, "DCL$OUTPUT_%08X", bp->nexus->parent );
	dclout = getenv ( logname );
	if ( dclout ) {
	    /* Compare strings ignoring leading underscores */
	    for ( a = dclout; *a == '_'; a++ );
	    for ( b = bp->nexus->name; *b == '_'; b++ );
	    if (strncmp ( a, b, strcspn ( a, ":" ) ) == 0) bp->is_dcl_out = 1;
	}
    }
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
    TTYPRINT "/bypass/ proc %x bypass negotiate action: %s, streams: %d %d\n",
	my_val->pid, action_name[action], my_val->flags.bit.stream_id,
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
 * Get name of current stderr file and its device's device characteristics.
 */
static int stderr_filename ( struct dsc$descriptor_s *name_dx, int *devchar )
{
    char *rtl_errfile, *buffer;
    int status;
    unsigned short int length, equivlen, code;
    static char logname[40];
    static char equiv[256];
    static $DESCRIPTOR(logname_dx,"SYS$ERROR");
    static $DESCRIPTOR(logvalue_dx,equiv);
    /*
     * Get the name the CRTL is using and see if it matches SYS$ERROR,
     * returning getname() result to caller if not.
     */
    *devchar = 0;
    rtl_errfile = getname(2,name_dx->dsc$a_pointer);
    if ( !rtl_errfile ) return 0;
    length = strcspn ( rtl_errfile, ":.;" );
    if ( strncasecmp(rtl_errfile,"SYS$ERROR:",10) != 0 ) {
	/* stderr NOT open on SYS$ERROR: */
        name_dx->dsc$w_length = strlen ( rtl_errfile );
	*devchar = DEV$M_REC | DEV$M_TRM;
	return 1;
    }
    /*
     * Translate sys$error since this logical isn't inherited by child.
     */
    logvalue_dx.dsc$w_length = sizeof(equiv)-1;

    status = LIB$GET_LOGICAL ( &logname_dx, &logvalue_dx, &equivlen );
#ifdef DEBUG
fprintf(tty,"/bypass/ status of SYS$ERROR translate %d, l=%d, [0..4] = %x %x %x %d\n", 
status,equivlen, (unsigned) equiv[0], (unsigned) equiv[1], (unsigned) equiv[2], (unsigned) equiv[3] );
#endif
    if ( (status&1) == 0 ) return status;
    equiv[equivlen] = 0;	/* terminate string */
    /*
     * Strip process permanent file info and copy into caller's argument.
     */
    buffer = name_dx->dsc$a_pointer;
    if ( (equiv[0] == 27) && (equiv[1] == 0) && (equivlen >= 4) ) {
	strcpy ( buffer, &equiv[4] );
    } else {
       strcpy ( buffer, equiv );
    }
    name_dx->dsc$w_length = strlen ( buffer );
    /*
     * Get device characteristics;
     */
    code = DVI$_DEVCHAR;
    status = LIB$GETDVI ( &code, 0, name_dx, devchar );

    return status;
}
struct stderr_relay {
    unsigned short int chan, active;
    struct { unsigned short status, count; long pid; } iosb;

    char buffer[512];
    char cc[4];
};
static void stderr_relay_ast ( struct stderr_relay *relay )
{
    int status, length;
    /*
     * Check I/O completion status.
     */
    status = relay->iosb.status;
    if ( status & 1 ) {
	/* Normal completion, write received data to our stderr. */
	length = relay->iosb.count;
	relay->buffer[length++] = '\n';
	relay->buffer[length] = '\0';

	fprintf ( stderr, "%s", relay->buffer );
    } else if ( status == SS$_ENDOFFILE ) {
	/* Force flush */
    }
    /*
     * Read next record.
     */
    status = SYS$QIO ( EFN$C_ENF, relay->chan, IO$_READVBLK, &relay->iosb,
	    stderr_relay_ast, relay, 
	    relay->buffer, sizeof(relay->buffer), 0, 0, 0, 0 );
    if ( (status&1) == 0 ) relay->active = 0;

    return;
}
/*
 * Create DMPIPE_STDERR_xxxxxxxx logical name with stderr file children
 * of process xxxxxxxx are to use for their stderr file (rather than stdout).
 */
int dm_bypass_stderr_propagate ( void )
{
    int devchar, status, proc_index, code;
    pid_t self;
    static char logname[40];
    static char errfile[256];
    static $DESCRIPTOR(logname_dx,logname);
    static $DESCRIPTOR(logvalue_dx,errfile);
    static $DESCRIPTOR(table_dx,"LNM$PROCESS");
    static struct stderr_relay relay = { 0, 0, { 0, 0, 0 } };

    struct {
	short int buflen, code;
	void *bufaddr;
	short int *retlen;
    } item[3];
#ifdef DEBUG
    if ( !tty ) tty = fopen ( tt_logical, "a", "shr=put" );
#endif
    /*
     * Use side effect of $GETJPI() to get our PID.
     */
    code = JPI$_PROC_INDEX;
    self = 0;
    status = LIB$GETJPI ( &code, &self, 0, &proc_index, 0, 0 );
    if ( (status&1) == 0 ) {
	if (tty) fprintf ( tty, "/bypass/ getjpi error in propagate(): %d\n", status );
	return status;
    }
    /*
     * Construct logical name and get current stderr filename as
     * equivalence name.
     */
    sprintf ( logname, "DMPIPE_STDERR_%08X", self );
    logname_dx.dsc$w_length = strlen ( logname );

    status = stderr_filename ( &logvalue_dx, &devchar );
    if ( (status&1) == 0 ) {
#ifdef DEBUG
	fprintf(tty,"/bypass/ stderr_filename call failed: %d\n", status );
#endif
	return status;
    }
    /*
     * If parent's stderr is record oriented, assume it is a shareable
     * device child processes can write to directly.
     */
    if ( devchar & DEV$M_REC ) {
	/*
	 * Create user mode logical name in process table.
	 */
	item[0].buflen = logvalue_dx.dsc$w_length;
	item[0].code = LNM$_STRING;
	item[0].bufaddr = logvalue_dx.dsc$a_pointer;
	item[0].retlen = 0;
	item[1].buflen = item[1].code = 0;

	status = SYS$CRELNM ( 0, &table_dx, &logname_dx, 0, item );
#ifdef DEBUG
	fprintf (tty, "/bypass/ fd[stderr] set logical %s status: %d (%s)\n",
		logname, status, errfile );
#endif
	return status;
    }
    /*
     * Parent's stderr is a file.  Create mailbox children will use and
     * relay to stderr in AST thread.  dmpipe_stderr_xxx logical will
     * be mailbox's logical name.
     */
    if ( relay.active ) return status;

    status = SYS$CREMBX ( 0, &relay.chan, 
	sizeof(relay.buffer), sizeof(relay.buffer)*2, 0x0ff0, 0, &logname_dx,
	CMB$M_READONLY );
    if ( (status&1) == 1 ) {
	relay.active = 1;
	status = SYS$QIO ( EFN$C_ENF, relay.chan, IO$_READVBLK, &relay.iosb,
	    stderr_relay_ast, &relay, 
	    relay.buffer, sizeof(relay.buffer), 0, 0, 0, 0 );
	if ( (status&1) == 0 ) relay.active = 0;
    }
    
    return status;
}

int dm_bypass_stderr_recover ( pid_t parent, dm_bypass out_bp )
{
    static char logname[40];
    static char errfile[256];
    static $DESCRIPTOR(logname_dx,logname);
    static $DESCRIPTOR(logvalue_dx,errfile);
    static $DESCRIPTOR(table_dx,"LNM$FILE_DEV");
    static int recovered = 0;
    int status;
    short int length, length2;

    if ( recovered ) return 1;		/* already redirected */
    recovered = 1;
    /*
     * Construct logical name.
     */
#ifdef DEBUG
    if ( !tty ) tty = fopen ( tt_logical, "a", "shr=put" );
#endif
    sprintf ( logname, "DMPIPE_STDERR_%08X", parent );
    logname_dx.dsc$w_length = strlen ( logname );
    logvalue_dx.dsc$w_length = sizeof(errfile)-1;
    /*
     * See if parent used popen.  Should really only check process table.
     */
    status = LIB$GET_LOGICAL(&logname_dx, &logvalue_dx, &length, &table_dx);
    if ( status & 1 ) {
	/*
         * Parent specified device/file we are to use to our stderr.  check
         * for current output.
	 */
	char old_errfile[256], tmp[256], *dclout;
	int i;
	for ( i = 0; i < length; i++ ) 
	    if ( !isprint(errfile[i]) ) tmp[i] = '.'; else tmp[i]=errfile[i];
	tmp[i] = 0;
	sprintf ( logname, "DCL$OUTPUT_%08X", parent );
    	logname_dx.dsc$w_length = strlen ( logname );
	dclout = getenv ( logname );
#ifdef DEBUG
	fprintf(tty, "/bypass/ stderr of parent: '%s', stdout_name: '%s'/'%s'\n", 
	tmp, out_bp->nexus->name, dclout ? dclout : "<none>" );
#endif

	errfile[length] = '\0';
	stderr = freopen ( errfile, "w", stderr, "shr=upi,put" );
	/* Leave old stderr dangling, but closing with freopen messes up DCL
	 * reading pipe output.
	 */
	/* stderr = fopen ( errfile, "a+", "shr=put,upi" ); */
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
    flags = initial_flags & 255;
/* TRACE */
if (flags)
{
/* dmpipe_trace = 1; */
dmpipe_trace_output("dm_bypass_startup_stall:\r\ninitial_flags=%d(0x%X)\r\n"
                    "flags = %d(0x%X)\r\n", initial_flags, initial_flags,
                    flags, flags);
}
/* END TRACE */
    if ( (flags & 1) == 0 )
    {
/* TRACE */
dmpipe_trace = 0;
/* END TRACE */
     return flags;
    }	/* negotiation over */
    if ( (flags & 2) && (*op == 'r') ) return flags;
    if ( (flags & 4) && (*op == 'w') ) return flags;
    if ( bp->is_dcl_out ) {
/* TRACE */
dmpipe_trace = 0;
/* END TRACE */
	return 0;			/* stdout being read by DCL */
    }
    /*
     * If we already have the corresponding stream, we are done.
     */
    nexus = bp->nexus;
    my_val = nexus->lock.my_val;
    peer_val = nexus->lock.peer_val;
    if ( my_val->flags.bit.shutdown || peer_val->flags.bit.shutdown ) {
/* TRACE */
dmpipe_trace = 0;
/* END TRACE */
	return 0;
    }

    if ( (*op == 'w') && (nexus->wstream) ) {
	flags |= 4;
	if ( flags == 7 ) flags = 6;
/* TRACE */
dmpipe_trace = 0;
/* END TRACE */
	return flags;
    } else if ( nexus->rstream ) {
	flags |= 2;
	if ( flags == 7 ) flags = 6;
/* TRACE */
dmpipe_trace = 0;
/* END TRACE */
	return flags;
    }
    /*
     * Method of stall varies with device type.
     */
#ifdef DEBUG
if ( !tty ) tty = fopen ( tt_logical, "a", "shr=put" );
   fprintf (tty, "/bypass/ %x-fd[%d] STARTUP_STALL, initial op '%s' dclout=%d (bfsz=%d)\n",
		bp->nexus->self, bp->related_fd, op, bp->is_dcl_out, nexus->dvi.bufsize  );
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

/* TRACE */
dmpipe_trace_output("Unexpected status from sys_enq to get PW SHM lock:status=%d(0x%X)\r\n",
                    status, status);
/* END TRACE */
	} else if ( peer_val->pid ) {
	   /*
	    * Presence of peer in value block means we can negotiate.
	    * No longer need a channel assigned to the mailbox.
	    */
	   nexus->mbx_watch.is_blocking_lock = 1;
	   deassign_nexus_chan ( nexus );

/* TRACE */
dmpipe_trace_output("Peer process ID is present: assuming SHM bypass.\r\n",
                    status, status);
/* END TRACE */
	} else if ( nexus->dtype == DT$_MBX ) {
	   /*
	    * No peer seen in lock value block, set attention AST on mailbox.
	    */
	    nexus->mbx_watch.func = IO$_SETMODE;
	    nexus->mbx_watch.func |= (*op == 'w') ? IO$M_READATTN : IO$M_WRTATTN;

	    status = SYS$QIOW ( EFN$C_ENF, nexus->chan, nexus->mbx_watch.func,
		&nexus->mbx_watch.iosb, 0, 0, stall_mbx_ast, bp, 0, 0, 0, 0 );
 	    if ( (status&1) == 1 ) status = nexus->mbx_watch.iosb.status;
/* TRACE */
dmpipe_trace_output("Attempted QIOW to set MBX attention AST:status=%d(0x%X)\r\n",
                    status, status);
/* END TRACE */
	} else if ( nexus->dtype == DT$_PIPE ) {
	    /*
	     * DCL pipe driver doesn't have attention ASTs, just stall
	     * for 200 milliseconds.
	     */
	    long long delay;
	    delay =  200 * -10000;	/* 100-nanosecond ticks */
	    status = SYS$SETIMR ( EFN$C_ENF, &delay, stall_mbx_ast, bp, 0 );
/* TRACE */
dmpipe_trace_output("Attempted SYS$SETIMR for PIPE:status=%d(0x%X)\r\n",
                    status, status);
/* END TRACE */
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
/* TRACE */
dmpipe_trace_output("Wakeup from SYS$HIBER with:status = %d(0x%X)\r\n"
                    "is_blocking_lock = %d\r\nmailbox_active = %d\r\n",
                    status, status, nexus->mbx_watch.is_blocking_lock,
                    nexus->mbx_watch.mailbox_active);
/* END TRACE */
	    }
	}
#ifdef DEBUG
fprintf ( tty,"/bypass/ %x-fd[%d] wait complete(sts=%d), lock block: %d, mbx attn: %d (%d)\n",
		bp->nexus->self, bp->related_fd, status,
		nexus->mbx_watch.is_blocking_lock, 
		nexus->mbx_watch.mailbox_active, nexus->dvi.bufsize );
#endif
	/*
	 * Wait done, cleanup.  Kill pending attention/timer AST.
	 */
	if ( !nexus->mbx_watch.mailbox_active ) {
/* TRACE */
dmpipe_trace_output("nexus->mbx_watch.mailbox_active = %d\r\n"
                    "nexus->dtype = %d\r\n", nexus->mbx_watch.mailbox_active, nexus->dtype);
/* END TRACE */
	    if ( nexus->dtype == DT$_MBX ) {
	      status = SYS$QIOW ( EFN$C_ENF, nexus->chan, nexus->mbx_watch.func,
	      		&nexus->mbx_watch.iosb, 0, 0, 0, 0, 0, 0, 0, 0 );
/* TRACE */
dmpipe_trace_output("QIOW to remove MBX attention AST:status=%d(0x%X)\r\n"
                    "iosb.status=%d(0x%X)\r\n", status, status,
                    nexus->mbx_watch.iosb, nexus->mbx_watch.iosb);
/* END TRACE */
	    } else {
		status = SYS$CANTIM ( bp, 0 );
/* TRACE */
dmpipe_trace_output("SYS$CANTIM to remove PIPE timer:status=%d(0x%X)\r\n",
                    status, status);
/* END TRACE */
	    }
	} else if ( (nexus->dtype == DT$_MBX) && (*op == 'r') ) {
	    /*
	     * Assign a readonly channel to the mailbox and set new
	     * attention AST to notify us when no writers
	     */
		status = alternate_read_bypass_init ( nexus );
	     if ( status&1 ) 
	       flags |= DM_BYPASS_HINT_READS;
/* TRACE */
dmpipe_trace_output("Attempted alternate_read_bypass_init:status=%d(0x%X)\r\n"
                    "flags = %d(0x%X)\r\n", status, status, flags, flags);
/* END TRACE */
	}

	if ( nexus->mbx_watch.is_blocking_lock ) {
	    /*
	     * Another process has lock.  setup streams if we can.
	     */
	    flags = negotiate_bypass ( flags, bp, op, fcntl_flags );
/* TRACE */
dmpipe_trace_output("Attempted negotiate_bypass:flags=%d(0x%X)\r\n",
                    flags, flags);
/* END TRACE */
	    deassign_nexus_chan ( nexus );
	} else {
	    /*
	     * No conflicting locks seen, give up on memory bypass.
	     */
	    my_val->flags.bit.shutdown = 1;
	    status = sys_enq ( LCK$K_CRMODE, &bp->nexus->lock, LCK$M_NOQUEUE,
		0, 0 );
	    flags = (flags&0x7ffe);
/* TRACE */
dmpipe_trace_output("No SHM locks detected :status = %d(0x%X)\r\nflags=%d(0x%X)\r\n",
                    status, status, flags, flags);
/* END TRACE */
	    /*
	     * Check for alternate bypass mode in which we take over
	     * reads of mailbox to detect no-writers condition.
	     */
	    if ( initial_flags&DM_BYPASS_HINT_POPEN_R ) {
		status = alternate_read_bypass_init ( nexus );
		if ( status&1 ) 
		   flags |= (DM_BYPASS_HINT_READS|DM_BYPASS_HINT_POPEN_R);
/* TRACE */
dmpipe_trace_output("Attempted alternate_read_bypass_init2:status = %d(0x%X)\r\n"
                    "flags=%d(0x%X)\r\n", status, status, flags, flags);
/* END TRACE */
	    }
	    deassign_nexus_chan ( nexus );
	}
    } else {
	/*
	 * Unsupported device type.
	 */
	flags = (flags&0x7ffe);
	    sys$dassgn ( nexus->chan );
	    nexus->chan = 0;
/* TRACE */
dmpipe_trace_output("Unsupported pipe device!:flags=%d(0x%X)\r\n",
                    flags, flags);
/* END TRACE */
    }
    if ( (flags&1) == 0 ) {
/* TRACE */
dmpipe_trace_output("SHM bypass not used; removing nexus\r\n");
/* END TRACE */
	/* Startup done, deassign channel so CRTL closes down */
	deassign_nexus_chan ( nexus );
    }
/* TRACE */
dmpipe_trace = 0;
/* END TRACE */
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
int dm_bypass_read ( dm_bypass bp, void *buffer, size_t nbytes, 
	size_t min_bytes, int *expedite_flag )
{
/* TRACE */
int count;
/* END TRACE */
    int doesnt_care;
    if ( bp->nexus->rstream ) {
	return memstream_read ( bp->nexus->rstream, buffer, nbytes, min_bytes,
		expedite_flag ? expedite_flag : &doesnt_care );
    }
    if ( bp->nexus->alt.buffer ) {
	/* Read directly from mailbox, but check for no writers */
	count = alternate_bypass_read ( bp->nexus, buffer, nbytes, min_bytes, 
		expedite_flag ? expedite_flag : &doesnt_care );
/* TRACE */
        if (count == 0)
           {
/*           dmpipe_trace = 1; */
           dmpipe_trace_output("alternate_bypass_read returned 0!\r\n");
           }
/* END TRACE */
	return count;
    }
    return -1;
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

/*
* Check to see if peer closed its stream.
*/
int is_dm_bypass_peer_done(dm_bypass bp)
{
int ret_val = 0;

if (bp->nexus->alt.buffer)
   {
   ret_val = (bp->nexus->alt.iosb.status == SS$_ENDOFFILE);
   }
else
   {
   if (bp->nexus->wstream)
      ret_val = is_memstream_peer_done(bp->nexus->wstream, 1);
   else
      if (bp->nexus->rstream)
         ret_val = is_memstream_peer_done(bp->nexus->rstream, 0);
      else
         ret_val = 1;
   }
return ret_val;
}
