/*
 * Private imeplentatin of C$DOPRINT printf engine.  Given a format string and
 * argument list, generate a stream of output characters returned to the
 * call via a buffer and buffer flush routine.
 */
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <wctype.h>
#include <stdio.h>

#include "doprint.h"
/*
 * Define table to map conversion specifier and size combination to type
 * of argument to pull from argument list.
 */
static struct arg_type {
    int qual[4];		/* 0-default, 1-'h', 2-'l', 3-'L' */
} arg_type_map[16] = {
    { 0, -1, 1, -1 },		/* %s, %ls                   0 */
    { 2, 2, 3, 14 },		/* %d, %hd, %ld, %Ld */
    { 2, -1, -1, -1 },		/* %c loads as int */
    { 4, -1, -1, 5 },           /* %e, %Le */
    { 4, -1, -1, 5 },           /* %E, %LE                   4 */
    { 4, -1, -1, 5 },           /* %f, %Lf */
    { 4, -1, -1, 5 },           /* %g, %Lg */
    { 4, -1, -1, 5 },           /* %G, %LG */
    { 2, 2, 3, -1 },           /* %i, %hi %li                8 */
    { 6, 7, 8, -1 },           /* %n, %hn, %ln */
    { 10, 11, 12, 15 },           /* %o, %ho %lo, unsigned */
    { 9, -1, -1, -1 },		/* %p, generic pointer */
    { 10, 11, 12, 15 },           /* %u, %hu %lu, unsigned   c */
    { 10, 11, 12, 15 },           /* %x, %hx %lx, unsigned */
    { 10, 11, 12, 15 },           /* %X, %hX %lX, unsigned */
    { 13, -1, -1, -1 }		/* %% */
};                                     /*   0   4   8   c  e */
static const char *conversion_specifiers = "sdceEfgGinopuxX%";

struct stream_descriptor {
    int size;
    int used;
    char *buffer;
    int (*flush) ( void *arg, char *buffer, int len, int *bytes_left );
    void *flush_arg;
};
typedef struct stream_descriptor *user_stream;

static int flush_stream ( user_stream stream )
{
    int status;
    status = stream->flush ( stream->flush_arg, stream->buffer,
	stream->used, &stream->used );
    return status;
}
/*
 * Add characters to output stream, return value is number added
 * or -1.
 */
static put_stream_nchar ( user_stream stream, char c, size_t length )
{
    size_t remaining, segsize;
    int status, all_same;

    status = 0;
    all_same = 0;
    for ( remaining = length; remaining > 0; remaining -= segsize ) {
	segsize = remaining;
	if ( segsize > stream->size ) segsize = stream->size;
	if ( !all_same ) memset ( &stream->buffer[stream->used], c, segsize );
	stream->used += segsize;

	if ( stream->used >= stream->size ) {
	    status = flush_stream ( stream );
	    stream->used = 0;
	    if ( status <= 0 ) return status;
	    all_same = (segsize==stream->size) ? 1 : 0;
	}
    }
    return length;
}
static int put_stream ( user_stream stream, char *string, size_t length )
{
    size_t remaining, segsize;
    int status;
    status = 0;
    for ( remaining = length; remaining > 0; remaining -= segsize ) {
	segsize = remaining;
	if ( segsize > stream->size ) segsize = stream->size;
	memcpy ( &stream->buffer[stream->used], string, segsize );
	string += segsize;
	stream->used += segsize;

	if ( stream->used >= stream->size ) {
	    status = flush_stream ( stream );
	    stream->used = 0;
	    if ( status <= 0 ) return status;
	}
    }
    return length;
}

static int put_stream_asciiz ( user_stream stream, char *string )
{
    size_t available, segsize;
    int status;
    char *out_ptr;

    for ( out_ptr = string; *out_ptr; out_ptr += segsize ) {
	if ( stream->used >= stream->size ) {
	    status = flush_stream ( stream );
	    stream->used = 0;
	    if ( status <= 0 ) return -1;
	}
	available = stream->size - stream->used;
	segsize = strnlen ( string, available );
	memcpy ( &stream->buffer[stream->used], string, segsize );

	stream->used += segsize;
    }

    return out_ptr - string;
}


static int parse_conversion_descriptor ( const char *cd_start,
	const char *cd_end, struct doprnt_conversion_item *itm )
{
    const char *cur_c;
    char c;
    /*
     * Check for conversion qualifier character immediately
     * preceding conversion specifier character (*cd_end), and if found
     * note it back up cd_end so we don't have to worry about the
     * the qualifier any more.
     */
    itm->specifier_char = *cd_end;
    *((int *)(&itm->flags)) = 0;	/* formatting flags */
    if ( cd_start < cd_end ) {
	c = *--cd_end;
#ifdef TESTIT
printf ( "   conversion qualifier candidate: '%c'\n", c );
#endif
	if ( c == 'h' ) itm->flags.sizeq = 1;
	else if ( c == 'l' ) {
	    itm->flags.sizeq = 2;
	    if ( (cd_start < cd_end) && (*cd_end == 'l') ) {
		cd_end--;	/* treat ll as L, back up one more */
		itm->flags.sizeq = 3;
	    }
	} else if ( c == 'L' ) itm->flags.sizeq = 3;
	else {
	    cd_end++;
	    itm->flags.sizeq = 0;	/* not present */
	}
    } else itm->flags.sizeq= 0;
    /*
     * Initialize rest of caller's arguments.
     */
    itm->width = 0;			/* output field width */
    itm->prec = 0;		/* decimal precision */
    /*
     * Identify and mark flag characters.
     */
    for ( cur_c = cd_start; cur_c < cd_end; cur_c++ ) {
	c = *cur_c;
	if ( (c == '*') || (c == '.') ) break;
	if ( (c>'0') && (c<='9') ) break;	/* width has started */

	switch ( *cur_c ) {
	  /* set flag bit corresponding to character, check for conflicts */
	  case ' ':
	    itm->flags.space = 1;
	    break;
	  case '#':
	    itm->flags.numsign = 1;
	    break;
	  case '+':
	    itm->flags.plus = 1;
	    break;
	  case '-':
	    itm->flags.minus = 1;
	    break;
	  case '0':
	    itm->flags.zero = 1;
	    break;
	  case '\'':
	    itm->flags.quote = 1;
	    break;
	  default:
	    cur_c = cd_end;		/* flag error */
	    break;
	}
	if ( cur_c == cd_end ) return -1;	/* error */
    }
    /*
     * *cur_c is now '.', '*', digit, or equal to *cd_end.
     */ 
    c = *cur_c;
    if ( c == '*' ) {
	/*
	 * Caller must retrieve width from user's argument list.
	 */
	itm->flags.ap_width = 1;
	cur_c++;
	if ( (cur_c < cd_end) && (*cur_c != '.') ) {
#ifdef TESTIT
printf ( "   character following * width not '.' (%c)\n", *cur_c );
#endif
	    return -1;
	}
	cur_c++;
	if ( isdigit(*cur_c) ) {
	    /* Look for n$ argument position */
	    itm->flags.prec_ap_n = 1;
	}
    } else if ( c != '.' ) {
	/*
	 * convert string of digits to width.
	 */
#ifdef TESTIT
printf ( "   expecting %d width+precision characters\n", cd_end - cur_c );
#endif
	while ( cur_c < cd_end ) {
	    c = *cur_c++;
	    if ( isdigit ( c ) ) {
		if ( itm->width >= 100000000 ) { return -1; } /* too large */
		/* assume digits code in contiguously in sequence */
		itm->width = (itm->width)*10 + (c - '0');
	    } else if ( c == '.' ) {
#ifdef TESTIT
printf ( "   width terminated by period (precision)\n" );
#endif
		break;
	    } else if ( c == '$' ) {
		/* caller user ap position indicator, save and reset */
#ifdef TESTIT
printf ( "   Positional argument: %d\n", itm->width );
#endif
		itm->width = 0;
		continue;
	    } else {
		return -1;	/* unexpected character */
	    }
	}
    } else cur_c++;	/* *cur_c was a '.', skip it */
    /*
     * Anything left between cur_c and cd_end is a precision specifier.
     */
    if ( cur_c >= cd_end ) {

    } else if ( *cur_c == '*' ) {
	/* precision loaded from user's argument list */
	cur_c++;
	itm->flags.ap_prec = 1;
	if ( isdigit ( *cur_c == '$' ) ) {
	    /* Absolute position of argument */
	    itm->flags.prec_ap_n = 1;
	}
	if ( cur_c != cd_end ) { return -1; }  /*extra characters */
    } else {
#ifdef TESTIT
printf ( "   expecting %d precision characters\n", cd_end - cur_c );
#endif
	while ( cur_c < cd_end ) {
	    c = *cur_c++;
	    if ( isdigit ( c ) ) {
		if ( itm->prec >= 100000000 ) { return -1; } /* too large */
		/* assume digits code in contiguously in sequence */
		itm->prec = (itm->prec)*10 + (c - '0');
	    } else {
		return -1;	/* unexpected character */
	    }
	}
    }
    return 0;
}
/*
 * integer to decimal functions, signed and unsigned.  Returns length
 * of string.  Note that adding
 */
static int i_to_a ( long long val, char a[24], 
	struct doprnt_conversion_flags flags )
{
    long long rem_stack[22];		/* holds remainders */
    long long *sp;
    int is_negative, digit, len;

#ifdef TESTIT
printf ( "i_to_a(%lld,0x%x,%x) entered\n", val, *((int *)&flags), a );
#endif
    is_negative = (val < 0);
    sp = rem_stack;
    *sp = val;
    do { val = val/10; *(++sp) = val; } while ( val != 0 );
    len = 0;
    if ( is_negative ) {
	a[len++] = '-';
	while ( sp > rem_stack ) {
	    digit = (-1)*(sp[-1] - (*sp)*10); sp--;
	    a[len++] = digit + '0';
	}
    } else {
	if ( flags.plus ) a[len++] = '+';
	while ( sp > rem_stack ) {
	    digit = (sp[-1] - (*sp)*10); sp--;
	    a[len++] = digit + '0';
	}
    }
    a[len] = 0;
    return len;
}

static int u_to_a ( unsigned long long val, char a[24],
	struct doprnt_conversion_flags flags )
{
    unsigned long long rem_stack[22];		/* holds remainders */
    unsigned long long *sp;
    int digit, len;

    sp = rem_stack;
    *sp = val;
    do { val = val/10; *(++sp) = val; } while ( val != 0 );
    len = 0;

    while ( sp > rem_stack ) {
	digit = (sp[-1] + (*sp)*10); sp--;
	a[len++] = digit + '0';
    }
    a[len] = 0;
    return len;
}
static int x_to_a ( unsigned long long val, char a[40],
	struct doprnt_conversion_flags flags )
{
    unsigned long long mask, rem_stack[22];		/* holds remainders */
    unsigned long long *sp;
    int digit, len, zero_suppress, nib_pos;
    static char nibble[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', 'a', 'b', 'c', 'd', 'e' };

    mask = val & 0xffffffff00000000L;
    if ( (flags.sizeq == 3) && flags.zero ) nib_pos = 60;
    else if ( !mask || (mask==0xffffffff00000000L) ) nib_pos = 28;
    else nib_pos = 60;

    len = 0;
    zero_suppress = flags.zero ? 0 : 1;
    if ( flags.uppercase ) {
      while ( nib_pos > 0 ) {
	a[len] = toupper(nibble[(val >> nib_pos) & 0x0f]);
	nib_pos = nib_pos - 4;
	if ( a[len] == '0' ) {
	    if  ( zero_suppress ) continue;  /* skip leading zeros */
	} else zero_suppress = 0;
	len++;
      }
    } else {
      while ( nib_pos > 0 ) {
	a[len] = nibble[(val >> nib_pos) & 0x0f];
	nib_pos = nib_pos - 4;
	if ( a[len] == '0' ) {
	    if  ( zero_suppress ) continue;  /* skip leading zeros */
	} else zero_suppress = 0;
	len++;
      }
    }
    a[len++] = nibble[val & 0x0f];
    a[len] = 0;

    return len;
}
/*
 * Handle formatting of single item and adding to output stream.
 * cnv argument is destroyed.
 */

static int output_item ( struct stream_descriptor *stream, 
	struct doprnt_conversion_item *cnv, doprnt_float_formatters flt_vec )
{
    char common_field[200];
    char *big_field;
    size_t big_field_size;
    char *field;	/* pointer to either common or big */
    int count, fill, status;
    /*
     * Select work buffer for formatting.
     */
    if ( cnv->width < sizeof(common_field) ) {
	field = common_field;
    } else {
	big_field = malloc ( cnv->width );
	field = big_field;
    }
    big_field_size = 0;
    /*
     * Do output.
     */
    fill = 0;			/* number of pad spaces to append. */
    if ( cnv->specifier_char == 's' ) {
	/*
	 * Guarantee at least cnv->width byets output.
	 */
	if ( (cnv->flags.minus) || (cnv->width == 0)) {
	    /* left justify or not padding on right */
	    count = put_stream_asciiz ( stream, cnv->value.char_p_arg );
	    if ( count < 0 ) return count;
	    fill = cnv->width - count;
	} else {
	    fill = cnv->width - strlen(cnv->value.char_p_arg);
	    if ( fill > 0 ) {
		count = put_stream_nchar ( stream, ' ', fill );
		if ( count < 0 ) return count;
	    }
	    fill = 0;
	    count = put_stream_asciiz ( stream, cnv->value.char_p_arg );
	}
    } else if ( cnv->specifier_char == 'd' ) {
	/*
	 * Integer conversion.
	 */
	long long out_val;
	switch ( cnv->flags.sizeq ) {
	  case 0:
	  case 1:
	    out_val = cnv->value.int_arg;
	    break;
	  case 2:
	    out_val = cnv->value.long_arg;
	    break;
	  case 3:
	    out_val = cnv->value.long_long_arg;
	    break;
	}

	count = i_to_a ( out_val, common_field, cnv->flags );
	count = put_stream ( stream, common_field, count );
	if ( count < 0 ) return count;

    } else if ( cnv->specifier_char == 'c' ) {
	/*
	 * Single character.
	 */
	unsigned char out[4];
	out[0] = cnv->value.int_arg;
	if ( cnv->width > 0 ) {
	    if ( cnv->flags.minus ) {	/* left justif */
		fill = cnv->width - 1;  /* pad on right */
	    } else {
		fill = 0;
		count = put_stream_nchar ( stream, ' ', cnv->width-1 );
	    }
	}
	count = put_stream ( stream, (char *) out, 1 );
    } else if ( (cnv->specifier_char == 'x') || 
		(cnv->specifier_char == 'X') ) {
	/*
	 * Integer conversion.
	 */
	unsigned long long out_val;
	switch ( cnv->flags.sizeq ) {
	  case 0:
	  case 1:
	    out_val = cnv->value.uint_arg;
	    break;
	  case 2:
	    out_val = cnv->value.ulong_arg;
	    break;
	  case 3:
	    out_val = cnv->value.ulong_long_arg;
	    break;
	}
	if ( isupper(cnv->specifier_char) ) cnv->flags.uppercase = 1;
	count = x_to_a ( out_val, common_field, cnv->flags );
	count = put_stream ( stream, common_field, count );
	if ( count < 0 ) return count;

    } else if ( strchr ( "fFgGeE", cnv->specifier_char ) ) {
	fill = 0;
	if ( cnv->prec == 0 ) cnv->prec = 6;	/* default */
	if ( (cnv->flags.sizeq == 0) || (cnv->flags.sizeq == 2) ) {
	    count = flt_vec->fmt_double ( cnv, common_field, 
		sizeof(common_field) );
	} else if ( cnv->flags.sizeq == 3 ) {
	    count = flt_vec->fmt_long_double ( cnv, common_field,
		sizeof(common_field)  );
	}
	if ( count > 0 ) count = put_stream ( stream, common_field, count );
	if ( count < 0 ) return count;
    }
    /*
     * Add pad spaces.
     */
    if ( fill > 0 ) {
	count = put_stream_nchar ( stream, ' ', fill );
	if ( count < 0 ) return count;
    }

    return 0;
}
/********************************************************************/
int doprint_engine ( 
	char *buffer, 			/* I/O buffer, filled */
	const char *format_spec, 	/* printf format */
	va_list ap, 			/* printf variable arguments */
	size_t bufsize, 		/* Size of I/O buffer */
	void *flush_arg, 
	int (*buffer_flush)( void *,char *, int, int * ),
	int *bytes_left,		/* Number of bytes remaining in buffer*/
	doprnt_float_formatters flt_vec )
{
    struct stream_descriptor stream;
    const char *orig_format_spec, *fmt_ptr, *cd_end;
    char c;
    struct doprnt_conversion_item cnv;
    int count, status, sc_index;
    /*
     * Initialize output buffer (should used copy in current bytes_left?).
     */
    *bytes_left = 0;
    stream.size = bufsize;
    stream.used = 0;
    stream.buffer = buffer;
    stream.flush = buffer_flush;
    stream.flush_arg = flush_arg;
    /*
     * Optimize common case of "%s".  Directly add argument pointer strings
     * to output stream and advance format_spec so they aren't scanned again.
     */
    for ( orig_format_spec = format_spec;
    	(format_spec[0] == '%') && (format_spec[1] == 's'); format_spec+=2 ) {
	char *str;
	str = va_arg(ap, char *);
	put_stream_asciiz ( &stream, str );
    }
    /*
     * Scan format_spec, adding characters to output buffer until format
     * descriptor encountered.
     */
    count = 0;
    status = 0;
    for ( fmt_ptr = format_spec; *fmt_ptr; fmt_ptr++ ) {
	c = *fmt_ptr;
	if ( c == '%' ) {
	    /*
	     * Find the conversion specifier character that terminates
	     * the descriptor;
	     */
	    cd_end = strpbrk ( fmt_ptr+1, conversion_specifiers );
	    if ( cd_end ) {
		/*
		 * Parse the descriptor to get flags, width, precision.
		 * (skip the leading %).
		 */
		status = parse_conversion_descriptor (fmt_ptr+1, cd_end, &cnv);
		if ( status < 0 ) break;
#ifdef TESTIT
printf ( "Status of parse fmt[%d..%d]: %d, flags=%x, width=%d, prec=%d, sizeq=%d\n",
fmt_ptr-format_spec, cd_end-format_spec, status, 
*((int *)&cnv.flags), cnv.width, cnv.prec,
cnv.flags.sizeq);
#endif
	        /*
		 * Extract width and precision from argument list if descriprot
		 * specified them as '*'.
		 */
		if ( cnv.flags.ap_width ) {
		    cnv.width = va_arg(ap, int);
		    cnv.flags.ap_width = 0;		 /* turn bit off */
		    if ( cnv.width < 0 ) {
			cnv.flags.minus = 1;		/* treat as flag */
			cnv.width *= (-1);
		    }
		}
		if ( cnv.flags.ap_prec ) {
		    cnv.prec = va_arg(ap, int);
		    cnv.flags.ap_prec = 0;		/* turn bit off */
		}
	        /*
		 * Extract argument from argument list, whose type is a
		 * function of the specifier character and size qualifier.
		 */
		sc_index = strchr(conversion_specifiers,cnv.specifier_char) -
			conversion_specifiers;
		switch ( arg_type_map[sc_index].qual[cnv.flags.sizeq] ) {
		    case 0:
			cnv.value.char_p_arg = va_arg(ap, char *); break;
		    case 1:
			cnv.value.wchar_p_arg = va_arg(ap, wchar_t *); break;
		    case 2:
			cnv.value.int_arg = va_arg(ap, int); break;
		    case 3:
			cnv.value.long_arg = va_arg(ap, long); break;
		    case 4:
			cnv.value.double_arg = va_arg(ap, double); break;
		    case 5:
			flt_vec->get_Ldouble ( ap, &cnv.value.long_double_arg );
			break;
		    case 6:
			cnv.value.int_p_arg = va_arg(ap, int *); break;
		    case 7:
			cnv.value.short_p_arg = va_arg(ap, short *); break;
		    case 8:
			cnv.value.long_p_arg = va_arg(ap, long *); break;
		    case 9:
			cnv.value.void_p_arg = va_arg(ap, void *); break;
		    case 10:
			cnv.value.uint_arg = va_arg(ap, unsigned int); break;
		    case 11:
			cnv.value.ulong_arg = va_arg(ap, unsigned long); break;
		    case 13:
			break;		/* do nothing */
		    case 14:
			cnv.value.long_long_arg = va_arg(ap, long long); break;
		    case 15:
			cnv.value.ulong_long_arg = va_arg(ap, unsigned long long); break;
			break;		/* do nothing */
		    default:
	printf ( "Bugcheck convspec[%d(%c)][%d] = %d\n", sc_index,
cnv.specifier_char, cnv.flags.sizeq, arg_type_map[sc_index].qual[cnv.flags.sizeq] );
			break;
		}
	  	/*
		 * conversion item can now be sent processed standalone to
		 * produce output.
		 */
		output_item ( &stream, &cnv, flt_vec );
		/*
		 * Update loop variable so scan will resume and character
		 * following the conversion descriptor.
		 */
		fmt_ptr = cd_end;
	    } else {
		/* Bad format, reached end of string with no conversion spec. */
		if ( stream.used > 0 ) flush_stream ( &stream );
		return -1;
	    }
	} else {
	    stream.buffer[stream.used++] = c;
	    if ( stream.used >= stream.size ) {
		if ( flush_stream ( &stream ) < 0 ) break;
	    }
	}
    }
    if ( stream.used > 0 ) flush_stream ( &stream );
    return status;
}
/*************************************************************************/
/* Floating point formatters produce and intermeiate result consisting of
 * a string of digits, decimal point offset and sign flag.  The functions
 * below are common finalize step of generated the formatted output.
 *
 * Return value is number of output bytes.
 */
int doprnt_ecvt_to_printf ( struct doprnt_conversion_item *cnv,
    const char *digits, int decpt, int negsign, char *buffer, size_t bufsize )
{
    int outlen, exponent, dec_digits, precision, width;
    /*
     * Check for invalid result from fcvt.
     */
    if ( !digits ) {
	strcpy ( buffer, "?ecvt?" );
	return 6;
    }
    /*
     * begin output buffer and output sign-related character
     */
    outlen = 0;
    dec_digits = 0;			/* number of decimal digits output */
    precision = cnv->prec;
    if ( negsign ) buffer[outlen++] = '-';
    /* else if ( digits[0] == '0' ) {
	if ( cnv->flags.space ) buffer[outlen++] = ' ';
    } */
    else if ( cnv->flags.plus ) buffer[outlen++] = '+';
    /*
     * Output digits to the left of the decimal point and end with dec.
     */
    exponent = decpt - 1;	/* show 1 digit to left of dec pt */
    buffer[outlen++] = *digits++;
    buffer[outlen++] = '.';

    /*
     * Copy remaining digits to right of decimal points or padd with
     * zeros to get required precision.
     */
    while ( *digits ) {
	dec_digits++;
	if ( dec_digits >= precision ) break;
	buffer[outlen++] = *digits++;
	if ( (outlen+4) > bufsize ) break;
    }
    while ( dec_digits < precision ) {
	dec_digits++;
	buffer[outlen++] = '0';
	if ( (outlen+4) > bufsize ) break;
    }
    /*
     * add exponent.
     */
    buffer[outlen++] = cnv->specifier_char;
    buffer[outlen++] = (exponent < 0) ? '-' : '+';
    if ( exponent < 0 ) exponent = (-exponent);
    exponent = exponent % 1000;
    if ( exponent >= 100 ) {
	buffer[outlen++] = (exponent/100) + '0';
    }
    exponent = exponent % 100;
    buffer[outlen++] = (exponent/10) + '0';
    buffer[outlen++] = (exponent%10) + '0';
    /*
     * Take care of padding and left justification.
     */
    width = cnv->width;
    if ( width > bufsize ) width = bufsize;
    if ( width > outlen ) {
	int shift, i;
	if ( cnv->flags.minus ) {
	    /* left justify, padd on right with spaces */
	    while ( outlen < width ) buffer[outlen++] = ' ';

	} else {
	    /* right justify, insert spaces at front */
	    shift = width - outlen;
	    if ( shift > 0 ) {
		memmove ( &buffer[shift], buffer, shift );
		for ( i = 0; i < shift; i++ ) buffer[i] = ' ';
		outlen += shift;
	    }
	    
	}
    }
    return outlen;
}

int doprnt_fcvt_to_printf ( struct doprnt_conversion_item *cnv,
    const char *digits, int decpt, int negsign, char *buffer, size_t bufsize )
{
    int outlen, dec_digits, precision, width;
    /*
     * Check for invalid result from fcvt.
     */
    if ( !digits ) {
	strcpy ( buffer, "?ecvt?" );
	return 6;
    }
    /*
     * begin output buffer and output sign-related character
     */
    outlen = 0;
    dec_digits = 0;			/* number of decimal digits output */
    precision = cnv->prec;
    if ( negsign ) buffer[outlen++] = '-';
    else if ( digits[0] == '0' ) {
	if ( cnv->flags.space ) buffer[outlen++] = ' ';
    } else if ( cnv->flags.plus ) buffer[outlen++] = '+';
    /*
     * Output digits to the left of the decimal point and end with decimal pt.
     */
    if ( decpt <= 0 ) {
	/*
	 * No digits in provided string are to left of decimal.
	 * Add "0." to buffer and padd with 0 to align.
	 */
	buffer[outlen++] = '0';
	for (buffer[outlen++] = '.'; decpt < 0; decpt++) {
	    dec_digits++;
	    buffer[outlen++]='0';
	    if ( dec_digits >= precision ) break;
	    if ( (outlen+4) > bufsize ) break;
	}
    } else {
	for ( ; (decpt > 0) && *digits; decpt-- ) {
	    buffer[outlen++] = *digits++;
	    if ( (outlen+4) > bufsize ) break;
	}
	buffer[outlen++] = '.';
    }
    /*
     * Copy remaining digits to right of decimal points or padd with
     * zeros to get required precision.
     */
    while ( *digits ) {
	if ( dec_digits >= precision ) break;
	buffer[outlen++] = *digits++;
	dec_digits++;
	if ( (outlen+4) > bufsize ) break;
    }
    while ( dec_digits < precision ) {
	dec_digits++;
	buffer[outlen++] = '0';
	if ( (outlen+4) > bufsize ) break;
    }
    /*
     * Take care of padding and left justification.
     */
    width = cnv->width;
    if ( width > bufsize ) width = bufsize;
    if ( width > outlen ) {
	int shift, i;
	if ( cnv->flags.minus ) {
	    /* left justify, padd on right with spaces */
	    while ( outlen < width ) buffer[outlen++] = ' ';

	} else {
	    /* right justify, insert spaces at front */
	    shift = width - outlen;
	    if ( shift > 0 ) {
		memmove ( &buffer[shift], buffer, outlen );
		for ( i = 0; i < shift; i++ ) buffer[i] = ' ';
		outlen += shift;
	    }
	    
	}
    }
    return outlen;
}
