#ifndef DOSCAN_H
#define DOSCAN_H
/*
 * Private imeplentatin of C$SCANF scanf engine.  Given a format string and
 * argument list, generate a stream of output characters returned to the
 * call via a buffer and buffer flush routine.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <wctype.h>		/* for wint_t */
#include <stdio.h>
/*
 * Conversion_item structure is output of parse_conversion_descriptor
 * function except for value member, which is loaded later by caller.
 * Flags indicate format flags prefixes as well as arglist source items.
 */
struct doscan_conversion_flags {
    unsigned int discard:    1,	    /* "%*", Don't store converted value */
	ws_precedes:1,
	ap_pos_n:   1,
	scanset_1:  1,	    /* "%1[ */
	uppercase:  1,      /* E, G, or X specifier characters */
	invert_sset:1,	    /* "%[^", Invert scanset chars. */
	sizeq:      2,      /* size qualifier: 0-none, 1-h, 2-l, 3-L/ll */
	fill:      24;      /* kick up to 32 bits */
};
struct doscan_conversion_item {
    int specifier_char;
    struct doscan_conversion_flags flags;
    char *scan_set;
    int width;				/* width, -1 if absent */

    union {
	int *int_arg;
	wint_t *wint_t_arg;
	long *long_arg;
	long long *long_long_arg;
        float *float_arg;
	double *double_arg;
	long double *long_double_arg;
	short *short_arg;
	void **void_p_arg;
	long **long_p_arg;
	int **int_p_arg;
	short **short_p_arg;
	char *char_p_arg;
	wchar_t *wchar_p_arg;
	unsigned int *uint_arg;
	unsigned long *ulong_arg;
	unsigned long long *ulong_long_arg;
	void *void_arg;
    } value;
    char *matchset;
    char small_matchset[20];
};
/*
 * float convert is a vector of string to floating point routines.  String to
 * be converted has been parsed into mantissa and exponent, exponent is
 * null pointer if none.
 */
typedef int (*doscan_vfscanf_fallback) ( FILE *, const char *, va_list );
typedef int (*doscan_float_convert)(struct doscan_conversion_item *itm, 
	char *numstr, int exp_offset );
struct doscan_float_format_functions {
    doscan_vfscanf_fallback fallback;
    doscan_float_convert fmt_float;
    doscan_float_convert fmt_double;
    doscan_float_convert fmt_long_double;
    doscan_float_convert fmt_imaginary;
    doscan_float_convert fmt_complex;
};
typedef struct doscan_float_format_functions *doscan_float_formatters;
/*
 * Float formatters that call ecvt and fcvt and use the finalize functions
 * to complete generating output for that conversion.  Note that digits
 * pointer may be null in case of ecvt/fcvt failure.
 */
int doscan_ecvt_to_printf ( struct doscan_conversion_item *cnv,
    const char *digits, int decpt, int sign, char *buffer, size_t bufsize );

int doscan_fcvt_to_printf ( struct doscan_conversion_item *cnv,
    const char *digits, int decpt, int sign, char *buffer, size_t bufsize );
/*
 * Callback function offloads part of the scan function back to the
 * caller.  Scan engine calls it to optionally skip white-space and return 
 * tokens of non white-space or with defined match/nonmatch sets.
 *
 * Scan control bits:
 *    <0> skip leading whitespace.
 *    <1> Ignore match set and include all non-whitespace characters.
 *    <2> Invert match set.
 *    <3> force next character into output even if not in match set.
 *
 * Return value is number of characters transferred.  If return value less
 * than bufsize, term_char points to next character (i.e. the delimiter).
 * Otherwise term_char returns a null pointer.
 */
#define DOSCAN_CTL_SKIP_WS 1
#define DOSCAN_CTL_MATCH_TO_WS 2
#define DOSCAN_CTL_INVERT 4
#define DOSCAN_CTL_MATCH_FIRST 8
#define DOSCAN_CTL_STRING (DOSCAN_CTL_SKIP_WS|DOSCAN_CTL_MATCH_TO_WS)

typedef int (*doscan_callback) ( void *cb_arg, int scan_control, 
	const char *matchset, char *buffer, int bufsize, char **term_char );

int doscan_engine ( 
	const char *format_spec, 	/* printf format */
	va_list ap, 			/* printf variable arguments */
	void *input_arg, 
	doscan_callback input_cb,
	doscan_float_formatters flt_vec );
/*
 * Pre-defined formatters, macro doscan_compiled_float will pick the
 * one appropriate for the current /FLOAT=xxx /L_DOUBLE_SIZE=nnn settings.
 */
extern struct doscan_float_format_functions doscan_gx_float_formatters;
extern struct doscan_float_format_functions doscan_dx_float_formatters;
extern struct doscan_float_format_functions doscan_tx_float_formatters;
extern struct doscan_float_format_functions doscan_g_float_formatters;
extern struct doscan_float_format_functions doscan_d_float_formatters;
extern struct doscan_float_format_functions doscan_t_float_formatters;
#if __X_FLOAT
#if __G_FLOAT
#define doscan_compiled_float doscan_gx_float_formatters /* G, long double X */
#elif __IEEE_FLOAT
#define doscan_compiled_float doscan_tx_float_formatters /* T, long double X */
#else
#define doscan_compiled_float doscan_dx_float_formatters /* D, long double X */
#endif
#else
#if __G_FLOAT
#define doscan_compiled_float doscan_g_float_formatters /* G, long double G */
#elif __IEEE_FLOAT
#define doscan_compiled_float doscan_t_float_formatters /* T, long double T */
#else
#define doscan_compiled_float doscan_d_float_formatters /* D, long double D */
#endif
#endif  /* __X_FLOAT */

#endif	/* DOSCAN_H */
