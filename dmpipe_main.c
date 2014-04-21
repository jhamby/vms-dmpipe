/*
 * This file is included by dmpipe.h when the DM_WRAP_MAIN macro is defined.
 * Applications that use dmpipe.h should compile the source module containing
 * the main() function -D DM_WRAP_MAIN (/define=DM_WRAP_MAIN).  The
 * applications main function is renamed, via macro, to dmpipe_wrapped_main 
 * and the main() function defined in this files becomes the programs'
 * main entry point.
 *
 * The purpose of the wrapper is to set up a more robust environment for
 * the use of dmpipe's memory-based pipe bypass.
 *
 * Author:  David Jones
 * Date:    17-APR-2014
 * Revised: 19-APR-2014		Fix isapipe calls and change to dm_isapipe().
 */
#include <signal.h>
#include <errnodef.h>
#include <unixio.h>
/*
 * Signal handler for SIGPIPE,1 signals.  dmpipe wrappers generate,
 * via gsignal(), a SIGPIPE whenever a write to a bypassed pipe fails
 * be it has been closed by the receiver.  Only action is to exit
 * with C$_SIGPIPE condition code, which is better than the default
 * action of raising a C$_SIGPIPE exception (traceback and all).
 */
static void exit_on_EPIPE ( int sig, ... )
{
    /*
     * Simply exit with C$_SIGPIPE status.
     */
    exit ( C$_SIGPIPE );
}

/*
 * Wapper main.
 */
int main ( int argc, char **argv, char **envp )
{
    int status, bp_status, dm_wrapped_main();
    struct pollfd poll_set[2] = { { -1, POLLIN, 0 }, { -1, POLLOUT, 0 } };
    /*
     * When stdin is a pipe, do a poll to force init of bypass so upstream
     * writers get a broken pipe instead of DCL reading it.
     */
    if ( dm_isapipe(0,&bp_status) ) poll_set[0].fd = 0;
    if ( dm_isapipe(1,&bp_status) ) poll_set[1].fd = 1;
    if ( (poll_set[0].fd==0) || (poll_set[1].fd==1) ) dm_poll(poll_set, 2, 0);
    /*
     * Set up signal handler to catch SIGPIPE and call the real main() routine,
     * passing along whatever status value it returnes.
     */
    ssignal ( SIGPIPE, exit_on_EPIPE );
    status = dm_wrapped_main ( argc, argv, envp );

    return status;
}
/*
 * Rename the real main() function to dm_wrapped_main.  Note that the
 * real main needs an explicit return value.
 */
#define main dm_wrapped_main
