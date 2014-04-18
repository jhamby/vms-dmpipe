#ifndef DMPIPE_BYPASS_H
#define DMPIPE_BYPASS_H
/*
 * Define data structures and prototypes for functions used to emulate
 * pipes via shared memory that can bypass mailbox I/O.
 */
#include "memstream.h"

#define DM_BYPASS_BUFSIZE 1024
#define DM_BYPASS_HINT_STARTING 1
#define DM_BYPASS_HINT_READS 2
#define DM_BYPASS_HINT_WRITES 4
typedef struct dm_bypass_ctx *dm_bypass;   /* opaque type */
/*
 * Initialize returns a context and sets flags.
 */
dm_bypass dm_bypass_init ( int fd, int *flags );
int dm_bypass_shutdown ( dm_bypass bp );
/*
 * bypass_negotiate is called prior starting an I/O and returns updated
 * flags value.  Op specifies either "w" or "r" indicating caller wishes
 * to write or read a number of bytes.  Flag bits:
 *    <0>    Negotiation in progress.
 *    <1>    read bypass exstaablished call dm_bypass_read to getting data.
 *    <2>    write bypass established call dm_bypass_write to pu data.
 */
int dm_bypass_startup_stall ( int intitial_flags, dm_bypass bp, char *op,
	int fcntl_flags );
int dm_bypass_negotiate (int initial_flags, dm_bypass bp, char *op, int bytes);

int dm_bypass_read(dm_bypass bp, void *buffer, size_t nbytes, 
	size_t min_bytes, int *expedite_flag );
int dm_bypass_write ( dm_bypass bp, const void *buffer, size_t nbytes );
/*
 * dm_bypass_stderr_propagate() is called by parent to convey its stderr
 * to any children whose stdout will be a pipe.  Child calls
 * dm_bypass_stderr_recover() to redirect stderr to the original parent's
 * rather than stdout.
 */
int dm_bypass_stderr_propagate(void);
int dm_bypass_stderr_recover(pid_t parent);
/*
 * Give direct access to memstream for polling.  A file open read/write
 * can have 2 streams.
 */
int dm_bypass_current_streams ( dm_bypass bp, 
	memstream *rstream, memstream *wstream  );
/*
 * Callback function for C$DOPRINT family of routines.
 */
int dm_bypass_doprint_cb ( dm_bypass bp, void *buffer, int len );
#endif
