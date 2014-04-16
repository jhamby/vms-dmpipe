/*
 * Module to be compiled 6 times to support the various DECC floating
 * point represention options. The global string doprnt_float_fomatters
 * will be renamed via macro to a floating point specific name.
 */
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "doprint.h"
/*
 * get_Ldouble function simple retrieve long double argument from argument
 * list. long double may be 128 bits when __X_FLOAT set, which may take
 * 2 registers when passed by value.
 */
static va_list get_Ldouble ( va_list ap, long double *arg )
{
    *arg = va_arg(ap, long double);

    return ap;
}
/*
 * Implement functions that convert 1 floating point value in cnv structure
 * to a character string.
 */
static int fmt_float ( struct doprnt_conversion_item *cnv,
	char *buffer, size_t bufsize )
{
    return -1;
}
static int fmt_double ( struct doprnt_conversion_item *cnv,
	char *buffer, size_t bufsize )
{
    char *result;
    int width, upcase, fixup, i, is_negative, decpt_pos;
    double value;

    width = cnv->width;
    if ( width >= bufsize ) {
	/* truncate output */
	width = bufsize;
    }
    upcase = 0;
    fixup = 1;
    value = cnv->value.double_arg;

    switch ( cnv->specifier_char ) {
	case 'E':
	    upcase = 1;
	case 'e':
	    result = ecvt (value, (width+1), &decpt_pos, &is_negative);
	    return doprnt_ecvt_to_printf ( cnv, result, decpt_pos,
		is_negative, buffer, bufsize );

	case 'F':
	    upcase = 1;
	case 'f':
	    result = fcvt (value, width, &decpt_pos, &is_negative);
	    return doprnt_fcvt_to_printf ( cnv, result, decpt_pos,
		is_negative, buffer, bufsize );

	case 'G':
	    upcase = 1;
	case 'g':
	    fixup = 0;
	    result = gcvt ( value, width>0?width : 6, buffer );
	    if ( ! result ) result = "?gcvt?";
	    break;

	default:
	    result = "??????";
	    break;
    }
    if ( result != buffer ) {
	strcpy ( buffer, result );
    }
    if ( upcase ) {
	for ( i = 0; buffer[i]; i++ ) buffer[i] = toupper(buffer[i]);
    }
    return strlen ( buffer );
}
static int fmt_long_double ( struct doprnt_conversion_item *cnv,
	char *buffer, size_t bufsize )
{
    double temp;

    temp = cnv->value.long_double_arg;	/* convert */
    cnv->value.double_arg = temp;
    return fmt_double ( cnv, buffer, bufsize );
}
static int fmt_imaginary ( struct doprnt_conversion_item *cnv,
	char *buffer, size_t bufsize )
{
    return -1;
}
static int fmt_complex ( struct doprnt_conversion_item *cnv,
	char *buffer, size_t bufsize )
{
    return -1;
}
/*
 * Global formatter vectors users of doprint reference to get the
 * formatters defined in this module.
 */
struct doprnt_float_format_functions doprnt_compiled_float =
{ fmt_float, fmt_double, fmt_long_double, fmt_imaginary, fmt_complex,
  get_Ldouble };
