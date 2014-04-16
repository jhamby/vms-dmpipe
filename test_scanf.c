#include "dmpipe.h"
#include <string.h>

int main ( int argc, char **argv )
{
    int status, count, value;
    char word[30000];
    float fvalue;
    double dvalue;
    /*
     * read tokens.
     */
    count = 0;
    printf ( "Scanning words...\n" );
    while ( 1 == scanf ( "%20s", word ) ) {
	printf ( "word: %s\n", word );
	if (strcmp(word,"icheck")==0) {
	    status=scanf("%i",&value);
	    printf("value: %d(%d)\n", value,status);
	}
	if (strcmp(word,"fcheck")==0) {
	    status=scanf("%e",&fvalue);
	    printf("fvalue: %g(%d)\n", fvalue,status);
	}
	if (strcmp(word,"dcheck")==0) {
	    status=scanf("%le",&dvalue);
	    printf("fvalue: %g(%d)\n", dvalue,status);
	}
	count++;
    }
    fvalue = 3.14;
    printf ( "Final word count: %d (pi=%g)\n", count, fvalue );
    return 0;
}
