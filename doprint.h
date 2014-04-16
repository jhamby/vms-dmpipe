#ifndef DOPRINT_H
#define DOPRINT_H
/*
 * Private imeplentatin of C$DOPRINT printf engine.  Given a format string and
 * argument list, generate a stream of output characters returned to the
 * call via a buffer and buffer flush routine.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <wctype.h>		/* for wint_t */
/*
 * Conversion_item structure is output of parse_conversion_descriptor
 * function except for value member, which is loaded later by caller.
 * Flags indicate format flags prefixes as well as arglist source items.
 */
struct doprnt_conversion_flags {
    unsigned int space:      1,
	numsign:    1,	    /* "%#d" */
	plus:       1,
	minus:      1,
	zero:       1,
	quote:      1,
	ap_width:   1,	    /* "%*d", width set in argument list */
	ap_prec:    1,      /* "%9.*f" precision set in arg. list */
	arg_ap_n:   1,      /* "%2$d" Argument at abosulte arg list pos. */
	width_ap_n: 1,
	prec_ap_n:  1,
	uppercase:  1,      /* E, G, or X specifier characters */
	sizeq:      2,      /* size qualifier: 0-none, 1-h, 2-l, 3-L/ll */
	fill:      18;      /* kick up to 32 bits */
};
struct doprnt_conversion_item {
    int specifier_char;
    struct doprnt_conversion_flags flags;
    int width;				/* width, 0 if absent */
    int prec;				/* Precision, 0 if missing */

    union {
	int int_arg;
	wint_t wint_t_arg;
	long long_arg;
	long long long_long_arg;
	double double_arg;
	long double long_double_arg;
	short short_arg;
	void *void_p_arg;
	long *long_p_arg;
	int *int_p_arg;
	short *short_p_arg;
	char *char_p_arg;
	wchar_t *wchar_p_arg;
	unsigned int uint_arg;
	unsigned long ulong_arg;
	unsigned long long ulong_long_arg;
	double long_double_alloc[2];	/* ensures space for X_FLOAT */
    } value;
};
#pragma assert non_zero (sizeof(struct doprnt_conversion_item) == 4*sizeof(double)) \
    "Structure sized wrong for /l_double=64 interoperability."
/*
 * Define masks for flags bits.
 */
#define CNV_FLAG_SPACE 1		/* "% 3d" */
#define CNV_FLAG_NUMSIGN 2		/* "%#3d" */
#define CNV_FLAG_PLUS 4			/* "%+8d"   (include plus) */
#define CNV_FLAG_MINUS 8		/* "%-8d"   (left justify output) */
#define CNV_FLAG_ZERO 16		/* "%08d"   (fill with leading 0s */
#define CNV_FLAG_QUOTE 32		/* "%'8d"   (include thousands sep.*/

#define CNV_FLAG_WIDTH_FROM_AP 2048	/* "%*d"    (width from argument list */
#define CNV_FLAG_PREC_FROM_AP 4096      /* "%9.*e"  (precision from arg list */
#define CNV_FLAG_WIDTH_AP_POS 8192	/* "%*3$d    ( positional arg ) */
#define CNV_FLAG_PREC_AP_POS 16384	/* "%9.**2$d"  */
#define CNV_FLAG_ARG_AP_POS 32768	/* "%1$d"     ( positional arg) */
/*
 * float convert is a vector of floating point to string routines.  Since
 * size of long double varies, vector includes an argument retrieval function.
 */
typedef int (*float_convert)(struct doprnt_conversion_item *itm, 
	char *buffer, size_t bufsize );
struct doprnt_float_format_functions {
    float_convert fmt_float;
    float_convert fmt_double;
    float_convert fmt_long_double;
    float_convert fmt_imaginary;
    float_convert fmt_complex;
    va_list (*get_Ldouble)(va_list, long double *);
};
typedef struct doprnt_float_format_functions *doprnt_float_formatters;
/*
 * Float formatters that call ecvt and fcvt and use the finalize functions
 * to complete generating output for that conversion.  Note that digits
 * pointer may be null in case of ecvt/fcvt failure.
 */
int doprnt_ecvt_to_printf ( struct doprnt_conversion_item *cnv,
    const char *digits, int decpt, int sign, char *buffer, size_t bufsize );

int doprnt_fcvt_to_printf ( struct doprnt_conversion_item *cnv,
    const char *digits, int decpt, int sign, char *buffer, size_t bufsize );

int doprint_engine ( 
	char *buffer, 			/* I/O buffer, filled */
	const char *format_spec, 	/* printf format */
	va_list ap, 			/* printf variable arguments */
	size_t bufsize, 		/* Size of I/O buffer */
	void *output_arg, 
	int (*output_cb)( void *,char *, int, int * ),
	int *bytes_left,		/* Number of bytes remaining in buffer*/
	doprnt_float_formatters flt_vec );
/*
 * Pre-defined formatters, macro doprnt_compiled_float will pick the
 * one appropriate for the current /FLOAT=xxx /L_DOUBLE_SIZE=nnn settings.
 */
extern struct doprnt_float_format_functions doprnt_gx_float_formatters;
extern struct doprnt_float_format_functions doprnt_dx_float_formatters;
extern struct doprnt_float_format_functions doprnt_tx_float_formatters;
extern struct doprnt_float_format_functions doprnt_g_float_formatters;
extern struct doprnt_float_format_functions doprnt_d_float_formatters;
extern struct doprnt_float_format_functions doprnt_t_float_formatters;
#if __X_FLOAT
#if __G_FLOAT
#define doprnt_compiled_float doprnt_gx_float_formatters /* G, long double X */
#elif __IEEE_FLOAT
#define doprnt_compiled_float doprnt_tx_float_formatters /* T, long double X */
#else
#define doprnt_compiled_float doprnt_dx_float_formatters /* D, long double X */
#endif
#else
#if __G_FLOAT
#define doprnt_compiled_float doprnt_g_float_formatters /* G, long double G */
#elif __IEEE_FLOAT
#define doprnt_compiled_float doprnt_t_float_formatters /* T, long double T */
#else
#define doprnt_compiled_float doprnt_d_float_formatters /* D, long double D */
#endif
#endif  /* __X_FLOAT */

#endif
