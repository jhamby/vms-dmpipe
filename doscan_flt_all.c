/*
 * Module to be compiled 6 times to support the various DECC floating
 * point represention options. The global string doscan_float_fomatters
 * will be renamed via macro to a floating point specific name.
 */
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "doscan.h"
/*
 * Implement functions that convert 1 floating point value in cnv structure
 * to a character string.
 */
static int fmt_float ( struct doscan_conversion_item *cnv,
	char *numstr, int exp_offset )
{
    double value;
    char *endptr;

    errno = 0;
    value = strtod ( numstr, &endptr );
    if ( errno ) return -1;
    *(cnv->value.float_arg) = value;
    return 0;
}
static int fmt_double ( struct doscan_conversion_item *cnv,
	char *numstr, int exp_offset )
{
    double value;
    char *endptr;

    errno = 0;
    value = strtod ( numstr, &endptr );
    if ( errno ) return -1;
    *(cnv->value.double_arg) = value;
    return 0;
}
static int fmt_long_double ( struct doscan_conversion_item *cnv,
	char *numstr, int exp_offset )
{
    double value;
    char *endptr;

    errno = 0;
    value = strtod ( numstr, &endptr );
    if ( errno ) return -1;
    *(cnv->value.long_double_arg) = value;
    return 0;
}
static int fmt_imaginary ( struct doscan_conversion_item *cnv,
	char *numstr, int exp_offset )
{
    return -1;
}
static int fmt_complex ( struct doscan_conversion_item *cnv,
	char *numstr, int exp_offset )
{
    return -1;
}
/*
 * Global formatter vectors users of doprint reference to get the
 * formatters defined in this module.
 */
struct doscan_float_format_functions doscan_compiled_float =
{ vfscanf, fmt_float, fmt_double, fmt_long_double, fmt_imaginary, fmt_complex };
