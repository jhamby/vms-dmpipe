/*
 * Define functions for moving stream of data between 2 processes via
 * a shared memory block.  One process is the writer that adds data to
 * the shared memory and the other process is the reader (2-way cata
 * transfer requires 2 streams).
 *
 * Date: 10-MAR-2014
 */
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>

#include <jpidef.h>			/* VMS Job/Process Information */
#include <syidef.h>			/* VMS System Information */
#include <starlet.h>			/* VMS system services prototypes */
#include <ssdef.h>			/* VMS system service condition codes*/
#include <lib$routines.h>		/* VMS RTL functions */
#include <builtins.h>			/* DEC C builtin functions */

#include "memstream.h"
/*
 * Glocal parameters for spin lock and exit handler.
 */
static struct {
    int initial_retry;			/* Spin lock retry count */
    int stall_retry;			/* Spin lock secondary retries */
    int stall_msec;			/* Secondary sleep time */
    int seg_limit;			/* Max data xfer while lock held */

    long long stall_delta;		/* VMS delta time */
    pid_t self;				/* Current process PID */
    int cpu_count;			/* Number of CPUs available */
    int sequence;			/* Number of memstreams initialized */
    int spinlock_fails;
} spn = {
    100000, 1500, 20, 4096, 0, 0, 0, 0, 0
};
static int memstream_rundown ( int *exit_status, memstream *open_streams );

static struct {
    int status;
    memstream open_streams;
} rundown;
static struct {
    void *flink;			/* used by VMS */
    int (*handler) (int *exit_status, memstream *open_streams );
    int arg_count;
    int *exit_status;
    memstream *open_streams;
} exit_handler_desc = {
    0, 0, 2, &rundown.status, &rundown.open_streams
};
/*
 * Block in shared memory used to transfer data.
 */
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
    int attributes;			/* control flags */
    struct memstream_stats *stats;      /* Optional. */
};
/****************************************************************************/
/* Get current process ID and save in global spn struct.
 */
static int set_spn_self ( void )
{
    int status, code, value;
    /*
     * Determine our own PID.
     */
    code = JPI$_PROC_INDEX;
    spn.self = 0;
    status = LIB$GETJPI ( &code, &spn.self, 0, &value, 0, 0 );
    if ( status&1 ) {
    }
    /*
     * Get number of CPUs so we can adjust spinlock parameters.
     */
    code = SYI$_ACTIVECPU_CNT;
    status = LIB$GETSYI ( &code, &spn.cpu_count, 0, 0, 0, 0 );

    if ( status&1 ) if ( spn.cpu_count == 1 ) {
	/*
	 * Change default parameters for spinlocks when a uni-processor.
	 * Spin locks don't make progress when only 1 CPU, minimize the
         * the spin and force stalls to a pre-expired time (yields processor
         * until next scheduler tick).
	 */
	spn.initial_retry = 2;
	spn.stall_retry = 1;
	spn.stall_msec = 0;		/* force to min. delay. */
	SYS$GETTIM ( &spn.stall_delta );
    }
    /*
     * Establish exit handler to force any open streams to closed state on
     * program exit to help prevent hangs.
     */
    if ( exit_handler_desc.handler == 0 ) {
	rundown.open_streams = 0;
	rundown.status = 1;
	exit_handler_desc.handler = memstream_rundown;
	status = SYS$DCLEXH ( &exit_handler_desc );
	if ( (status&1) == 0 ) printf ( "Bugcheck, DCLEXH failed: %d\n",status);
    }
    return status;
}
static void acquire_lock ( volatile struct commbuf *buf )
{
    union lock_state new, old;
    if ( spn.cpu_count == 1 ) {		/* uniprocessor */
	new.state.flag = 1;
	new.state.owner = spn.self;	/* take ownership */
	while ( 1 ) {
	    old.state_qw = __ATOMIC_EXCH_QUAD ( &buf->lock, new.state_qw );
	    if ( old.state.flag ) {		/* previously locked */
		spn.spinlock_fails++;
	        SYS$HIBER();
	    } else break;
	};
    } else {				/* multi-processor */
	int spin_result, status;
	for (spin_result=__LOCK_LONG_RETRY(&buf->lock.state.flag, spn.initial_retry);
	     spin_result == 0;
	     spin_result=__LOCK_LONG_RETRY(&buf->lock.state.flag,spn.stall_retry)) {
	     spn.spinlock_fails++;
	     status=SYS$SCHDWK( &spn.self, 0, &spn.stall_delta, 0 );
	     if ( status&1 ) status = SYS$HIBER();
	     if ( status&1 ) return;	/* 0 */
	}
	buf->lock.state.owner = spn.self;
    }
}
static int release_lock ( volatile struct commbuf *buf )
{
    union lock_state new, old;
    int status;

    if ( spn.cpu_count == 1 ) {		/* uniprocessor */
	new.state.flag = 0;
	new.state.owner = 0;		/* make unowned */
	while ( 1 ) {
	    old.state_qw = __ATOMIC_EXCH_QUAD ( &buf->lock, new.state_qw );
	    if ( old.state.owner && old.state.owner != spn.self ) {
		/* Peer is trying to get access, kick it */
	        status = SYS$WAKE ( &old.state.owner, 0 );
		if ( (status & 1) == 0 ) {
		    /* Assume peer went away (rather than unprivileged) */
		    return 0;
		}
	    } else break;
	};
    } else {				/* multi-processor */
	__UNLOCK_LONG ( &buf->lock.state.flag );
    }
    return 1;
}
/***********************************************************************/
/* Primitives for copying data into and out of buffer as atomic operation
 * using spin lock.
 *
 * Return value:
 *        READY          Success, all bytes transferred.
 *        BLOCKED        Buffer full(write) or empty(empty), partial transfer.
 *        CLOSED         Pipe is closed, partial transfer.
 *        ABORT          Error, indeterminate transfer.
 *
 * Actions taken by caller depend on both status and previous buffer state:
 *     status    prev_state  xferred
 *    READY     idle                    continue
 *    READY     full(read)     >0       continue
 *    READY     full(read)     0        wake writer (state now idle).
 *    READY     empty(write)   *        wake reader.(state now idle).
 *    BLOCKED       *                   sleep
 *    CLOSED        *          >0       continue.
 *    CLOSED        *           0       discontinue I/O attempts.
 */
static int put_to_commbuf ( const void *bytes_vp, int count, 
	volatile struct commbuf *buf, int *written, 
	int *enter_state, int *exit_state )
{
    int spin_result, available, segsize, kick_reader, status;
    volatile char *dest;
    const char *bytes;

    bytes = bytes_vp;
    /*
     * Obtain mutex (spin lock).
     */
    acquire_lock ( buf );
    *enter_state = buf->state;
    /*
     * Determine amount of caller's buffer to write (segsize), smaller of
     * amount to be written, seg_limit, or space left in buffer.
     */
    if ( count > spn.seg_limit ) count = spn.seg_limit;
    available = buf->data_limit - buf->write_pos;
    segsize = (count > available) ? available : count;
    /*
     * Examine state of buffer to determine how to handle transfer, segment
     * size is updated.  buf->state can only be examine
     */
    dest = &buf->data[buf->write_pos];
    status = COMMBUF_COMPLETED;		/* Assume success */
    switch ( buf->state ) {
      case MEMSTREAM_STATE_IDLE:
	/* Nobody reading at momemt, append data if we can */
	if ( segsize == 0 ) {
	    buf->state = MEMSTREAM_STATE_FULL;
	    status = COMMBUF_BLOCKED;
	}
	break;

      case MEMSTREAM_STATE_EMPTY:
	/* Same as idle, caller will kick reader though */
	if ( segsize == 0 ) {
	    buf->state = MEMSTREAM_STATE_FULL;
	    status = COMMBUF_BLOCKED;
	}
	else buf->state = MEMSTREAM_STATE_IDLE;
	break;

      case MEMSTREAM_STATE_FULL:
	/*
	 * Should never happen, but spurious wakes can occur so force
         * another block.
         */
	segsize = 0;
	status = COMMBUF_BLOCKED;
	break;

      case MEMSTREAM_STATE_WRITER_DONE:
      case MEMSTREAM_STATE_READER_DONE:
	segsize = 0;
	status = COMMBUF_DISCARDED;		/* attempt to write to closed stream */
	break;

      case MEMSTREAM_STATE_CLOSED:
	segsize = 0;
	status = COMMBUF_ABORT;		/* attempt to write to closed stream */
	break;

      default:
	segsize = 0;
	status = COMMBUF_ABORT;
	break;
    }
    /*
     * Copy data and update write position.
     */
    if ( segsize > 0 ) {
	__MEMCPY ( (void *) dest, bytes, segsize );
	buf->write_pos += segsize;
    }
    *written = segsize;
    /*
     * Release mutex.
     */
    *exit_state =  buf->state;
    release_lock ( buf );

    return status;
}

static int get_from_commbuf ( volatile struct commbuf *buf,
	void *bytes_vp, int limit, int *retrieved, 
	int *enter_state, int *exit_state )
{
    int spin_result, available, segment, kick_reader, status;
    volatile char *src;
    char *bytes;

    bytes = bytes_vp;
    /*
     * Obtain mutex (spin lock).
     */
    acquire_lock ( buf );
    *enter_state = buf->state;
    /*
     * Determine amount of caller's buffer to fill (segment), smaller of
     * amount to be copied, seg_limit, or data available.
     */
    if ( limit > spn.seg_limit ) limit = spn.seg_limit;
    available = buf->write_pos - buf->read_pos;
    segment = (limit > available) ? available : limit;
    /*
     * Examine state of buffer to determine how to handle transfer, segment
     * size is updated.
     */
    src = &buf->data[buf->read_pos];
    status = COMMBUF_COMPLETED;
    switch ( buf->state ) {
      case MEMSTREAM_STATE_IDLE:
	/* Nobody reading at momemt, block if no data */
	if ( segment == 0 ) {
	    buf->state = MEMSTREAM_STATE_EMPTY;
	    status = COMMBUF_BLOCKED;
	}
	break;

      case MEMSTREAM_STATE_EMPTY:
	/* Should never happen since caller should have waited last time */
	status = COMMBUF_BLOCKED;
	if ( buf->writer_pid == 0 ) {
	     status = COMMBUF_DISCARDED;
	}
	break;

      case MEMSTREAM_STATE_FULL:
	/* Copy rest of data */
	if ( segment == 0 ) buf->state = MEMSTREAM_STATE_IDLE;
	break;

      case MEMSTREAM_STATE_WRITER_DONE:
	/* Peer closed pipe, flush data until empty */
	if ( segment == 0 ) status = COMMBUF_DISCARDED;
	break;

      case MEMSTREAM_STATE_READER_DONE:
	segment = 0;
	status = COMMBUF_ABORT;		/* Attempt to read from closed stream */
	break;

      case MEMSTREAM_STATE_CLOSED:
	segment = 0;
	status = COMMBUF_ABORT;		/* attempt to write to closed stream */
	break;

      default:
	segment = 0;
	status = COMMBUF_ABORT;
	break;
    }
    /*
     * Copy data and update read position.  Reset if we've read all data.
     */
    if ( segment > 0 ) {
	__MEMCPY ( bytes, (void *) src, segment );
	buf->read_pos += segment;
	if ( buf->read_pos == buf->write_pos ) {
	    buf->read_pos = 0;
	    buf->write_pos = 0;
	    if ( !buf->writer_pid ) {
		/* Writer went away, force close if he didn't do so */
		buf->state = MEMSTREAM_STATE_WRITER_DONE;
	    }
	}
    }
    *retrieved = segment;
    /*
     * Release mutex.
     */
    *exit_state = buf->state;
    release_lock ( buf );

    return status;
}
/*
 * Closedown commbuf.
 */
static int close_commbuf ( volatile struct commbuf *buf, int close_state )
{
    int prev_state;

    acquire_lock ( buf );
    /*
     * Upgrade close state to full if partner also closed.
     */
    prev_state = buf->state;
    if ( close_state == MEMSTREAM_STATE_WRITER_DONE ) {
	if ( buf->state == MEMSTREAM_STATE_READER_DONE )
	    close_state = MEMSTREAM_STATE_CLOSED;
    } else if ( close_state == MEMSTREAM_STATE_READER_DONE ) {
	if ( buf->state == MEMSTREAM_STATE_WRITER_DONE )
	    close_state = MEMSTREAM_STATE_CLOSED;
    }
    buf->state = close_state;
    release_lock ( buf );
    return prev_state;
}
/***************************************************************************/
/*
 * Process block/unblock primitives, this implementation use $HIBER/$WAKE.
 */
static int wake_peer ( memstream stream )
{
    int status;
    pid_t target;
    target = stream->is_writer ? 
	stream->buf->reader_pid : stream->buf->writer_pid;

    if ( stream->stats ) stream->stats->signals++;
    status = SYS$WAKE ( &target, 0 );
    if ( status == SS$_NONEXPR ) {
	/*
	 * Target went away.  Don't trust spinlock state.
	 */
	stream->buf->state = MEMSTREAM_STATE_CLOSED;
	return 0;
    }
    return COMMBUF_COMPLETED;
}
static int hibernate ( memstream stream )
{
    int status;

    if ( stream->stats ) stream->stats->waits++;
    status = SYS$HIBER();
    return status;
}

static int memstream_rundown ( int *exit_status, memstream *open_streams )
{
    memstream stream;
    int partial_close;
    struct commbuf *buf;
    /*
     * Traverse list of open streams and force all commbufs to closed state.
     */
    for ( stream = *open_streams; stream; stream = stream->next ) {

	*open_streams = stream->next;	/* remove from list */
	buf = stream->buf;
	if ( !buf ) continue;		/* Nothing shared, skip it */
	/*
	 * Make effort to lock commbuf so we can change state.
	 */
	if ( __LOCK_LONG_RETRY(&buf->lock.state.flag, 1) == 0 ) {
	    /* Lock failed, see if we already own it before long wait */
	    if ( stream->buf->lock.state.owner != spn.self ) {
		__LOCK_LONG(&buf->lock.state.flag);
		buf->lock.state.owner = spn.self;
	    }
	}
	/*
	 * Force state to closed, giving kick to peer if it is waiting.
	 */
	if ( stream->is_writer ) {
	    partial_close = MEMSTREAM_STATE_WRITER_DONE;
	    buf->writer_pid = 0;
	} else {
	    partial_close = MEMSTREAM_STATE_READER_DONE;
	    buf->reader_pid = 0;
	}
	switch ( stream->buf->state ) {
	  case MEMSTREAM_STATE_IDLE:
	    stream->buf->state = partial_close;
	    break;

	  case MEMSTREAM_STATE_EMPTY:
	    stream->buf->state = partial_close;
	    if ( stream->is_writer ) wake_peer ( stream );
	    break;

	  case MEMSTREAM_STATE_FULL:
	    stream->buf->state = partial_close;
	    if ( !stream->is_writer ) wake_peer ( stream );
	    break;

	  case MEMSTREAM_STATE_WRITER_DONE:
	  case MEMSTREAM_STATE_READER_DONE:
	    if ( stream->buf->state != partial_close )
		stream->buf->state = MEMSTREAM_STATE_CLOSED;
	    break;

	  case MEMSTREAM_STATE_CLOSED:
	  default:
	    break;
	}
	release_lock ( stream->buf );
    }
    return 1;
}

/************************************************************************/
/* Exported functions:
 */
int memstream_set_spinlock ( 
	int initial_retry,	/* retry limit to acquire spinlock */
	int stall_retry,	/* retry limit after stall */
	int stall_msec, 	/* Stall time if initial failure */
	int xfer_segment )	/* limit of data that can be moved while
				   holding spinlock */
{
    int status;
    /*
     * Fill in PID and adjust defaults if not initialized yet.
     */
    if ( !spn.self ) status = set_spn_self ( );
    /*
     * Overwrite spn structure with caller's arguments
     */
    spn.initial_retry = initial_retry;
    spn.stall_retry = stall_retry;
    spn.stall_msec = stall_msec;
    spn.seg_limit = xfer_segment;
    spn.stall_delta = stall_msec;	/* VMS delta time */
    /*
     * Compute VMS delta time for stall and fetch current process's PID.
     */
    if ( spn.stall_delta > 0 ) {
	spn.stall_delta = spn.stall_delta * -10000;
    } else {
	SYS$GETTIM ( &spn.stall_delta );
    }

    return status;
}
/*
 * create a new memstream and assign.
 */
memstream memstream_create ( void *shared_blk, int blk_size, int is_writer )
{
    memstream ctx;
    struct commbuf *buf;
    /*
     * Initialize buffer or verify it is supported type.
     */
    if ( blk_size <= (sizeof(struct commbuf)+MEMSTREAM_MIN_BLK_SIZE) ) {
	return 0;		/* block too small */
    }
    buf = shared_blk;
    if ( buf->fmt_version == 0 ) {
	spn.sequence++;
	buf->fmt_version = MEMSTREAM_FMT_VERSION;
	buf->ipc_version = MEMSTREAM_IPC_VERSION;
	buf->sequence = spn.sequence;
	buf->lock.state_qw = 0;
	buf->data_limit = blk_size - sizeof(struct commbuf);
	if ( (spn.seg_limit*2) > buf->data_limit ) {
	    spn.seg_limit = buf->data_limit > 2;
        }
	buf->state = MEMSTREAM_STATE_IDLE;
	buf->write_pos = 0;
	buf->read_pos = 0;

    } else if ( buf->fmt_version != MEMSTREAM_FMT_VERSION ) {
	/*
	 * Unknown version.
	 */
	return 0;
    }
    /*
     * Fill in PID for IPC signalling.
     */
    if ( !spn.self ) set_spn_self( );
    if ( is_writer ) buf->writer_pid = spn.self; 
    else buf->reader_pid = spn.self;
    /*
     * Allocate context structure for tracking stream.
     */
    ctx = malloc ( sizeof(struct memstream_context) );
    if ( !ctx ) return ctx;

    __MEMSET ( ctx, 0, sizeof(struct memstream_context) );
    ctx->is_writer = is_writer;
    ctx->buf = buf;
    /*
     * Link into open streams list for exit handler.
     */
    ctx->next = rundown.open_streams;
    rundown.open_streams = ctx;

    return ctx;
}

int memstream_control ( memstream stream, int *new_attributes,
	int *old_attributes )
{
    int attributes;
    attributes = stream->attributes;
    if  ( old_attributes ) *old_attributes = attributes;
    if ( new_attributes ) {
	/*
	 * Return error if new_attributes mask sets undefined bits.
	 */
	if ( (*new_attributes) & ~(MEMSTREAM_ATTR_NONBLOCK) ) {
	    errno = EINVAL;
	    return -1;
	}
	stream->attributes = (*new_attributes);
    }
    return 0;
}
/*
 * Link caller-supplied statistics block to memstream context.  Block is
 * zeroed.
 */
int memstream_assign_statistics ( memstream stream,
         struct memstream_stats *stats )
{
    __MEMSET ( stats, 0, sizeof(struct memstream_stats) );
    stream->stats = stats;
    return 1;
}
/*
 * write block of data to stream.
 */
int memstream_write ( memstream stream, const void *buffer_vp, int bufsize )
{
    int status, remaining, deferred_wake, seg, count, written;
    int enter_state, exit_state;
    const char *buffer;
    /*
     * Prepare for main lopp.
     */
    if ( stream->stats ) stream->stats->operations++;
    count = 0;
    deferred_wake = 0;
    buffer = buffer_vp;
    /*
     * Call put_to_commbuf as many times as needed to transfer caller's buffer.
     * Put_to_commbuf transfer at most spn.seg_limit bytes at a time.
     */
    for ( remaining = bufsize; remaining > 0; remaining -= written ) {
	status = put_to_commbuf ( buffer, remaining, stream->buf,
		&written, &enter_state, &exit_state );
	if ( stream->stats && (written>0) ) stream->stats->segments++;
	if ( status == COMMBUF_COMPLETED ) {
	    /*
	     * Skip over buffer we wrote and note if we should wake reader.
	     * Defer wake until we finish write or block to minimize
             * thrashing.
	     */
	    buffer += written;
	    if ( enter_state == MEMSTREAM_STATE_EMPTY ) deferred_wake = 1;

	} else if ( status == COMMBUF_BLOCKED ) {
	    /*
	     * Ran out of space in commbuf, sleep and retry when awakened.
             * Clear any pending wake to prevent deadlock in case where 
             * reader was also blocked (i.e. bufsize > buf->data_limit).
	     */
	    if ( stream->attributes&MEMSTREAM_ATTR_NONBLOCK ) {
		errno = EWOULDBLOCK;	/* rethink */
		return -1;
	    }
	    buffer += written;
    	    if ( deferred_wake ) {
		deferred_wake = 0;
		wake_peer ( stream );
	    }
	    hibernate ( stream );

	} else if ( status == COMMBUF_DISCARDED ) {
	    /*
	     * Broken stream, reader no longer reading
             */
	    errno = EPIPE;
	    return -1;

	} else if ( status == COMMBUF_ABORT ) {
	    /*
             * Other error.
             */
	    if ( stream->stats ) stream->stats->errors++;
            if ( deferred_wake ) wake_peer ( stream );
	    errno = EIO;
	    return -1;
	}
    }
    /*
     */
    if ( deferred_wake ) wake_peer ( stream );
    return bufsize - remaining;
}

int memstream_read ( memstream stream, void *buffer_vp, int bufsize )
{
    int status, remaining, seg, count, retrieved, enter_state, exit_state;
    int defer_wake;
    char *buffer;

    if ( stream->stats ) stream->stats->operations++;
    count = 0;
    buffer = buffer_vp;
    /*
     * Transfer bytes from shared buffer to caller's buffer in chunks until
     * bufsize moved or stream closed.
     */
    do {
	status = get_from_commbuf ( stream->buf, buffer, bufsize-count, 
		&seg, &enter_state, &exit_state );
	if ( seg > 0 ) {
	    count += seg;
	    buffer += seg;
	    if ( stream->stats ) stream->stats->segments++;
	}
	if ( status == COMMBUF_COMPLETED ) {
	    if ( enter_state == MEMSTREAM_STATE_FULL ) {
		/*
		 * Writer filled buffer and is waiting for us to flush
		 * it, which may take multiple gets.  Defer wake until
		 * commbuf_get returned all data and flips state to IDLE.
		 */
		if ( exit_state == MEMSTREAM_STATE_IDLE ) {
		    wake_peer ( stream );
		    /* If we have read something, return now to let it
		       be processed rather than wait on writer */
		    if ( count > 0 ) break;
		}
	    }
	} else if ( status == COMMBUF_BLOCKED ) {
	    /* No data, wait and retry. */
	    if ( stream->attributes&MEMSTREAM_ATTR_NONBLOCK ) {
		/* Return bytes read or EWOULDBLOCK error */
		if ( count == 0 ) {
		    errno = EWOULDBLOCK;
		    return -1;
		}
		break;
	    }
	    hibernate ( stream );
	} else if ( status == COMMBUF_DISCARDED ) {
	    if ( count > 0 ) ; else errno = EPIPE;
	    return (count > 0) ? count : -1;
	} else if ( status == COMMBUF_ABORT ) {
	    if ( stream->stats ) stream->stats->errors++;
	    errno = EIO;
	    return -1;
	}
    } while ( count < bufsize );

    return count;
}

int memstream_close ( memstream stream )
{
    int prev_state, status;
    memstream prev, cur;
    /*
     * Remove from open streams list.
     */
    prev = 0;
    for ( cur = rundown.open_streams; cur; cur = cur->next ) {
	if ( cur == stream ) break;
	prev = cur;
    }
    if ( cur ) {
	if ( prev ) prev->next = cur->next;
	else rundown.open_streams = cur->next;
    } else {
	/* Stream not found on list, abort */
	errno = EINVAL;
	return -1;
    }
    if ( stream->stats ) stream->stats->errors = spn.spinlock_fails;
    /*
     * Mark closed.
     */
    prev_state = close_commbuf ( stream->buf, stream->is_writer ? 
	MEMSTREAM_STATE_WRITER_DONE : MEMSTREAM_STATE_READER_DONE );
    if ( prev_state == MEMSTREAM_STATE_FULL ) {
	/* Someone was blocked writing to pipe, wake if not us */
	if ( !stream->is_writer && stream->buf->writer_pid ) {
	    status = wake_peer ( stream );
	    if ( (status&1) == 0 ) {
		errno = EINTR;
		return -1;
	    }
	}
    } else if ( prev_state == MEMSTREAM_STATE_EMPTY ) {
	/* Someone was blocked reading from pipe, wake if not us */
	if ( stream->is_writer && stream->buf->reader_pid ) {
	    status = wake_peer ( stream );
	    if ( (status&1) == 0 ) { errno=EINTR; return -1; }
	}
    }
    stream->buf = 0;
    return 0;
}

int memstream_destroy ( memstream stream )
{
    /*
     * Disconnect from exit handler chain and free resources.
     */
    free ( stream );
    return 1;
}
/*
 * The memstream_query function supports poll functionality on a stream.
 * The state of the stream is analyzed and returned, allowing the
 * caller to determine if a read or write to the stream would block or
 * fail.
 *
 * The arm_notification parameter, if true, forces the stream state from
 * IDLE to EMPTY if *pending_bytes is zero and we are a reader, or FULL
 * if *available_space is zero and we are a writer.  The state change will
 * force the peer to issue a wake to us if another state state occurs.
 *
 * Return status is 0 for success, -1 for failure.
 */
int memstream_query ( memstream stream, 
	int *state, 		/* stream state */
	int *pending_bytes,	/* bytes in stream ready to be read */
	int *available_space,	/* bytes that can be written without blocking */
	int arm_notification )  /* change state to empty/full if appropriate */
{
    int pending, available, enter_state, exit_state;
    struct commbuf *buf;

    if ( stream->stats ) stream->stats->operations++;
    /*
     * Lock commbuf and extract header information.
     */
    acquire_lock ( stream->buf );
    buf = stream->buf;
    enter_state = buf->state;
    available = buf->data_limit - buf->write_pos;
    pending = buf->write_pos - buf->read_pos;
    /*
     * perform arm notification.
     */
    if ( (buf->state == MEMSTREAM_STATE_IDLE) && arm_notification ) {
	if ( stream->is_writer && (available <= 0) ) {
	    /*
	     * Next write would change state to full, do it now.
	     */
	    buf->state = MEMSTREAM_STATE_FULL;

	} else if ( !stream->is_writer && (pending <= 0) ) {
	    /*
	     * Next read would reset buffer and mark it empty, do it now.
	     */
	    if ( buf->read_pos > 0 ) {
		buf->read_pos = 0;
		buf->write_pos = 0;    /* give write maximun space */
	    }
	    buf->state = MEMSTREAM_STATE_EMPTY;
	}
	exit_state = buf->state;
    } else exit_state = enter_state;
    /*
     * Release lock or buffer and return data.  Translate state.
     */
    release_lock ( buf );
    *pending_bytes = pending;
    *available_space = available;
    *state = exit_state;

    return 0;
}
