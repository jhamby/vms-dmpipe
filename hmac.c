/*
 * Simple program to demonstrate dmpipe pipe bypass routines.  Take input
 * from standard in (or file) and write hex-encoded security hash of
 * the contents.
 *
 * The program uses normal C stdio function calls for its I/O.  If compiled
 * with macro ENABLE_BYPASS defined, the pipe bypass wrapper routines are
 * called instead.
 *
 * Command line:
 *      hmac [digest-name] [filename|"(popen-command)"]
 *
 * Arguments:
 *   digest-name     Name of hash function to apply to contents of input file.
 *                   Valid hash names are those currently recognized by
 *                   OpenSSL: MD4 MD5 SHA SHA1 SHA256.  Names are insensitive
 *                   to case.  If omitted, MD5 is used.
 *
 *   filename        Name of file to digest.  If omitted, stdin is used.
 *
 * Output:
 *      [nnnn bytes]name:xxxxxxxxxxx
 *
 *       nnnn         Number of bytes read.
 *       name         Digest name used.
 *       xxxxxxxxx    Resultant digest value, encoded as hex digits.
 *
 * Author: David Jones
 * Date:   19-MAR-2014
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <openssl/evp.h>		/* OpenSSL encryption functions */

#ifdef ENABLE_BYPASS
#include "dmpipe.h"
#endif

#define OUTBUF_SIZE 500

int main ( int argc, char **argv )
{
    FILE *inp;
    unsigned int digest_vallen;
    int i, argndx, count;
    long long total_bytes;
    const EVP_MD *cur_md;
    unsigned char digest_value[EVP_MAX_MD_SIZE];
    EVP_MD_CTX digest_state;
    char *digest_name, *hex_digest;
    unsigned char buffer[OUTBUF_SIZE];
    /*
     * Initialize hash function specified by first argument on command line.
     */
    digest_name = (argc > 1) ? argv[1] : "MD5";
    OpenSSL_add_all_digests ( );
    cur_md = EVP_get_digestbyname ( digest_name );
    if ( !cur_md ) { perror ( "bad digest name" ); return 44; }
    /*
     * loop through input files specified by remaining arguments.
     */
    argndx = 2;
    do {
	/*
 	 * Open/select input file, use stdin in none specified.
 	 */
	if ( argc > argndx ) {
	    if ( argv[argndx][0] == '(' ) {
	        char *command;
	        command = strdup ( &argv[argndx][1] );
	        i = strlen ( command );
	        if ( (i > 1) && (command[i-1] == ')') ) command[i-1] = 0;
	        inp = popen ( command, "r" );
	    } else {
	        inp = fopen ( argv[argndx], "r", "rop=rah" );
	    }
	    if ( !inp ) { perror ( "input stream" ); return 44; }
        } else inp = stdin;

	/*
	 * Init digest engine and feed it contents of input file.
 	 */
	EVP_DigestInit ( &digest_state, cur_md );
	for ( total_bytes = 0;
    	    (count = fread ( buffer, 1, sizeof(buffer), inp )) > 0;
	     total_bytes += count ) {
	    EVP_DigestUpdate ( &digest_state, buffer, count );
	}
	fclose ( inp );
	/*
	 * Get digest and format in hex.  [nnnn bytes]MD5:xxxxx
	 */
	EVP_DigestFinal ( &digest_state, digest_value, &digest_vallen );
	hex_digest = malloc ( digest_vallen*2 + 1 );
	for ( i = 0; i < digest_vallen; i++ ) {
	    sprintf ( &hex_digest[i*2], "%02x", digest_value[i] );
	}
	printf ("[%lld bytes]%s:%s\n", total_bytes, digest_name, hex_digest);
	argndx++;
   } while ( argndx < argc );
    return 1;
}
