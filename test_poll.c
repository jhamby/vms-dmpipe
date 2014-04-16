/*
 * Test poll operation with DMPIPE pipes.  The parent process spawns itself
 * as 1 or more child processes that output to a pipe several lines of text
 * at fixed intervals.  The parent uses dm_poll() to wait for and display
 * the child input as it arrives.
 *
 * The first child is created using vfork/exec and dm_pipe() is used to
 * create the pipe.  This first child inherits the open file descriptor and
 * and writes to it.  The other child processes are created with dm_popen()
 * and write to stdout and stderr.
 *
 * Several aspects of DMPIPE are demonstrated:
 *   - DMPIPE functions are called directly by the parent.  Children will
 *     call them indirectly as macro defitions for the crtl functions unless
 *     program is compiled with DM_NO_CRTL_WRAP is defined.
 *
 *   - The parent sets the pipe file descriptors non-blocking with dm_fcntl().
 *
 *   - Secondary children output line to both stdout and stderr, DMPIPE
 *     merges the output.
 *
 *   - The parent alternates whether each child's pipe is read with dm_read() 
 *     pr dm_fgets().
 *
 * Command line:
 *    (parent): test_poll poll_timeout child-1_interval [[child-2_interval...]]
 *
 *    (child):  test_poll interval
 *
 * Arguments:
 *    poll_timeout	Timeout time parent uses for dm_poll() call, in
 *                       milliseconds.
 *
 *    interval          After startup announcement, time in seconds that
 *                      child waits between each of the 3 subsequent lines
 *                      of ouput.
 *
 *    child-n_interval	Interval argument to use for child n.
 *
 * Author: David Jones
 * Date:   27-MAR-2014
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <starlet.h>

#define ENABLE_BYPASS
#ifdef ENABLE_BYPASS
#include "dmpipe.h"
#endif
int decc$write_eof_to_mbx(), decc$fprintf(), decc$printf();
#define decc$fprintf fprintf
#define decc$printf printf

struct child_control {
    FILE *fp;		/* if not first slave */
    int fd[2];		/* if first slave */

    pid_t pid;
    int status;
};
/*
 * Display 3 elapsed times in seconds:
 *    Time between t2 and t0 (poll() return since program start)
 *    time between t2 and t1 (time spent in poll call)
 *    time between t3 and t2 (time since last active)
 */
static char *timestamp_deltas ( long long tstamp[4] )
{
    static char display[80];
    long long delta_t2_t0, delta_t2_t3, delta_t2_t1;
    double elapsed_from_start, call_time, act_time;

    delta_t2_t0 = (tstamp[2] - tstamp[0])/10000;	/* milliseconds */
    delta_t2_t1 = (tstamp[2] - tstamp[1])/10000;
    delta_t2_t3 = (tstamp[2] - tstamp[3])/10000;
    elapsed_from_start = delta_t2_t0;
    call_time = delta_t2_t1;
    act_time = delta_t2_t3;

    sprintf ( display, "elapsed: %10.3f sec|%7.3f| idle%7.3f sec\n", elapsed_from_start/1000.0,
	call_time/1000.0, act_time/1000.0 );
    return display;
}
/*
 * Master routine to monitor children and report data the send as it come
 * in.  Directly call the DMPIPE wrapper functions (dm_ prefix).
 */
monitor_children ( struct child_control *slave, int slave_count, int timeout )
{
    int i, slaves_done, active, count, j, old, timeout_count;
    time_t now;
    struct pollfd *poll_desc;
    long long tstamp[4];
    char buffer [44];
    char bigbuf[10000];
    /*
     * Setup array for polling.  Size it to the number of slaves plus 1
     * in order to have a tty device be polled as well.
     */
    poll_desc = calloc ( sizeof(struct pollfd), slave_count+1 );
    for ( i = 0; i < slave_count; i++ ) {
	poll_desc[i].fd = slave[i].fd[0];
	poll_desc[i].events = POLLHUP | POLLIN;
	slave[i].status = dm_fcntl ( slave[i].fd[0], F_SETFL, O_NONBLOCK );
#ifdef DM_NO_CRTL_WRAP
	slave[i].status = fcntl ( slave[i].fd[0], F_SETFL, O_NONBLOCK );
	slave[i].status = -1;
#endif
    }
    poll_desc[slave_count].fd = 0;	/* standard input */
    poll_desc[slave_count].events = POLLHUP | POLLIN;
    poll_desc[slave_count].revents = 0;
    /*
     *  Loop until broken pipes on all slaves.
     */
    timeout_count = 0;
    SYS$GETTIM(&tstamp[0]);  tstamp[3] = tstamp[0];
    for ( slaves_done = 0;  slave_count > slaves_done; ) {
	SYS$GETTIM ( &tstamp[1] );
	active = dm_poll ( poll_desc, slave_count+1, timeout );
	SYS$GETTIM ( &tstamp[2] );

	if ( active == 0 ) {
	    /* if ( timeout_count == 0 ) printf ( 
		    "\npoll() timeout,                 %s", 
		timestamp_deltas(tstamp) ); */
	    timeout_count++;
	} else if ( active > 0 ) {
	    printf ( "\npoll() events: %d, timeouts %d, %s", 
		active, timeout_count, timestamp_deltas(tstamp) );
	    SYS$GETTIM ( &tstamp[3] );
	    timeout_count = 0;
	    /*
	     * Test pipes.
	     */
	    for ( i = 0; i < slave_count; i++ ){
		if ( poll_desc[i].fd < 0 ) continue;

		if ( poll_desc[i].revents ) printf ( 
		    "  (parent) slave[%d], fd=%d(%d) revents: 0%o\n", i, 
		    poll_desc[i].fd, slave[i].status, poll_desc[i].revents );

		if ( poll_desc[i].revents & POLLHUP ) {
		    printf ( "    (parent) process hangup, slave %d, pid %x\n", i, slave[i].pid );
		    poll_desc[i].fd = -1;	/* ignore in future polls */
		    slaves_done++;

		} else if ( poll_desc[i].revents & POLLIN ) {
		    /* printf("    (parent) slave[%d]-in ready, fd: %d, pid: %x\n",
			 i, slave[i].fd[0], slave[i].pid ); */
		    do {
		      if ( (i & 1) && slave[i].fp ) { /* ------------*/
			char *line; int ecode;
			strcpy ( bigbuf, "bigbuf-init\n" );
			line = dm_fgets ( bigbuf, 10000, slave[i].fp );
			if ( !line ) {
			    ecode = errno;
			    printf ( "    (parent) slave[%d] errno: %d '%s'\n",
					i, ecode, (ecode==EWOULDBLOCK) ?
					"[EWOULDBLOCK]" : strerror(ecode) );
			    if ( ecode != EWOULDBLOCK ) {
			        poll_desc[i].fd = -1; slaves_done++;
			    }
			    break;
			} else {
			    printf ( "    (parent) slave[%d] gets(%d): %s", i, 
				strlen(line), line);
			}
		      } else {    /*-------------------------------------*/
		      count = dm_read ( slave[i].fd[0], buffer, 40 );
		      if ( count > 0 ) {
			buffer[count] = 0;
		  	for(j=0;j<count;j++) if (buffer[j]=='\n') buffer[j]='.';
		        printf ( "    (parent) slave[%d] read: %d/40, '%s'\n", 
				i, count, buffer );
		      } else if ( count == 0 ) {
			printf ( "    (parent) slave[%d] EOF in=%d, hup=%d\n", 
				i, POLLIN, POLLHUP );
			poll_desc[i].fd = -1;
			slaves_done++;
			break;

		      } else if ( count < 0 ) {
			printf ( "    (parent) slave[%d] read error: %d '%s'\n", 
				i, errno, (errno==EWOULDBLOCK)?
				"(EWOULDBLOCK)" : strerror(errno) );
			if ( errno != EWOULDBLOCK ) {
				/* poll_desc[i].fd = -1; */
			}
			break;
		      }
		      }
		    } while ( slave[i].status != (-1) );
		} else if ( poll_desc[i].revents & POLLNVAL ) {
		    /*
		     * Error, take out of list.
		     */
		    printf ( "    (parent) pdesc[%d], fd %d reported as invalid!\n",
				i, poll_desc[i].fd );
		    poll_desc[i].fd = -1;
		    slaves_done = slave_count;
	       	    break;
		}
	    }
	    /*
	     * Test standard in for ready.
	     */
	    if ( poll_desc[slave_count].revents & POLLIN ) {
		char buffer[2048];
		printf ( "  Input: " );
		if ( !fgets ( buffer, sizeof(buffer), stdin ) ) {
		    poll_desc[slave_count].fd = -1;
		}
		printf ( "\n echo: %s", buffer );
	    }
	} else {
	    perror ( "poll() error" );
	}
    }

    return;
}
/***************************************************************************/
/* Main function for child processes.
 */
static void child_mode ( int arg_c, char **arg_v )
{
    char message[80];
    char *child_out_fd, line[80];
    FILE *tty;
    pid_t self;
    int out_fd, count, delay, i;

    tty = fopen ( "TT:", "w" );
    /*
     * See if we are the initial (vfork/exec) child or a scondary (popen)
     * child by checking for special variable added to environment
     * telling us what file descriptor number to use.
     */
    child_out_fd = getenv ( "CHILD_OUT_FD" );
    self = getpid();
    if ( child_out_fd ) {
	/*
	 * Decode the fd and write out first message immediately.
	 */
	out_fd = atoi ( child_out_fd );
	sprintf ( message, "Primary child, pid=%X, output_fd=%d\n", 
		self, out_fd );
	count = write ( out_fd, message, strlen(message) );

    } else {
	/*
	 * For popen()-created processes, outpt goes to stdout.
	 * write first line to stderr to confirm DMPIPE handles
	 * multiple opens to same mailbox.
	 */
	out_fd = fileno(stdout);	/* popen openned pipe on stdout */
	fprintf ( stderr, "secondary child starting, pid=%X\n", self );
    }
    /*
     * Write another line to start off to see how the master process reads it.
     */
    sprintf ( line, "child %X startup done\n", self );
    count = write ( out_fd, line, strlen(line) );
    if ( count < 0 ) fprintf ( tty, "Write error" );
    /*
     * Send out three messages and exit.
     */
    delay = atoi ( arg_v[1] );
    for ( i = 1; i <= 3; i++ ) {
	if ( sleep ( delay ) != 0 ) perror ( "sleep failed" );
	sprintf ( message, "child %X delay %d done\n", self, i );
	count = write ( out_fd, message, strlen(message) );
	if ( count < 0 ) fprintf ( tty, "child Write error" );
    }
#ifdef DM_NO_CRTL_WRAP
    /*
     * Traditional CRTL pipes need help recognizing when we are done.
     */
    count = decc$write_eof_to_mbx ( out_fd );
    fprintf(tty, "child %X sent eof_to_mbx: %d\n", self, count );
#endif
}
/****************************************************************************/
int main ( int argc, char **argv, char **envp )
{
    int child_count, i, timeout;
    struct child_control *child;
    char child_command[800];
   /*
    * Determine if we are master process or child by counting command
    * line arguments.
    */
    if ( argc > 2 ) {
	/*
	 * allocate array of child control blocks.
	 */
	child_count = argc - 2;
	child = calloc ( sizeof(struct child_control), child_count );
	timeout = atoi ( argv[1] );
	if ( (timeout == 0) && (argv[1][0] != '0') ) timeout = 3000;
#ifdef DM_NO_CRTL_WRAP
	printf ( "Children will not use DMPIPE\n" );
#else
	printf ( "Children are using DMPIPE wrapper functions\n" );
#endif
	/*
	 * first child is vfork/exec.
	 */
	if ( 0 == dm_pipe ( child[0].fd ) ) {
	    /* Make the write fd from the pipe a known value */
	    child[0].fd[1] = dm_dup2 ( child[0].fd[1], 53 );
	    child[0].pid = vfork ( );
	    if ( child[0].pid == 0 ) {
		/*
		 * exec the command give child extra environment variable.
		 */
		char *child_argv[3], *child_envp[20];
		child_argv[0] = argv[0];
		child_argv[1] = argv[2];
		child_argv[2] = 0;
		for (i = 0; (i < 18) && envp[i]; i++) child_envp[i] = envp[i];
		child_envp[i] = malloc ( 80 );
		sprintf ( child_envp[i], "CHILD_OUT_FD=%d", 53 );
		child_envp[i+1] = 0;

		if ( 0 > execve ( argv[0], child_argv, child_envp ) ) {
		    perror ( "execve failed" );
		}
		return 20;
	    }
	    printf ( "(parent) child[0] created, pid %x\n", child[0].pid );
	} else {
	    perror ( "pipe() failed" );
	    return 44;
	}
	close ( child[0].fd[1] );
	/*
	 * Remaining children created with popen().
	 */
	for ( i = 3; i < argc; i++ ) {
	    sprintf ( child_command, "mcr %s %s", 
		argv[0], argv[i] );
	    child[i-2].fp = dm_popen ( child_command, "r" );
	    if ( child[i-2].fp ) {

		child[i-2].fd[0] = fileno(child[i-2].fp);
		printf ("(parent) child[%d].fd = %d\n", i-2, child[i-2].fd[0]);
	    } else {
		perror ( "popen() failed" ) ;
	    }
	}
	/*
	 * Read responses from children.
	 */
	monitor_children ( child, child_count, timeout );

    } else if ( argc == 2 ) {
	/* 
	 * One argument means we are a child 
	 */

	child_mode ( argc, argv );
    } else {
	printf ( "usage: test_poll timeout child-n-delay\n" ) ;
    }
    return 0;
}
