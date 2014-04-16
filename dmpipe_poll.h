#ifndef DMPIPE_POLL_H
#define DMPIPE_POLL_H
/*
 * dmpipe_poll module supports poll() and select() functions inside of DMPIPE.
 * Taking over poll() function from the RTL allows proper polling of the
 * pipe devices being bypassed.
 */
#include <types.h>
#include <poll.h>
#include "dmpipe_bypass.h"

typedef struct dm_poll_extension *dm_poll_track;   /* should be opaque? */
typedef int (*dm_device_query_function) ( dm_poll_track pt );
typedef int (*dm_device_cancel_function) ( dm_poll_track pt );

struct dm_poll_extension {
    struct dm_poll_extension *next_ext;		/* list for exit handler */
    struct dm_poll_extension *next_member;	/* current poll group list */
    unsigned int incarnation;			/* prevent adding twice */
    dm_bypass bp;    
    int fd;
    int fcntl_flags;
    unsigned short chan, sdc_chan;
    union {
      struct {
	unsigned short status, count;
        unsigned int info;
      } any;
      struct {
	unsigned short status, count;
	pid_t pid;
      } mbxio;
      struct {
	unsigned short status, count;
	unsigned int pending_bytes;
      } mbxsense;
    } iosb;
    int dev_type;

    int select_mask;		/* events of interest if select() group */
    int select_event;		/* Filled in during scan */
    struct pollfd *filedes;	/* points into poll() call arrray to allowed
				   update of revents */
    char device_name[32];
    dm_device_query_function device_query;	/* device-specific poll */
    dm_device_cancel_function device_cancel;	/* device-specific poll */
};
/*
 * A group collects several FDs that are polled at once.
 */
struct dm_poll_group {
    int type;			/* poll or select */
    unsigned int incarnation;
    int timeout_ms;
    int timeout_expired;
    long long timeout_period;	/* VMS delta time */
    long long poll_period;	/* VMS delta time */
    /*
     * Count number of each type of file in group (permitting optimizations
     * if all of one type).
     * 0-unknown, 1-bypassed pipes, 2-pipes, 3-terminals, 4-sockets, 5-x11
     */
#define DMPIPE_POLL_DEV_UNKNOWN 0
#define DMPIPE_POLL_DEV_BYPASS 1
#define DMPIPE_POLL_DEV_PIPE 2
#define DMPIPE_POLL_DEV_TTY 3
#define DMPIPE_POLL_DEV_SOCKET 4
#define DMPIPE_POLL_DEV_X11 5
    int histogram[6];
    int total_fds;		/* number of FDs in group */
    /*
     * Try to keep order files are polled the same as in fd array.
     */
    struct dm_poll_extension *first_member;
    struct dm_poll_extension *last_member;

};
#define POLL_TYPE_POLL 1
#define POLL_TYPE_SELECT 2
/*
 * Function prototypes.  Usage is to create a poll group, add the
 * fds to be polled to it (add will screen out fds open on files and
 * devices we don't support), then call scan to actually do the polling.
 * Finally, the group is torn down (possibly resources released) by
 * calling dm_poll_group_end().
 */
dm_poll_track dm_poll_create_track ( int fd, dm_bypass bp, int fcnt_flags );
int dm_poll_rundownn_track (dm_poll_track *pt_ptr );

int dm_poll_group_begin ( struct dm_poll_group *group, int timeout, int type );
int dm_poll_group_add_pollfd 
	( struct dm_poll_group *group, struct pollfd *filed, dm_poll_track pt );
int dm_poll_group_add_selectfd 
	( struct dm_poll_group *group, int fd, int select_mask, dm_poll_track pt );
#define DM_POLL_SELECT_READ 1
#define DM_POLL_SELECT_WRITE 2
#define DM_POLL_SELECT_EXCEPT 4

int dm_poll_scan_group ( struct dm_poll_group *group );

int dm_poll_group_end ( struct dm_poll_group *group );

#endif
