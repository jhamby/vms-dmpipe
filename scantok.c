/*
 * Support routine for implementing scanf engine.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define DM_INBUF_IOSIZE 1024
#define DM_INBUF_LOOKBACK 16
#define DM_INBUF_BUFSIZE (DM_INBUF_IOSIZE+DM_INBUF_LOOKBACK)

struct stream_buffer {
    FILE *fp;
    int rpos;			/* position of next character to read */
    int length;			/* position of first unread char */
    char buffer[DM_INBUF_IOSIZE];
    char eob[4];
};
/*
 * Ensure at least bytes_needed number of bytes available int
 * stream buffer.  Return zero for success or -1 for failure.
 */
static int load_inbuf ( struct stream_buffer *inbuf, int bytes_needed )
{
    int available, count, potential;
    available = inbuf->length - inbuf->rpos;
    while ( available < bytes_needed ) {
	potential = sizeof(inbuf->buffer) - inbuf->rpos;
	if ( potential < bytes_needed ) {
	    memmove(inbuf->buffer, &inbuf->buffer[inbuf->rpos], available );
	   inbuf->length = available;
	   inbuf->rpos = 0;
 	}
	count = fread ( &inbuf->buffer[inbuf->length], 1,
		sizeof(inbuf->buffer)-inbuf->length, inbuf->fp );
	if ( count > 0 ) inbuf->length += count;
	else return -1;

	available = inbuf->length - inbuf->rpos;
    }
    return 0;
}
/*
 * Base routine.  Return value is length number of characters transferred.
 * Codes:
 *    0     Use characters in scan set.
 *    1     Skip whitespace and stop, next character read will be non-space.
 *    2     Don't skip whitespace, token is non-WS characters.
 *    3     Skip leading whitespace, load 
 *    4     Complement characters in scan set.
 */
static int get_token ( struct stream_buffer *stream, int code, char *scanset,
	char *outbuf, int bufsize, char **term_char )
{
    int outlen, span;
    /*
     * If bit 0 of code set, skip whitespace.
     */
    *term_char = 0;
    outlen = 0;
    if ( code & 1 ) {
	if ( load_inbuf ( stream, 1 ) < 0 ) return -1;
	while ( isspace(stream->buffer[stream->rpos]) ) {
	    stream->rpos++;
	    if ( stream->rpos >= stream->length ) {
		if ( load_inbuf ( stream, 1 ) < 0 ) return -1;
	    }
	}
    }
    /*
     * Simple token.
     */
    if ( code & 2 ) {
	do {
	    /*
	     * Ensure at lease one characetr in stream buffer and
	     * count number of non-space chars starting at stream->rpos.
	     */
	    if ( load_inbuf ( stream, 1 ) < 0 ) return -1;
	    stream->buffer[stream->length] = '\0';
	    span = strcspn ( &stream->buffer[stream->rpos], " \t\r\n" );
	    /*
	     */
	    if ( span > 0 ) {
		if ( span > (bufsize-outlen) ) {
		    span = bufsize - outlen;
		}
		memcpy (&outbuf[outlen], &stream->buffer[stream->rpos], span);
		stream->rpos += span;
		outlen += span;
		if ( stream->buffer[stream->rpos] ) {
		    *term_char = &stream->buffer[stream->rpos];
		    break;
		}
	    } else {
		/* character at rpos was whitespace. */
		*term_char = &stream->buffer[stream->rpos];
		break;
	    }
	} while ( outlen < bufsize );
    } else if ( !(code&4) ) {
	/*
	 * Scan set argument defines characters comprising token.
	 */
	char c;
 	while ( outlen < bufsize ) {
	    if ( stream->rpos >= stream->length ) {
		if ( load_inbuf ( stream, 1 ) < 0 ) {
		    if ( outlen > 0 ) return outlen;
		    return -1;
		}
	    }
	    c = stream->buffer[stream->rpos];
	    if ( !strchr ( scanset, c ) ) {
		*term_char = &stream->buffer[stream->rpos];
		break;
	    }
	    stream->rpos++;
	    outbuf[outlen++] = c;
	}
	
    } else {
	/*
	 * Scanset argument defines delimiter tokens.
	 */
	char c;
 	while ( outlen < bufsize ) {
	    if ( stream->rpos >= stream->length ) {
		if ( load_inbuf ( stream, 1 ) < 0 ) {
		    if ( outlen > 0 ) return outlen;
		    return -1;
		}
	    }
	    c = stream->buffer[stream->rpos];
	    if ( strchr ( scanset, c ) ) {
		*term_char = &stream->buffer[stream->rpos];
		break;
	    }
	    stream->rpos++;
	    outbuf[outlen++] = c;
	}
	
    }
    return outlen;
}


int main ( int argc, char **argv )
{
    FILE *inp;
    char temp[80], *term_char, *tok[1000];
    int matched, tcount, i, len;
    struct stream_buffer input;

    if ( argc > 1 ) inp = fopen ( argv[1], "r" );
    else inp = 0;

    if ( !inp ) {
	printf ( "error opening input file\n" );
        return 44;
    }
    /*
     * Use fscanf to read 1000 tokens from input file.
     */
    for ( tcount = 0; tcount < 1000; tcount++ ) {
	strcpy ( &temp[42], "qqqqqqq" );
	matched = fscanf ( inp, "%24s", temp );
	if ( matched != 1 ) break;
	tok[tcount] = strdup ( temp );
	i = strlen(tok[tcount]);
	if ( i > 3 ) printf ( "tok[%d] = '%s' (%d)\n", tcount, tok[tcount],i );
    }
    printf ( "Tokens read: %d\n", tcount );
    /*
     * Reset file and read tokens with private routine.
     */
    fseek ( inp, 0, SEEK_SET );
    memset ( &input, 0, sizeof(input) );
    input.fp = inp;

    for ( i = 0; i < tcount; i++ ) {
	len = get_token ( &input, 3, 0, temp, 24, &term_char );
	if ( len < 0 ) break;
	if ( !term_char ) printf ( "tok %d truncated\n", i );

	if ( (len !=strlen(tok[i])) || 
		strncmp ( tok[i], temp, len ) ) {
	    temp[len] = '\0';
	    printf ( "Tok[%d] = '%s' mismatches '%s' (%d)\n", 
		i, tok[i], temp,len);
	}
    }
    /*
     * Read alphanumeric keywords.
     */
    if ( argc < 3 ) return 0;

    for ( i = 2; (i+1) < argc; i+=2 ) {
	int num, count;
	count = sscanf ( argv[i+1], argv[i], &num );
	printf ( "scanf('%s','%s',&num) = %d, num=%d\n",
	    argv[i+1], argv[i], count, num );
    }
    if ( argc > 3 ) return 0;

    fseek ( inp, 0, SEEK_SET );
    input.rpos = input.length = 0;
    while ( (len = get_token ( &input, 1, 
	    "012345679abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ$_",
	    temp, sizeof(temp)-1, &term_char )) >= 0 ) {
	temp[len] = '\0';
	if ( term_char ) {
	    printf("token: '%s', len=%d, term: 0x%x\n", temp, len, *term_char);
	    input.rpos++;
	} else printf ( "token: '%s', len=%d\n", temp,len );
    }
    return 0;
}
