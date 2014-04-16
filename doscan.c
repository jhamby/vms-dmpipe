/*
 * Private imeplentatin of DOSCAN-style scanf engine.  Given a format string and
 * argument list, read a stream of input characters and load/convert into 
 * storage pointed to by argument list entries.
 *
 * Wide characters are not yet supported!
 *
 * Author: David Jones
 * Date:   2-APR-2014
 *
 */
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <wctype.h>
#include <stdio.h>
#include <errno.h>

#include "doscan.h"
/*
 * Define table to map conversion specifier and size combination to type
 * of argument to pull from argument list.
 */
static struct arg_type {
    int qual[4];		/* 0-default, 1-'h', 2-'l', 3-'L' */
} arg_type_map[17] = {
    { 0, -1, 1, -1 },		/* %s, %ls                   0 */
    { 1, 1, 1, 1 },		/* %d, %hd, %ld, %Ld */
    { 2, -1, -1, -1 },		/* %c loads as int */
    { 1, -1, -1, 1 },           /* %i, %Li */
    { 3, -1, 3, 3 },           /* %e, %le, %LE               4 */
    { 3, -1, 3, 3 },           /* %E, %LE                    */
    { 3, -1, 3, 3 },           /* %f, %Lf */
    { 3, -1, 3, 3 },           /* %g, %Lg */
    { 3, -1, 3, 3 },           /* %G, %LG                   8 */
    { 4, -1, -1, 4 },           /* %[..., %l[... */
    { 5, -1, -1, -1 },          /* %n, %hi %li                 */
    { 1, 1, 1, 1 },     	/* %o, %ho, %lo */
    { 7, -1, -1, -1 },		/* %p, generic pointer */
    { 1 , 1, 1, 1 },	        /* %u, %hu %lu, unsigned   c */
    { 1 , 1, 1, 1 },	        /* %x, %hx %lx, unsigned */
    { 1 , 1, 1, 1 },    	/* %X, %hX %lX, unsigned */
    { 6 , -1, -1, -1 }		/* %% */
};                                     /*   0   4   8   c  e */
static const char *conversion_specifiers = "sdcieEfgG[nopuxX%";

struct doscan_stream {
    void *input_arg;
    doscan_callback input_scan;

    long bytes_read;
    char *term_char;			/* terminating character */
};

static int get_input ( struct doscan_stream *stream, int scan_control,
	const char *matchset, char *buffer, int bufsize )
{
    int count;

    count = stream->input_scan ( stream->input_arg, scan_control,
	matchset, buffer, bufsize, &stream->term_char );
    if ( count > 0 ) stream->bytes_read += count;
    return count;
}
/*
 * Parse single conversino decriptor extracted from the format string.
 * cd_start points to first character after the % and cd_end is the
 * conversino descriptor character.  Return 0 on success, -1 on failure.
 */
static const char *parse_conversion_descriptor ( const char *cd_start,
	const char *cd_end, struct doscan_conversion_item *itm )
{
    const char *cur_c, *retval, *rbrack;
    char c;
    /*
     * Check for conversion qualifier character immediately
     * preceding conversion specifier character (*cd_end), and if found
     * note it back up cd_end so we don't have to worry about the
     * the qualifier any more.
     */
    itm->specifier_char = *cd_end;
    *((int *)(&itm->flags)) = 0;	/* formatting flags */
    retval = cd_end;
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
	    cd_end++;			/* restore to original value */
	    itm->flags.sizeq = 0;	/* not present */
	}
    } else itm->flags.sizeq= 0;
    /*
     * Initialize rest of caller's arguments.
     */
    itm->width = 0;			/* output field width */
    /*
     * Note presence of * following %.
     */
    c = *cd_start;
    if ( c == '*' ) {
	cd_start++;
	itm->flags.discard = 1;	
    }
    /*
     * Convert digits to width.
     */
    for ( cur_c = cd_start; cur_c < cd_end; cur_c++ ) {
	c = *cur_c;
	if ( isdigit(c) ) {
	    itm->width = itm->width*10 + (c-'0');
	} else break;
    }
    /*
     * Descriptor has extraneous, illegal, characters if anything between
     * the digits and the end of the conversion descriptor.
     */
    if ( cur_c < cd_end ) {
printf ( "/doscan/ parse_conversion failed, start(%d) < end(%d)\n",
cd_start, cd_end );
	return 0;
    }
    /*
     * The '[' specifier is followed by the matchset characters to use.
     * Extend return value to the closing ']'
     */
    if ( itm->specifier_char == '[' ) {
	/*
	 * Note caret flag and skip.
	 */
	int len;
	cd_start = retval+1;
	if ( *cd_start ) {
	    itm->flags.invert_sset = 1;
	    cd_start++;
	}
	/*
	 * Match set is always at least 1 character.  Find closing
	 * delimiter and copy string to itm->matchset.
	 */
	rbrack = strchr ( (*cd_start==']') ? cd_start+1 : cd_start, ']' );
	if ( !rbrack ) return 0;	/* ']' not found */
	len = rbrack - cd_start;	/* include terminating null */
	itm->matchset = itm->small_matchset;
	if ( len > sizeof(itm->small_matchset) ) {
	    itm->matchset = malloc ( len );
	    if ( !itm->matchset ) return 0;
	}
	memcpy ( itm->matchset, cd_start, len-1 );
	itm->matchset[len-1] = '\0';
    }
    return retval;
}
/*
 * Begin reading number from input stream, return value is state:
 *    -1    	read error
 *     0        no chars read, pending char not in match set nor +/-.
 *     1	no chars read, pending char is non-zero digit.
 *     2        no chars read, pending char is zero.
 *     3        1 character read, plus or minus.
 */
static int start_number ( struct doscan_stream *stream, int flags,
	const char *matchset, char *buffer )
{
    int status, state;
    status = get_input ( stream, flags, "-+", buffer, 1 );
    if ( status < 0 ) return -1;
    state = 0;
    if ( status == 0 ) {
	if ( stream->term_char ) {
	    if ( *stream->term_char == '0' ) state = 2;
	    else if ( strchr ( matchset, *stream->term_char ) ) {
		state = 1;
	    }
	}
    } else {
	/* '-' or '+' read */
	state = 3;
    }
    return state;
}
/*
 * Handle reading of single item from input stream and converting value
 * to caller's argument.  Return value is 1 for success.
 */
static int input_item ( struct doscan_stream *stream,
	struct doscan_conversion_item *cnv, doscan_float_formatters flt_vec,
	char **scratch_buffer )
{
    char common_field[200];
    char *big_field, *field_end, *cp, c;
    size_t big_field_size;
    double fvalue;
    char *field;	/* pointer to either common or big */
    int count, len, state, status, scan_flags, radix, sc_index, value;
    int exp_offset;
    static struct {
	char spec;
	char code;
	short radix;
	char *range;		/* starts with +- */
	int is_signed;
    } *idef, intdef[] = {
	{ 'd', 0, 10, "0123456789",1 }, 
	{ 'f', 0, 10, "0123456789",1 },
	{ 'i', 1, 10, "0123456789",1 },
	{ 'o', 0, 8, "01234567",0 },
	{ 'x', 2, 16, "0123456789abcdeFABCDEF", 0 },
	{ 'X', 2, 16, "0123456789abcdeFABCDEF", 0 },
	{ 0, 0, 0, 0, 0 }
    };
    /*
     * Select work buffer for formatting.
     */
    if ( cnv->width < sizeof(common_field) ) {
	field = common_field;
	field_end = field + sizeof(common_field) - 4;
	*scratch_buffer = 0;
    } else {
	big_field = malloc ( cnv->width + 4 );
	field = big_field;
	field_end = field + cnv->width;
	*scratch_buffer = big_field;
    }
    /*
     * Lookup steps in table.
     */
    count = 0;
    sc_index = strchr(conversion_specifiers,cnv->specifier_char) -
			conversion_specifiers;
    switch ( arg_type_map[sc_index].qual[cnv->flags.sizeq] ) {
	case 0:	
	    /*
	     * s conversion.  Copy characters into caller's scanf argument.
	     */
	    status = get_input ( stream, DOSCAN_CTL_STRING,
		"", cnv->value.char_p_arg, cnv->width ? cnv->width : -1 );
	    if ( status > 0 ) { 
		count++; cnv->value.char_p_arg[status] = '\0';
	    } else return -1;
	    break;
			
	case 1:
	    /*
	     * d, o, x and i conversion.  Read first character from set that
	     * includes '+'/'-', then read remainder from octal set or
	     * radix-specific set, possibly doing 3rd read.
	     *
	     */
	    for ( idef = intdef; idef->spec; idef++ ) {
		if ( idef->spec == cnv->specifier_char ) break;
	    }
	    if ( !idef ) return -1;	/* bugcheck, shouldn't happen */
	    state = 0;
	    cp = field;
	    scan_flags = 0;
	    state = start_number (stream, DOSCAN_CTL_SKIP_WS, idef->range, cp);
	    if ( state <= 0 ) return state;	/* EOF or not a number */
	    if ( state == 3 ) cp++;
	    /*
	     * State is 1, 2, or 3.  Read rest of number.
	     */
	    if ( (state == 2) && (idef->code==0) ) state = 1;
	    status = get_input (stream, 0, (state>1) ? "01234567" :idef->range,
		cp, cnv->width ? cnv->width : 80 );
	    len = (status > 0) ? status : 0;
	    if ( state == 3 ) {
		/* Check for -0nnn or -0x case and change state to 2 if so. */
		if ( status < 0 ) return 0;
		if ( status == 0 ) return 0;	/* just sign present */
		if ( *cp == '0' ) {
		    state = 2;		/* must be octal or hex */
		    status--;		/* pretend we read the '0' last time */
		    cp++;
		    len--;
		} else {
		    /* Force third read if term_char was non-octal digit */
		    c = stream->term_char ? *stream->term_char : 0;
		    if ( strchr(idef->range,c) ) 
			scan_flags = DOSCAN_CTL_MATCH_FIRST;
		}
	    }
	    if ( (state == 2) && (status >= 0) ) {
		/* Force number to octal or hex interpretation */
		c = stream->term_char ? *stream->term_char : 0;
	        if ( (status <= 1) && ((c == 'x') || (c == 'X')) ) {
		    if ( idef->radix != 16 ) do { idef++;
		    } while ( idef->spec != 'x' );

		} else {
		    if ( idef->radix != 8 ) do { idef++;
		    } while ( idef->spec != 'o' );
		}
		scan_flags = DOSCAN_CTL_MATCH_FIRST;
	    }
	    cp += len;
	    /*
	     * Final read to include string after premature halt.
	     */
	    if ( scan_flags ) {
		status = get_input ( stream, scan_flags, idef->range,
			cp, cnv->width ? cnv->width : 80 );
		if ( status > 0 ) cp += status;
	    }
	    *cp = 0;
	    value = strtol ( field, &stream->term_char, idef->radix );
	    if ( (cnv->flags.sizeq == 0) || (cnv->flags.sizeq == 2) ) {
	        *cnv->value.int_arg = value;  /* should check sizeq */
	    } else if ( cnv->flags.sizeq == 1 ) {
		*cnv->value.short_arg = (short int ) value;
	    } else {
		*cnv->value.long_long_arg = value;
	    }
	    count++;
	    break;

	case 2:
		/* c conversion */
	    if ( cnv->width == 0 ) cnv->width = 1;
	    status = get_input ( stream, DOSCAN_CTL_INVERT, "", 
		cnv->value.char_p_arg, cnv->width );
	    if ( status > 0 ) count++;
	    break;

	case 3:
	    /* 
	     * Floating point conversions, read first character.
             */
	    for ( idef = intdef; idef->spec; idef++ )
		if ( idef->spec == 'f' ) break;
	    exp_offset = 0;
	    cp = field;
	    state = start_number(stream, DOSCAN_CTL_SKIP_WS, ".0123456789", cp);
	    if ( state <= 0 ) return state;	/* EOF or not a number */
	    if ( state == 3 ) cp++;
            /*
	     * Read remainder of number and get terminating character.
	     */
	    status = get_input ( stream, DOSCAN_CTL_SKIP_WS, 
		idef->range, cp, cnv->width ? cnv->width : field_end-cp );
	    if ( status >= 0 ) {
		c = stream->term_char ? *stream->term_char : '\0';
	    } else if ( state == 3 ) {
		return 0;			/* only +/- read */
	    } else return 0;
	    cp += status;
	    /*
	     * Add in fraction if decimal point present.
	     */
	    if ( c == '.' ) {
		status = get_input(stream, DOSCAN_CTL_MATCH_FIRST, 
			idef->range, cp, field_end-cp);
		if ( status >= 0 ) {
		    cp += status;
		    c = stream->term_char ? *stream->term_char : '\0';
		}
	    }
	    /*
	     * Add in exponent if number terminated with 'e'.
	     */
	    if ( (c == 'e') || (c == 'E') ) {
		/* Read the E and following character */
		exp_offset = cp - field;
		status = get_input(stream, 0, "eE-+0123456789", cp, 2 );
		if ( status != 2 ) {
		    return 0;
		}
		cp += status;
		status = get_input ( stream, 0, idef->range, cp,
			field_end-cp );
		if ( status >= 0 ) cp += status;
	    }
	    /*
	     * Convert the value.
	     */
	    *cp = '\0';
	    fvalue = strtod ( field, &stream->term_char );
	    if ( cnv->flags.sizeq == 2 ) {
	        status = flt_vec->fmt_double ( cnv, field, exp_offset );
	    } else {
		status = flt_vec->fmt_float ( cnv, field, exp_offset );
	    }
	    if ( status == 0 ) count++;
	    break;

	case 4:
		/* Match set */
	    status = get_input ( stream,
		cnv->flags.invert_sset ? DOSCAN_CTL_INVERT : 0, cnv->matchset, 
		cnv->value.char_p_arg, 
		cnv->width ? cnv->width : -1 );
	    if ( status > 0 ) { 
		count++; cnv->value.char_p_arg[status] = '\0';
	    } else return -1;
	    break;

	case 5:
	    /*
	     * Number of characters used so far, doesn't bump count.
	     */
	    *cnv->value.int_arg = stream->bytes_read;
	    break;

	case 6:
	    /* % */
	    status = get_input ( stream, 0, "%", field, 1 );
	    if ( status == 1 ) count++;
	    else return -1;	/* % sign not at expected position */
	    break;

	case 7:
	    /* Pointer */
	    break;
	default:
	fprintf ( stderr, "/doscan/ Bugcheck convspec[%d(%c)][%d] = %d\n", sc_index,
cnv->specifier_char, cnv->flags.sizeq, arg_type_map[sc_index].qual[cnv->flags.sizeq] );
	    return -1;
	    break;
    }
    return count;
}
/********************************************************************/
int doscan_engine ( 
	const char *format_spec, 	/* printf format */
	va_list ap, 			/* printf variable arguments */
	void *input_arg, 
	doscan_callback input_cb,
	doscan_float_formatters flt_vec )
{
    const char *orig_format_spec, *fmt_ptr, *cd_end;
    char c, *scratch;
    struct doscan_conversion_item cnv;
    struct doscan_stream stream;
    int count, status, sc_index;
    /*
     * Setup stream structure so we can track bytes read for %n.
     */
    stream.input_arg = input_arg;
    stream.input_scan = input_cb;
    stream.bytes_read = 0;
    stream.term_char = 0;
    count = 0;
    /*
     * Optimize common case of "%s".  Directly add argument pointer strings
     * to output stream and advance format_spec so they aren't scanned again.
     */
    for ( orig_format_spec = format_spec;
    	(format_spec[0] == '%') && (format_spec[1] == 'y'); format_spec+=2 ) {
	char *str;
	str = va_arg(ap, char *);
	status = get_input ( &stream, DOSCAN_CTL_STRING, "", str, -1 );
printf ( "/doscan/ optimized s conversion, callback status: %d\n", status );
	if ( status > 0 ) { count++; str[status] = '\0'; }
	else return -1;
    }
    /*
     * Scan format_spec, adding characters to output buffer until format
     * descriptor encountered.
     */
    status = 0;
    for ( fmt_ptr = format_spec; *fmt_ptr; fmt_ptr++ ) {
	char matchset[4], instr[4];
	c = *fmt_ptr;
	if ( isspace ( c ) ) {
	    /*
	     * Skip whitespace by scanning with an empty matchset.
	     */
            cnv.flags.ws_precedes = 1;
	    status = get_input ( &stream, DOSCAN_CTL_SKIP_WS, "", instr, 4 );
	    if ( status < 0 ) break;

	} else if ( c != '%' ) {
	    /*
	     * Create a 1 character scanset consisting of the current character.
	     */
	    matchset[0] = c;
	    matchset[1] = 0;
	    status = get_input ( &stream, 0, matchset, instr, 1 );
	    if ( status != 1 ) {
		errno = 0;
		break;
	    }

	} else if ( c == '%' ) {
	    /*
	     * Find the conversion specifier character that terminates
	     * the descriptor;
	     */
	    cd_end = strpbrk ( fmt_ptr+1, conversion_specifiers );
	    if ( cd_end ) {
		/*
		 * Parse the descriptor to get flags, width.
		 * (skip the leading %).
		 */
		cd_end = parse_conversion_descriptor (fmt_ptr+1, cd_end, &cnv);
		if ( !cd_end < 0 ) break;
	        /*
		 * Extract argument from argument list, whose type is a
		 * function of the specifier character and size qualifier.
		 */
	 	if ( !cnv.flags.discard ) {
		    cnv.value.void_arg = va_arg(ap,void *);
		} else cnv.value.void_arg = 0;
	  	/*
		 * conversion item can now be sent processed standalone to
		 * convert input.
		 */
		status = input_item ( &stream, &cnv, flt_vec, &scratch );
	 	if ( scratch ) free ( scratch );
		if ( cnv.matchset && cnv.matchset != cnv.small_matchset )
			free ( cnv.small_matchset );
		if ( status < 0 ) break;
		if ( status > 0 ) count += status;
		/*
		 * Update loop variable so scan will resume at character
		 * following the conversion descriptor.
		 */
		fmt_ptr = cd_end;
	    } else {
		/* Bad format, reached end of string with no conversion spec. */
		/* printf ( "/doscan/ conversion specifier not found\n" ); */
		errno = EINVAL;
		return -1;
	    }
	}
    }
    return count;
}
