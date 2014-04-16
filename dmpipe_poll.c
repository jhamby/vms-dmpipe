/*
 * dmpipe_poll module supports poll() and select() functions inside of DMPIPE.
 * Taking over poll() function from the RTL allows proper polling of the
 * pipe devices being bypassed.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unixio.h>
#include <socket.h>
#include <errno.h>

#include <starlet.h>
#include <ssdef.h>
#include <lib$routines.h>
#include <descrip.h>
#include <dvidef.h>
#include <iodef.h>
#include <dcdef.h>
#include <devdef.h>
#include <efndef.h>
#include <agndef.h>		/* assign flags */

#include "dmpipe_poll.h"
#define POLL_PERIOD_MSEC 100;
FILE * tty;
#define TTYPRINT if(!tty) tty=fopen("DBG$OUTPUT:","w"); fprintf(tty,
/*
 * Commbuf stream states:
 */
#define MEMSTREAM_STATE_IDLE 0		/* not full and space available */
#define MEMSTREAM_STATE_EMPTY 1		/* empty and reader waiting */
#define MEMSTREAM_STATE_FULL 2		/* no space and writer waiting */
#define MEMSTREAM_STATE_WRITER_DONE 3   /* Closed by writer */
#define MEMSTREAM_STATE_READER_DONE 4   /* closed by reader */
#define MEMSTREAM_STATE_CLOSED 5	/* Both sides closed */
/*
 * Declare global exit handler control block responsible for cleanup of
 * all tracking blocks on program exit (especially to garantee mailbox
 * channels deassigned.
 */
static int poll_track_rundown ( int *exit_status, dm_poll_track *known_trk );

static struct {
    int status;				/* receives image exit status */
    dm_poll_track known_tracks;
} rundown;
static struct {
    void *flink;			/* used by VMS */
    int (*handler) (int *exit_status, dm_poll_track *track_list );
    int arg_count;
    int *exit_status;
    dm_poll_track *track_list;
} exit_handler_desc = {
    0, 0, 2, &rundown.status, &rundown.known_tracks
};
/*
 * Each new poll group begin increases the last_incarnation value by one.
 * The main purpose of the incarnation is so we can detect duplicate fds
 * in the fildes array passed to us.
 */
static unsigned int dm_last_incarnation = 0;
/*
 * Cleanup and dispose of poll_track block allocated by create_track.
 */
static int rundown_extension ( dm_poll_track pt )
{
    int status;

    if ( pt->chan ) status = SYS$DASSGN ( pt->chan );
    pt->chan = 0;
    /*
     * if block currently part of poll operation, skip freeing block.
     */
    if ( pt->incarnation > 0 ) return status;

    free ( pt );
    return status;
}
/*
 * Exit handller called by VMS on image exit.
 */
static int poll_extension_rundown ( int *exit_status, 
	dm_poll_track *known_tracks )
{
    dm_poll_track pt, next_pt;
    struct commbuf *buf;
    /*
     * Traverse list of known tracks and dispose.
     */
    for ( pt = *known_tracks; pt; pt = *known_tracks ) {
	/*
         * remove from list known so can deallocate memory before
         * going to next block.
         */
	*known_tracks = pt->next_ext;
	/*
	 * Dispose of block.
	 */
	rundown_extension ( pt );
    }
    return 1;
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
struct dm_devinfo {
    int devclass;
    int devtype;
    long devchar;
    int bufsize;
    pid_t owner;
    int refcnt;
};

static int get_device_chan_and_info ( int fd, char device_name[32],
	unsigned short *chan, struct dm_devinfo *info, int skip )
{
    struct stat finfo;
    static $DESCRIPTOR(st_dev_dx,"");
    unsigned short devnam_len;
    int status, required_devchar, flags;
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
	if ( fstat ( fd, &finfo ) < 0 ) {
	    return SS$_BADPARAM;
	}
	st_dev_dx.dsc$a_pointer = finfo.st_dev;
	st_dev_dx.dsc$w_length = strlen (st_dev_dx.dsc$a_pointer);
	/*
 	 * Assign channel.  If fd open on a network link, device name
         * is invalid.  ($define staff$disk mba0: may cause mischief).
	 * If it's a mailbox, assign channel readonly so CRTL can get NOWRITER
	 * errors.
     	 */
	flags = 0;
	if ( finfo.st_dev[0] == '-' ) {
	    if ( strncasecmp(&finfo.st_dev[1], "MBA", 3 == 0 ) )
		flags |= AGN$M_READONLY;
	} else if ( strncasecmp ( finfo.st_dev, "MBA", 3 ) == 0 ) {
	    flags |= AGN$M_READONLY;
	}
	status = SYS$ASSIGN ( &st_dev_dx, chan, 0, 0, flags );
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
	TTYPRINT "/poll/ getdvi err on '%s': %d\n", finfo.st_dev,status);
	return status;
    }
    if ( skip == 0 ) device_name[devnam_len] = 0;	/* terminate string */
    /*
     * If device class unknown, force a mailbox type if it appears to
     * be the pipe driver used by DCL.
     */
#ifdef DEBUG
    TTYPRINT "/poll/ fd[%d] info: nam=%s, class=%d/%d, refcnt=%d/%x\n",
	fd, device_name, info->devclass, info->devtype, info->refcnt,
	info->owner );
#endif

    required_devchar = DEV$M_REC | DEV$M_AVL | DEV$M_IDV | DEV$M_ODV;
    if ( (info->devclass == 0) && (info->devtype == 0) &&
	((info->devchar & required_devchar) == required_devchar) ) {
	char *devnam;
	for ( devnam = device_name; *devnam == '_'; devnam++ );
	if ( strncmp ( devnam, "MPA", 3 ) == 0 ) {
#ifdef DEBUG
	    TTYPRINT "/poll/ deeming %s a mailbox...\n", devnam );
#endif
	    info->devclass = DC$_MAILBOX;
	    info->devtype = DT$_PIPE;
	}
    }
    return status;
}
/*
 * Utility functions for use by device-specific functions.  The maskable_event
 * function sets the event bit conditional upon the caller-supplied mask.
 * When a bit is set, the count is updated.
 */
static inline void maskable_event ( dm_poll_track pt, int poll_event, int
 select_event, int *count )
{
    if ( pt->filedes ) {
	if ( pt->filedes->events & poll_event ) {
	    pt->filedes->revents |= (poll_event & pt->filedes->events);
	    *count = (*count)+1;
	}
    } else {
	if ( pt->select_mask & select_event ) {
	    pt->select_event |= select_event;
	    *count = (*count)+1;
	}
    }
}
static inline void nonmaskable_event ( dm_poll_track pt, int poll_event,
    int select_event, int *count )
{
    if ( pt->filedes ) {
	    pt->filedes->revents |= poll_event;
    } else {
	    pt->select_event |= select_event;
    }
    (*count)++;
}

static int sys_qiow_2p ( dm_poll_track pt, int func_code, void *p1, 
	unsigned int p2 )
{
    int status;
    status = SYS$QIOW ( EFN$C_ENF, pt->chan, func_code, &pt->iosb, 0, 0,
		p1, p2, 0, 0, 0, 0 );
    if ( (status&1) == 1 ) status = pt->iosb.any.status;
    return status;
}
/*
 * Asynch call, 2 driver/function-specific parameters.
 */
typedef void (*dm_ast_func) ( void *arg_vp );

static int sys_async_qio_2p ( dm_poll_track pt, int func_code, 
	dm_ast_func ast_rtn, void *ast_arg, void *p1, unsigned int p2 )
{
    int status;
    status = SYS$QIOW ( EFN$C_ENF, pt->chan, func_code, &pt->iosb, 
		ast_rtn, ast_arg, p1, p2, 0, 0, 0, 0 );
    if ( (status&1) == 1 ) status = pt->iosb.any.status;
    return status;
}
/****************************************************************************/
/*
 * Device specific query and cancel query routines.
 * query functions return values:
 *    -1   error, exception event should be generated.
 *     0    No events reported.
 *     >0   at least one event reported.
 *
 * Cancel functions return 0 or -1 on fatal error.
 */
static int x11_query ( dm_poll_track pt )
{
    int status, ef, all;

    ef = (pt->fcntl_flags>>24) & 127;
    status = SYS$READEF ( ef, &all );
    if ( status == SS$_WASCLR ) return 0;
    if ( status == SS$_WASSET ) return 1;
    /*
     * Unexpected return code, likely invalid event flag number.
     */
    errno = EINVAL;
    return -1;
}
static int x11_cancel ( dm_poll_track pt )
{
    pt->device_cancel = 0;	/* will never do anything, so delete myself */
    return 0;
}

static int socket_query ( dm_poll_track pt )
{
    return -1;
}
static int socket_cancel ( dm_poll_track pt )
{
    pt->device_cancel = 0;	/* will never do anything, so delete myself */
    return -1;
}
static int pipe_query ( dm_poll_track pt )
{
    int count, status;
    /*
     * Assume device is a mailbox.  The SENSEMODE function puts mailbox
     * information in the IOSB.
     */
    count = 0;
    status = sys_qiow_2p ( pt, IO$_SENSEMODE, 0, 0 );
    if ( status & 1 ) {
	if ( pt->iosb.mbxsense.count > 0 ) {
	    /*
	     * Messages available, turn on POLLIN and POLLOUT
	     */
	    maskable_event ( pt, POLLIN|POLLOUT,
		DM_POLL_SELECT_READ|DM_POLL_SELECT_WRITE, &count );
	}
    } else {
 	nonmaskable_event ( pt, POLLERR, DM_POLL_SELECT_EXCEPT, &count );
    }
    return count;
}
static int pipe_cancel ( dm_poll_track pt )
{
    pt->device_cancel = 0;	/* will never do anything, so delete myself */
    return 0;
}
/*
 * Terminal devices.  Use sensemode function to check typeahead buffer.
 */
static int tty_query ( dm_poll_track pt )
{
    int count, status;
    struct {
	unsigned short numchars;
	unsigned char firstchar, reserved0;
	unsigned long reserved1;
    } type_ahead;
    /*
     * Always report terminal as writable.
     */
    count = 0;
    maskable_event ( pt, POLLOUT, DM_POLL_SELECT_WRITE, &count );
    /*
     * Read typahead buffer.
     */
    status = sys_qiow_2p ( pt, IO$_SENSEMODE | IO$M_TYPEAHDCNT,
	&type_ahead, sizeof(type_ahead) );

    if ( status&1 ) {
	if ( type_ahead.numchars > 0 ) 
	    maskable_event ( pt, POLLIN, DM_POLL_SELECT_READ, &count );
    } else {
	nonmaskable_event ( pt, POLLERR, DM_POLL_SELECT_EXCEPT, &count );
    }
    return count;
}
static int tty_cancel ( dm_poll_track pt )
{
    pt->device_cancel = 0;	/* will never do anything, so delete myself */
    return 0;
}
/*
 * DMPIPE bypass streams.
 */
static int bypass_query ( dm_poll_track pt )
{
    memstream rstream, wstream;
    int status, count, state, pending_bytes, available_space;
    struct pollfd *pblk;
    /*
     * drill past bypass layer to get directly to memstream objects for
     * query.
     */
#ifdef DEBUG
TTYPRINT "/poll/ bypass query for fd[%d]\n", pt->fd );
#endif
    if ( 0 > dm_bypass_current_streams (pt->bp, &rstream, &wstream)) return -1;

    pblk = pt->filedes;
    count = 0;
    if ( rstream ) {
	memstream_query(rstream, &state, &pending_bytes, &available_space,1);
#ifdef DEBUG
TTYPRINT "/poll/ rstream query: 0%o on fd[%d]: %d %d %d\n", 
pblk->events, pt->fd, state, pending_bytes,
available_space );
#endif

	if ( pending_bytes > 0 ) 
	    maskable_event ( pt, POLLIN, DM_POLL_SELECT_READ, &count );
	else if ( (state == MEMSTREAM_STATE_WRITER_DONE) ||
		(state == MEMSTREAM_STATE_CLOSED) ) {
	    nonmaskable_event ( pt, POLLHUP, DM_POLL_SELECT_EXCEPT, &count );
	}
    } else {
	/* report error/hup if polling for readablity */
/*
	if ( pblk ) {
	    if ( pblk->events&POLLOUT ) { pblk->revents |= POLLNVAL; count++;}
	} else {
	    count++; pt->select_event |= DM_POLL_SELECT_EXCEPT;
	} */
    }

    if ( wstream ) {
	memstream_query(wstream, &state, &pending_bytes, &available_space,0);

	if ( (state == MEMSTREAM_STATE_READER_DONE) ||
		(state == MEMSTREAM_STATE_CLOSED) ) 
	    nonmaskable_event ( pt, POLLHUP, DM_POLL_SELECT_EXCEPT, &count );

	if ( available_space > 0 ) 
	    maskable_event ( pt, POLLOUT, DM_POLL_SELECT_WRITE, &count );
    } else {
	/* report error/hup if polling for writeablity */
/*
	if ( pblk ) { pt->filedes->revents |= POLLNVAL; count++; }
	else pt->select_event |= DM_POLL_SELECT_EXCEPT; count++; */
    }

    return count;
}
static int bypass_cancel ( dm_poll_track pt )
{

    pt->device_cancel = 0;	/* will never do anything, so delete myself */
    return 0;
}
/*
 * Decide what a device is and prepare it for polling.  The device class
 * specifiy query and cancel routines are loaded int the track structure.
 */
static int classify_device ( dm_poll_track pt )
{
    struct dm_devinfo devinfo;
    dm_bypass bp;
    memstream wstream, rstream;
    int status;
    /*
     * Lookup VMS device characteristics.  Channel assigned will be persistent
     * so polling won't require a new $ASSIGN call each time.
     */
    status = get_device_chan_and_info ( pt->fd, pt->device_name,
		&pt->chan, &devinfo, 0 );
    if ( (status&1) == 0 ) {
	errno = EINVAL;
	return -1;
    }
    /*
     * Suss out what type of device it is, default to UNKNOWN.
     */
    pt->dev_type = DMPIPE_POLL_DEV_UNKNOWN;
    bp = pt->bp;
    if ( bp ) {
	/* Potentially a memory stream pipe, have to verify though */
	dm_bypass_current_streams ( bp, &rstream, &wstream );
	if ( !rstream && !wstream ) bp = 0;	/* memstream not active */
    }
    if ( bp ) {
	pt->device_query = bypass_query;
 	pt->device_cancel = bypass_cancel;
	pt->dev_type = DMPIPE_POLL_DEV_BYPASS;

    } else if ( isapipe ( pt->fd ) ) {
	/* assign channel will be used to sensemode pipe device */
	pt->device_query = pipe_query;
	pt->device_cancel = pipe_cancel;
	pt->dev_type = DMPIPE_POLL_DEV_PIPE;

    } else if ( devinfo.devclass == DC$_TERM ) {
	/* assign channel to terminal */
	pt->device_query = tty_query;
	pt->device_cancel = tty_cancel;
	pt->dev_type = DMPIPE_POLL_DEV_TTY;

    } else if ( pt->sdc_chan = decc$get_sdc(pt->fd) ) {
	/* We are a TCP/IP socket, fake the devinfo  */
	pt->device_query = socket_query;
 	pt->device_cancel = socket_cancel;
	pt->dev_type = DMPIPE_POLL_DEV_SOCKET;

    } else if ( devinfo.devclass == DC$_MAILBOX ) {
	/* 
	 * Mailbox devices cover a range: mailbox, shared-memory, null device.
	 */
	if ( devinfo.devtype == DT$_MBX ) {
	    /* Treat as a pipe */
	    pt->device_query = pipe_query;
	    pt->device_cancel = pipe_cancel;
	    pt->dev_type = DMPIPE_POLL_DEV_PIPE;

	} else if ( (devinfo.devtype == DT$_NULL) && 
		(pt->fcntl_flags & 0xff000000) ) {
	    /* Treat as X11 device, flag to wait on is encoded in fcntl_flags */
	    pt->device_query = x11_query;
	    pt->device_cancel = x11_cancel;
	    pt->dev_type = DMPIPE_POLL_DEV_X11;
	}
	
    }
#ifdef DEBUG
TTYPRINT "/poll/ fd[%d] classified as type %d (dc=%d,dt=%d,bp=%x)\n", pt->fd,
pt->dev_type, devinfo.devclass, devinfo.devtype, pt->bp);
#endif
    return 0;
}
/*********************************************************************/
/*
 * Public function. Usage is to create a poll group, add the
 * fds to be polled to it (add will screen out fds open on files and
 * devices we don't support), then call scan to actually do the polling.
 * Finally, the group is torn down (possibly resources released) by
 * calling dm_poll_group_end().
 */
dm_poll_track dm_poll_create_track ( int fd, dm_bypass bp, int fcntl_flags )
{
    int status;
    dm_poll_track pt;
    /*
     * Establish exit handler to clean up dangling poll track resources.
     */
    if ( exit_handler_desc.handler == 0 ) {
	rundown.known_tracks = 0;
	rundown.status = 1;
	exit_handler_desc.handler = poll_extension_rundown;
	status = SYS$DCLEXH ( &exit_handler_desc );
	if ( (status&1) == 0 ) printf ( "Bugcheck, DCLEXH failed: %d\n",status);
    }
    /*
     * Allocate a block and link into rundown list.
     */
    pt = calloc ( sizeof(struct dm_poll_extension), 1 );
    if ( !pt ) return pt;
    pt->next_ext = rundown.known_tracks;
    rundown.known_tracks = pt;
    /*
     * Determine what kind of file is open on fd so we can decide how
     * to poll it.
     */
    pt->bp = bp;
    pt->fd = fd;
    pt->fcntl_flags = fcntl_flags;

    classify_device ( pt );

    return pt;
}

int dm_poll_rundownn_track (dm_poll_track *pt_ptr )
{
    dm_poll_track pt, prev, cur;
    /*
     * Remove from rundown list.  Don't trust the contents if pointer not
     * found on known tracks list.
     */
    if ( !pt_ptr ) return -1;
    pt = *pt_ptr;
    if ( !pt ) return -1;		/* invalid pointer */

    prev = 0;
    for ( cur = rundown.known_tracks; cur; cur = cur->next_ext ) {
	if ( cur == pt ) {
	    if ( prev ) prev->next_ext = cur->next_ext;
	    else rundown.known_tracks = cur->next_ext;  /* head of list */
	    break;
	}
	prev = cur;
    }
    if ( !cur ) return -1;
    *pt_ptr = 0;		/* Zero for caller */
    /*
     * Dispose of block using common routine called by exit handler.
     */
    return rundown_extension ( pt );
}

int dm_poll_group_begin ( struct dm_poll_group *group, int timeout, int type )
{
    memset ( group, 0, sizeof(struct dm_poll_group) );
    group->incarnation = (++dm_last_incarnation);
    if ( !group->incarnation ) group->incarnation = (++dm_last_incarnation);
    /*
     * Save timeout and initialize poll period.
     */
    if ( (type != POLL_TYPE_POLL) && (type != POLL_TYPE_SELECT) ) {
TTYPRINT "group_being called with invalid poll type: %d\n", type );
	return -1;
    }
    group->poll_period = (-10000) * POLL_PERIOD_MSEC;
    group->timeout_ms = timeout;
    group->timeout_period = -10000;   /* 1 millsecond delta time */
    if ( timeout > 0 ) group->timeout_period *= timeout;
    else group->timeout_period = 0;
    group->type = type;
    return 0;
}
/*
 * add poll_track to current group and prepare it for polling.
 */
int dm_poll_group_add_pollfd 
	(struct dm_poll_group *group, struct pollfd *filed, dm_poll_track pt )
{
    /*
     * Hook into group membership list.
     */
    if ( group->type != POLL_TYPE_POLL ) return -1;

    if ( pt->incarnation == group->incarnation ) {
	/* Existing block, merge the request masks */
	return 0;
    } else if ( pt->incarnation ) {
	/* Bugcheck, shouldn't be on another list. */
	return -1;
    }
    pt->next_member = 0;
    if ( group->first_member ) group->last_member->next_member = pt;
    else group->first_member = pt;
    group->last_member = pt;
    /*
     * If a previous poll_group_begin this fd, we are already set up
     * to query it and don't need to classify it again.
     */
    if ( !pt->device_query ) classify_device ( pt );
    else { }
    group->histogram[pt->dev_type]++;
    group->total_fds++;
    /*
     * Build mask of events to check for.
     */
    pt->select_mask = 0;
    pt->select_event = 0;
    pt->filedes = filed;
    
    return 0;
}

int dm_poll_group_add_selectfd ( struct dm_poll_group *group, int fd, 
	int select_mask, dm_poll_track pt )
{
    /*
     * Allocate poll_track block and add to group, prevent adding
     * multiple fd's to avoid duplicate deallocation problems.
     */
    if ( group->type != POLL_TYPE_SELECT ) return -1;
    if ( pt->incarnation == group->incarnation ) {
	/* Existing block, merge the request masks */
	return 0;
    } else if ( pt->incarnation ) {
	/* Bugcheck, shouldn't be on another list. */
	return -1;
    }
    pt->next_member = 0;
    if ( group->first_member ) group->last_member->next_member = pt;
    else group->first_member = pt;
    group->last_member = pt;
    /*
     * If a previous poll_group_begin this fd, we are already set up
     * to query it and don't need to classify it again.
     */
    if ( !pt->device_query ) classify_device ( pt );
    else { }
    /*
     * Build mask of events to check for.
     */
    pt->select_mask = select_mask;
    pt->filedes = 0;
    
    return 0;;
}

static void scan_timeout ( void *group_vp )
{
    struct dm_poll_group *group;
    group = group_vp;
    group->timeout_expired = 1;
    SYS$WAKE ( 0, 0 );
}
/*
 * Do the guts of the poll() function.  Return value is number of events
 * or -1.
 */
int dm_poll_scan_group ( struct dm_poll_group *group )
{
    dm_poll_track pt;
    int status, count, timer_armed;
    long long timeout_time, start_time, now;
    /*
     * Outer loop, repeat until timout
     */
    count = 0;
    if ( group->timeout_ms > 0 ) {
        timer_armed = 0;
	group->timeout_expired = 0;
	SYS$GETTIM ( &start_time );
    } else if ( group->timeout_ms == 0 ) { 
	/* Immediate timeout */
	timer_armed = 1; 
	group->timeout_expired = 1;
    } else {
	/* indefinite time, make code think whe have a timer when we don't */
	timer_armed = 1;
	group->timeout_expired = 0;
     }
#ifdef DEBUG
TTYPRINT "/poll/ scan_group, timer: %d (exp:%d), first_mem: %x\n",
timer_armed, group->timeout_expired, group->first_member );
#endif

    do {
	/*
	 * Do a device scan and note positive results.
	 */
	for ( pt = group->first_member; pt; pt = pt->next_member ) {
	    if ( !pt->device_query ) continue;
	    status = pt->device_query ( pt );
	    if ( status < 0 ) break;
	    else if ( status > 0 ) count++;
	}
	/*
	 * Stall for poll_period delay if wait not over.
	 */
	if ( count == 0 ) {
	    if ( !timer_armed ) {
		/*
		 * Set timer to terminate scan.  Adjust timeout period for
		 * ammount of time consumed by scan (should be zero).
		 */
		SYS$GETTIM ( &now );
		timeout_time = now - start_time;	/* positive number */
		timeout_time += group->timeout_period;
		/*
		 * timeout_time should now be a negative number, if not then
		 * current time is past timeout_period+start_time.
		 */
		if ( timeout_time < 0 ) {
		    status = SYS$SETIMR ( EFN$C_ENF, &timeout_time,
			scan_timeout, group, 0 );
		} else status = SS$_TIMEOUT;

		if ((status&1) == 0 ) group->timeout_expired = 1;
		timer_armed = 1;
	    }
	    SYS$SCHDWK ( 0, 0, &group->poll_period, 0 );
	    SYS$HIBER();
	}
    } while ( (count == 0) && !group->timeout_expired );
    /*
     * Cleanup, remove timer queue entry if still pending.
     */
    if ( timer_armed && !group->timeout_expired && (group->timeout_ms > 0) ) {
	status = SYS$CANTIM ( group, 0 );
    }
    return count;
}

int dm_poll_group_end ( struct dm_poll_group *group )
{
    dm_poll_track pt;
    /*
     * Break down group;
     */
    while ( group->first_member ) {
	pt = group->first_member;
	group->first_member = pt->next_member;
	pt->incarnation = 0;

	/* Cancel waits */
	if ( pt->device_cancel ) pt->device_cancel ( pt );
    }
    return -1;
}
