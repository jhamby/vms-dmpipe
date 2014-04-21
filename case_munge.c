/*
 * Simple program to demonstrate dmpipe pipe bypass routines. Take input
 * from standard in (or file) and write to standard out with case changes
 * specifed by argv[1]:
 *    -1             Make no case change.
 *     0             force all to lower case.
 *     1             force first character of each word to upper case.
 *     2             force all to upper case.
 *
 * The program uses normal C stdio function calls for its I/O.  If compiled
 * with macro ENABLE_BYPASS defined, the pipe bypass wrapper routines are
 * called instead.
 *
 * Command line:
 *    case_munge mode [filename ["popen-command"]]
 *
  * Arguments:
 *     mode		number inidating case-change style.
 *
 *     filename		File to read, if missing or specified as -, stdin
 *                      is read.
 *
 *     popen-command    Command to spawn to receive case-munged output via
 *                      pipe (created with popen() call).  If absent output is
 *			sent to stdout.
 *
 * Output:
 *     The program outputs a case-changed version of the input followed
 *     by a perforance staticstics of the form:
 *
 *     (ELAPSED: 0 00:00:00.00  CPU: 0:00:00.00  BUFIO: 0  DIRIO: 0  FAULTS: 0)
 *
 *      If stdout is NOT a terminal (e.g. a pipe), the program attempts to
 *      write the performance information to TT: rather than stdout.  Programs
 *      recieving the output data therefore won't see the show_timer line.
 *
 * Author: David Jones
 * Date:   18-MAR-2014 
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unixio.h>		/* for isatty() function */
#include <lib$routines.h>
#include <descrip.h>
    
#ifdef ENABLE_BYPASS
#include "dmpipe.h"
#endif

#define OUTBUF_SIZE 5000
/*
 * Define out own output routine for LIB$SHOW_TIMER() so we can output
 * directly to the terminal and not stdout if it has been re-directed to
 * a pipe.
 */
static long timer_data;		/* context handle for LIB$xxx_TIMER functions */
static int timer_report ( struct dsc$descriptor_s *line_dx, FILE *fp )
{
    unsigned short line_len;
    char *line_chars, *line;
    int status;

    status = LIB$ANALYZE_SDESC ( line_dx, &line_len, &line_chars );
    if ( (status&1) == 0 ) return status;

    while ( (line_len > 0) && line_chars[0] == ' ' ) {
	line_chars++; --line_len;
    }
    line = malloc ( line_len+10 );
    memcpy ( line, line_chars, line_len );
    line[line_len] = 0;
    while ( (line_len > 0) && line[line_len-1] == ' ' ) {
	line_len--; line[line_len] = 0;   /* trim trailing blanks */
    }
    while ( strncmp(line, "ELAPSED:  ", 10) == 0 ) {
	memmove ( &line[8], &line[9], line_len-8 );
	line_len--;
    }
    fprintf ( fp, "(%s)\n", line );
    free ( line );

    return 1;
}
/***********************************************************************/
/* Main program.
 */
int main ( int argc, char **argv )
{
    FILE *inp, *out, *tt;
    int i, c, mode, count, in_word;
    unsigned char buffer[OUTBUF_SIZE];
    /*
     * Initialize RTL timer routines and select file it is to use.
     */
    mode = 0;
    if ( isatty ( 1 ) ) tt = stdout;
    else {
	tt = fopen ( "TT:", "w" );
	if ( !tt ) tt = stdout;
    }

    LIB$INIT_TIMER(&timer_data);
    /*
     * Process command line arguments and open input and output streams.
     */
    if ( argc > 1 ) mode = atoi(argv[1]);
    if ( argc > 2 ) {
	if ( strcmp(argv[2],"-") == 0 ) inp = stdin;
	else inp = fopen ( argv[2], "r", "rop=rah" );
    } else inp = stdin;
    if ( !inp ) { perror ( "fopen fail" ); return 44; }

    if ( argc > 3 ) {
	out = popen ( argv[3], "w" );
    } else out = stdout;
    if ( !out ) { perror ( "popen fail" ); return 44; }
    /*
     * Main loop.  Simple state machine (really just busy work to have
     * something to output).
     */
    count = 0;		/* bytes in output buffer */
    in_word = 0;	/* previous character was not whitespace */
    for ( c = fgetc ( inp ); c != EOF; c = fgetc ( inp ) ) {
        /*
         * Change case of input character or not based on mode and
         * simple state machine (really just busy work).
         */
	if ( mode < 0 ) c = c;
	else if ( mode == 0 ) c = tolower ( c );
	else if ( mode == 2 ) c = toupper ( c );
	else if ( isspace ( c ) || ispunct ( c ) ) {
	    in_word = 0;
	} else if ( in_word ) {
	    if ( mode != 1 ) c = tolower ( c );
        } else {
	    in_word = 1;
	    c = toupper ( c );
	}
        /*
         * Save output characters in buffer and write out in chunks.
         */
	buffer[count++] = (unsigned char) c;
        if ( count >= sizeof(buffer) ) {
	    count = fwrite ( buffer, count, 1, out );
	    if ( count <= 0 ) break;
	    count = 0;
	}
    }
    /*
     * Flush any remaining output characters and clean up.
     */
    if ( count > 0 ) {
	fwrite ( buffer, count, 1, out );
    }
    if ( inp != stdin ) fclose ( inp );
    if ( out != stdout ) fclose ( out );

    LIB$SHOW_TIMER(&timer_data, 0, timer_report, tt );

    return 1;
}
