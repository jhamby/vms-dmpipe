#ifndef MEMSTREAM_H
#define MEMSTREAM_H
/*
 * Define interface for sending data between processes via shared memory:
 *
 *    typedef memstream;	Handle for stream context.
 *    memstream_create();       Create new memstream.
 *    memstream_write();        Write data bytes to stream.
 *    memstream_read();         Read data bytes from stream.
 *    memstream_close();        Shutdown stream.
 *    memstream_destroy();      Free memstream resources.
 *
 *    memstream_assign_statistics();
 *
 * Caller is responsible for creating a shared memory region between
 * the processes using the stream.
 */
typedef struct memstream_context *memstream;
struct memstream_stats {
    int operations;		/* writes or reads calls */
    int errors;
    int segments;
    int waits;
    int signals;
};
/*
 * Allow tuning of parameters for spinlock.  Glocal setting.
 */
int memstream_set_spinlock ( 
	int initial_retry,	/* retry limit to acquire spinlock */
	int secondary_retry,	/* retry limit after stall */
	int stall_msec, 	/* Stall time if initial failure */
	int xfer_segment );	/* limit of data that can be moved while
				   holding spinlock */

memstream memstream_create ( void *shared_blk, int blk_size, int is_writer );
#define MEMSTREAM_MIN_BLK_SIZE 512

int memstream_assign_statistics ( memstream stream,
	 struct memstream_stats *stats );

int memstream_write ( memstream stream, const void *buffer, int bufsize );

int memstream_read ( memstream stream, void *buffer, int bufsize );

int memstream_control ( memstream stream, int *new_attributes, 
	int *old_attribtes );
#define MEMSTREAM_ATTR_NONBLOCK 1

int memstream_query ( memstream stream, 
	int *state, 		/* stream state */
	int *pending_bytes,	/* bytes in stream ready to be read */
	int *available_space,	/* bytes that can be written without blocking */
	int arm_notification ); /* change state to empty/full if appropriate */

int memstream_close ( memstream stream );

int memstream_destroy ( memstream stream );

#endif
