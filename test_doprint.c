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
#include <math.h>		/* compaq portable math library */

#include "doprint.h"

static int ilog10 ( double value )
{
    static int pow10_mask[16] = {0};
    static double  pow10[16];	   /* pow10[i] = 10^(2^i) */
    static double invpow10[16];
    int i, result;
    double exponent;

    if ( pow10_mask[0] == 0  ) {
	/* Initialize on first call */
	pow10_mask[0] = 1;		/* bit 0 mask value */
	pow10[0] = 1.0;
	invpow10[0] = 1.0;

	pow10_mask[1] = 2;			/* bit 1 mask value */
	pow10[1] = 10.0;			/* 10^(2^i) == 10^(pow10_mask[i]) */
	invpow10[1] = 1.0 / pow10[1];

	for ( i = 2; i < 5; i++ ) {	/* 2^16 */
	    pow10[i] = pow10[i-1] * pow10[i-1];
	    invpow10[i] = invpow10[i-1];
	}
    }

    result = 0;
    exponent = 1.0;
    if ( value > 1.0 ) {
	i = 0;
	while ( exponent < value ) {
	    i = i + 1;
	    exponent = exponent * pow10[i];
	}
	result |= pow10_mask[i-1];
	exponent = pow10[i-1];

	i = i - 2;
	while ( i > 0 ) {
	    if ( exponent*pow10[i] < value ) {
		exponent = exponent * pow10[i];
		result |= pow10_mask[i];
	    }
	    i--;
	}
    } else if ( value < 1.0 ) {
    } else {
	return 0;	/* value == 1 */
    }
    return result;
}

static char *my_ecvt ( double value, int ndigits, int *decpt, int *sign )
{
    static char buffer[400];
    double logv, base, divisor, remainder, digit;
    int len, i, iexponent;
    /*
     * Convert negative value to positive and not sign change.
     */
    if ( value < 0.0 ) {
	*sign = 1; 
	value = value * (-1.0);
    } else *sign = 0;
    /*
     * Use log10 function to determine exponent and number of
     * digits to left of decimal.  ceil() function for number of digits
     * accounts for .5 => 5.e-1.
     */
    if ( value == 0.0 ) {
	iexponent = 0;
	*decpt = 1;
    } else {
        logv = log10(value);
        iexponent = trunc(logv);
        *decpt = ceil(logv);
    }
    /*
     * Setup initial divisor for generating digits (digit=value/divisor).
     */
    base = 10.0;
    divisor = 1.0;

    for ( i=iexponent; i > 0; i-- ) divisor = divisor*base;
    while ( i < 0 ) { i++; divisor = divisor/base; }
    /*
     * Generate ndigits output digits.  Always make at least 1.
     */
    len = 0;
    do {
	digit = trunc(value/divisor);
	if ( (len==0) && (digit >= 10.0) ) {
	    /* correct for round-off error in first division */
	    buffer[len++] = '1';
	    digit = digit - 10.0;
	    *decpt = *decpt + 1;
	}
	buffer[len++] = digit + '0';
	value = value - (digit*divisor);
	divisor = divisor / base;
    } while ( len < ndigits );
    buffer[len] = 0;
    return buffer;
}

static char *my_fcvt ( double value, int ndigits, int *decpt, int *sign )
{
    static char buffer[400];
    double logv, base, divisor, remainder, digit;
    int len, iexponent, i;
    /*
     * Convert negative value to positive and not sign change.
     */
    if ( value < 0.0 ) {
	*sign = 1; 
	value = value * (-1.0);
    } else *sign = 0;
    /*
     * Use log10 function to determine exponent and number of
     * digits to left of decimal.  ceil() function for number of digits
     * accounts for .5 => 5.e-1.
     */
    if ( value == 0.0 ) {
	iexponent = 0;
	*decpt = 1;
    } else {
        logv = log10(value);
        iexponent = trunc(logv);
        *decpt = ceil(logv);
    }
    /*
     * Setup initial divisor for generating digits (digit=value/divisor).
     * divisor = 10^(iexponent).
     */
    base = 10.0;
    divisor = 1.0;
    for ( i=iexponent; i > 0; i-- ) divisor = divisor*base;
    while ( i < 0 ) { i++; divisor = divisor/base; }
    /*
     * Output string until ndigits output digits have been produced.
     */
    len = 0;
    while ( (ndigits > 0) || (divisor >= 1.0) ) {
	digit = trunc(value/divisor);
	if( (len==0) && (digit >= 10.0) ) {
	    buffer[len++] = '1';
	    digit = digit - 10.0;
	    *decpt = *decpt + 1;
	}
	buffer[len++] = digit + '0';
	value = value - (digit*divisor);
	if ( divisor < 1 ) ndigits--;
	divisor = divisor / base;
    }
    buffer[len] = 0;
    return buffer;
}

static int test_flush ( void *fp_vp, char *buffer, int len, int *left )
{
    FILE *fp;
    fp = fp_vp;
    *left = 0;
    return fwrite ( buffer, 1, len, fp );
}

static int test_printf ( const char *fmt, ... )
{
    va_list ap;
    int status, bytes_left, flleft;
    char buffer[1024];

    va_start(ap,fmt);
    status = doprint_engine ( buffer, fmt, ap, sizeof(buffer), stdout, 
	test_flush, &bytes_left, &doprnt_compiled_float );

    if ( bytes_left > 0 ) test_flush ( stdout, buffer, bytes_left, &flleft);
    printf ( "doprint engine ('%s',..) result: %d, left: %d\n", fmt, status,
	bytes_left );
    return status;
}
#pragma assert func_attrs(test_printf) format(printf,1,2)

static char *display_cvt ( const char *cvt, int decpt, int sign,
	char spec, char *buffer )
{
    char *cp;
    int exponent;
    /*
     * Start with sign.
     */
    if ( (spec == 'e') || (spec == 'E') ) {
        exponent = decpt - 1;
	decpt = 1;
    }
    cp = buffer;
    if ( sign ) *cp++ = '-';
    /*
     * Adjust for decimal point.
     */
    if ( decpt < 0 ) {
	*cp++ = '0';
	for ( *cp++ = '.'; decpt < 0; decpt++ ) *cp++ = '0';
    } else {
	for ( ; (decpt > 0) && *cvt; decpt-- ) *cp++ = *cvt++;
	*cp++ = '.';
    }
    while ( *cvt ) *cp++ = *cvt++;
    *cp = 0;
    if ( (spec == 'e') || (spec == 'E') ) {
	*cp++ = spec;
	sprintf ( cp, "%+03d", exponent );
    }

    return buffer;
}

int main ( int argc, char **argv, ... )
{
    int i, count, ndigits, decpt, sign, bytes_left, width, prec;
    char buffer[1024], *cvt_result, *my_cvt_result;;
    double pi;

    pi = acos(-1.0);
    width = 8;
    prec = 6;

    if ( argc > 1 ) {
	ndigits = atoi ( argv[1] );

	i = 1;
	if ( (i < argc) && !strchr(argv[i],'.') ) {
	    width = atoi ( argv[i] ); i++;
	    printf ( "Reset width to %d\n", width );
	}
	if ( (i < argc) && !strchr(argv[i],'.') ) {
	    prec = atoi ( argv[i] ); i++;
	    printf ( "Reset precisition to %d\n", prec );
	}
	do {
	    cvt_result = ecvt ( pi, ndigits, &decpt, &sign );
	    printf ( "ecvt(pi,%d,...) = '%s' %d %d => '%s' (%*.*E)\n", ndigits, 
		cvt_result, decpt, sign, 
		display_cvt(cvt_result,decpt,sign,'e',buffer), width, prec, pi);
	    my_cvt_result = my_ecvt ( pi, ndigits, &decpt, &sign );
	    printf ( "myecvt(pi,%d,...)='%s' %d %d => '%s' (%*.*e)\n", ndigits, 
		my_cvt_result, decpt, sign, 
		display_cvt(cvt_result,decpt,sign,'e',buffer), width, prec, pi );

	    cvt_result = fcvt ( pi, ndigits, &decpt, &sign );
	    printf ( "fcvt(pi,%d,...) = '%s' %d %d => '%s' (%*.*f)\n", 
		ndigits, cvt_result, decpt, sign, 
		display_cvt(cvt_result,decpt,sign,'f',buffer), width, prec, pi );

	    cvt_result = my_fcvt ( pi, ndigits, &decpt, &sign );
	    printf ( "myfcvt(pi,%d,...)='%s' %d %d => '%s' (%*.*f)\n", 
		ndigits, cvt_result, decpt, sign, 
		display_cvt(cvt_result,decpt,sign,'f',buffer), width, prec, pi );

	    cvt_result = gcvt ( pi, ndigits, buffer );
	    printf ( "gcvt(pi,%d,...) = '%s' (%g)\n", ndigits, cvt_result, pi );

	    i++;
	    if ( i < argc ) {
		if ( argv[i][0] == 'w' ) {
		    width = atoi ( argv[i]+1 );
		} else if ( argv[i][0] == 'p' ) {
		    prec = atoi ( argv[i]+1 );
		} else sscanf ( argv[i], "%lg", &pi );
		printf ( "-------------------------\nPi now '%s' > %g\n",
		    argv[i], pi );
	    } else break;
	} while ( 1 );

    }

    count = test_printf ( "%s\n", "single string" );
    count = test_printf ( "Pi: '%8.6f' (f), '%8.6e' (e)\n", pi, pi );
    count = test_printf ( "again: '%s'\n", "single string" );
    count = test_printf ( "Single int: '%d'\n", 15 );
    return 0;
}
