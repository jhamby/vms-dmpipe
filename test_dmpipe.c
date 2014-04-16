/*
 * Simple program to test dmpipe functions
 */
#include <stdio.h>

#include "dmpipe.h"

int main ( int argc, char **argv )
{
    int i;
    printf ( "Hello world\n" );

    for ( i = 0; i < argc; i++ ) {
	printf ( "argv[%d] = '%s'\n", i, argv[i] );
    }
    return 1;
}
