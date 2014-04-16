/*
 * Play with pipes
 *
 * Command line:
 *     test_pipe.c [source_file|=]
 *
 *     If argv[1] is filename, file is sent to pipe.  If '=' then process
 *     is child.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <unixlib.h>
#include <stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include <openssl/evp.h>	/* for digest functions */
#include <starlet.h>

#include "memstream.h"

static unsigned char digest_value[EVP_MAX_MD_SIZE];
static unsigned int digest_vallen;
static EVP_MD_CTX digest_state;
static char *digest_name;
/*
 * Create a shared memory section.
 */
void *shared_memory ( const char *name, size_t size )
{
    void *blk;
    int mem_obj;
    off_t eof;

    mem_obj = shm_open ( name, O_CREAT | O_RDWR, 0660 );
    printf ( "mem_obj: %d\n", mem_obj );
    if ( mem_obj < 0 ) { perror ( "shm_open failed" ); return 0; }

    size = (size+8191) & (~8191);
    eof = lseek ( mem_obj, size-4, SEEK_SET );
    write ( mem_obj, "    ", 4 );
    fsync ( mem_obj );
    /* printf ( "Seek result: %x\n", eof ); */

    blk = mmap ( 0, size, PROT_WRITE, 
	MAP_VARIABLE | MAP_SHARED, mem_obj, 0 );
    if ( blk == MAP_FAILED ) {
	perror ( "mmap failed" );
	return 0;
    }
    return blk;
}

#ifdef VMS
#include <lib$routines.h>
#endif
/*
 * Allocate and return string describing device assigned to a file descriptor.
 */
static char *device_name ( int fd )
{
    struct stat info;
    char *name;
    int len;

    if ( fstat ( fd, &info ) < 0 ) name = strdup ( "<error>" );
    else {
#ifdef VMS
	/* Parse out st_dev */
	char *st_dev;
	len = strlen(info.st_dev);		/* string length */
	name = malloc ( len + 1 );
	memcpy ( name, &info.st_dev[1], len );
	name[len-1] = 0;
#else
	/* synthesize /dev/... name for device. */
	name= malloc ( 40 );
	sprintf ( name, "/dev/#%d/#%d", (int) major(info.st_dev),
		(int) minor(into.st_dev) );
#endif
    }
    return name;
}
static char *finalize_digest ( void )
{
    static char string[400];
    int i, len;

    string[0] = 0;
    if ( digest_name ) {
	sprintf ( string, ", %s:", digest_name );
	len = strlen ( string );
        EVP_DigestFinal ( &digest_state, digest_value, &digest_vallen );
        for (i=0; i<digest_vallen; i++) { 
	    sprintf( &string[len], " %02x", digest_value[i] );
	    len += strlen ( &string[len] );
	}
    }
    return string;
}

/*
 * Receive file sent over pipe and return byte count.
 */
static int alt_is_pipe = 0;
static alt_read ( int fd, memstream stream, void *buffer, size_t bufsize )
{
    int result;
    if ( alt_is_pipe ) {
	result = read ( fd, buffer, bufsize );
	return result;
    }
    return memstream_read ( stream, buffer, bufsize );
}

static alt_write ( int fd, memstream stream, void *buffer, size_t bufsize )
{
    int result;
    if ( alt_is_pipe ) {
	result = write ( fd, buffer, bufsize );
	return result;
    }
    return memstream_write ( stream, buffer, bufsize );
}

static pipe_sink ( int pfd[2], memstream mpipe[2] )
{
    int count, total_bytes, read_count, i;
    int ret_val;
    char buffer[22000];

    LIB$INIT_TIMER();
    read_count = 0;
    for ( total_bytes = 0; 
	(count=alt_read(pfd[0], mpipe[0], buffer, sizeof(buffer))) > 0;
	total_bytes += count ) {
	read_count++;
	/* printf ( "child read completed: %d\n", count ); */
	if ( digest_name ) EVP_DigestUpdate ( &digest_state, buffer, count );
    }
    LIB$SHOW_TIMER();
    printf ( "\nchild bytes read: %d, reads: %d%s\n",
	total_bytes, read_count, finalize_digest() );

    ret_val = total_bytes;
    /* count = write ( pfd[1], &ret_val, sizeof(ret_val) ); */
    count = alt_write ( pfd[1], mpipe[1], &ret_val, sizeof(ret_val) );
    if ( digest_name ) count = alt_write (pfd[1], mpipe[1], digest_value, digest_vallen );
    printf ( "Child Closing pipes, final write: %d\n", count );
    memstream_close ( mpipe[0] );
    memstream_close ( mpipe[1] );
    close ( pfd[0] );
    close ( pfd[1] );
}
/*
 * Read file and send to source.
 */
static pipe_source ( FILE *sf, int pfd[2], memstream mpipe[2] )
{
    int count, total_bytes, i;
    char buffer[20480];
    unsigned char rem_digest[EVP_MAX_MD_SIZE];

    LIB$INIT_TIMER();
    total_bytes = 0;
    while ( sf ) {
	count = fread ( buffer, 1, sizeof(buffer), sf );
	if ( count > 0 ) {
	    if ( digest_name ) EVP_DigestUpdate ( 
			&digest_state, buffer, count );
	    total_bytes += count;
	    count = alt_write (pfd[1], mpipe[1], buffer, count );
   		/*  printf ( "write count: %d\n", count ); */
	    if ( count == 0 ) break;
	    else if ( count < 0 ) {
		printf ( "Broken pipe trying to write\n" );
		break;
	    }
	} else break;
    }
    memstream_close ( mpipe[1] );
    decc$write_eof_to_mbx ( pfd[1] );
    decc$write_eof_to_mbx ( pfd[1] );
    close ( pfd[1] );
    printf ( "file sent, bytes: %d%s\n", total_bytes, finalize_digest() );

    count = alt_read ( pfd[0], mpipe[0], buffer, 4 );	/* bytecount+ */
    if ( digest_name ) count = alt_read ( pfd[0], mpipe[0], rem_digest, digest_vallen );
    printf ( "final read: %d %x %x %x %x\n", count, buffer[3],
	buffer[2], buffer[1], buffer[0] );
    for ( i = 0; i < digest_vallen; i++ ) if (rem_digest[i] != digest_value[i]) {
	printf ( "Digest mismatch!\n" );
	break;
    }
    close ( pfd[0] );
    memstream_close ( mpipe[0] );
    LIB$SHOW_TIMER();
    printf ( "Sleeping...\n" );
    sleep ( 4 );
}

static void timeout_ast ( char *commbuf )
{
    int status;
    printf ( "Child timed out, killing\n" );
    status = SYS$FORCEX ( 0, 0, 44 );
    printf ( "forcex status: %d\n", status );
}

static void dump_stats ( char *label, struct memstream_stats *rstats,
	struct memstream_stats *wstats )
{
    printf ( "%s stats reader: ops=%d, err=%d, seg=%d, waits=%d, wak=%d\n",
	label, rstats->operations, rstats->errors, rstats->segments, 
	rstats->waits, rstats->signals );
    printf ( "%s stats writer: ops=%d, err=%d, seg=%d, waits=%d, wak=%d\n",
	label, wstats->operations, wstats->errors, wstats->segments, 
	wstats->waits, wstats->signals );
}

int main ( int argc, char **argv, char *env[] )
{
    int pfd[3], pfd2[2], count;
    size_t blk_size;
    char *fname, *pname[2], *test;
    const EVP_MD *cur_md;
    FILE *dummyf;
    char cmd_line[712];
    char *alt_select, *child_timeout, *commbuf;
    memstream mpipe[2];
    struct memstream_stats rstats, wstats;

    blk_size = 0x8000;		/* 32K buffer */
    commbuf = shared_memory ( "MEMSTREAM_TEST", blk_size*2 );
    printf ( "Shared memory address: %x\n", commbuf );
    if ( !commbuf ) return 44;
    alt_select = getenv ( "TEST_MEMSTREAM_ALT_SELECT" );
    if ( alt_select ) alt_is_pipe = atoi ( alt_select );

    digest_vallen = 0;
    digest_name = getenv("TEST_MEMSTREAM_DIGEST");
    if ( digest_name ) {
	OpenSSL_add_all_digests ( );
	cur_md = EVP_get_digestbyname ( digest_name );
	if ( cur_md ) {
	    printf ( "Using OpenSSL digest %s\n", digest_name );
	    EVP_DigestInit ( &digest_state, cur_md );
	} else {
	    printf ( "Digest %s unknown to OpenSSL, ignored!\n", digest_name );
	    digest_name = 0;		/* disable used of digest */
	}
    }

    fname = (argc > 1) ? argv[1] : "test_pipe.c";
    if ( strcmp ( fname, "=" ) == 0  ) {
	/*
	 * Child process extract file descriptors and recieve file.
	 */
	char *p0fd, *p1fd;
	p0fd = getenv ( "PIPEFD0" );
	p1fd = getenv ( "PIPEFD1" );
	child_timeout = getenv ( "TEST_MEMSTREAM_CHILD_TIMEOUT" );
	if ( child_timeout ) {
	    long long delta = atoi ( child_timeout );  /* seconds */
	    delta = delta * -10000000;   /* convert to 100-nsec ticks */
	    SYS$SETIMR ( 3, &delta, timeout_ast, commbuf, 0 );
	}

	printf ( "child process active...(%s, %s)\n", p0fd?p0fd:"<NULL>",
		p1fd?p1fd:"<NULL>");
        mpipe[0] = memstream_create ( &commbuf[blk_size], blk_size, 0 );
	mpipe[1] = memstream_create ( &commbuf[0], blk_size, 1 );
	memstream_assign_statistics ( mpipe[0], &rstats );
	memstream_assign_statistics ( mpipe[1], &wstats );

	pfd[0] = atoi ( p0fd );
	pfd[1] = atoi ( p1fd );
	pname[0] = device_name(pfd[0]);
	pname[1] = device_name(pfd[1]);
	/* child process, pipes named by argv[argc-2] and argv[argc-1] */
	printf ( "child opens: %d/'%s' %d/'%s'\n", pfd[0], pname[0],
		pfd[1],pname[1] );
	pipe_sink ( pfd, mpipe );
	dump_stats ( "child", &rstats, &wstats );
	return 0;
    }
    /*
     * Master process.  Open file and create pipe.
     */
    dummyf = fopen ( fname, "r" );
    if ( !dummyf ) {
	perror ( "file open failue" );
	return 0;
    }

    memset ( commbuf, 0, 40 );
    memset ( &commbuf[blk_size], 0, 40 );
    mpipe[0] = memstream_create ( &commbuf[0], blk_size, 0 );
    mpipe[1] = memstream_create ( &commbuf[blk_size], blk_size, 1 );
    memstream_assign_statistics ( mpipe[0], &rstats );
    memstream_assign_statistics ( mpipe[1], &wstats );

    if ( pipe ( pfd ) != 0 ) {
	perror ( "Pipe() call" );
	return 44;
    }
    pfd2[1] = pfd[1];			/* child write to mbx I read */
    /* close ( pfd[1] ); */
    pipe ( &pfd[1] );
    pfd2[0] = pfd[1];			/* child reads from mbx I write */
    /* close ( pfd[1] ); */
    pfd[1] = pfd[2];			/* make independant mailboxes */
    pname[0] = device_name(pfd[0]);
    pname[1] = device_name(pfd[1]);
    printf ( "pfd[0] = %d '%s' %d\n", pfd[0], pname[0], strlen(pname[0]) );
    printf ( "pfd[1] = %d '%s' %d\n", pfd[1], pname[1], strlen(pname[1]) );

    sprintf ( cmd_line, "mcr %s %s %s %s\n", argv[0], (argc > 1) ? argv[1] :
	"", pname[0], pname[1] );
    /*
     * Spawn ourselves to be pipe sink via vfork/exec mechanism.  Use
     * environment array to pass file descriptor numbers.
     */
    if ( dummyf ) {
	pid_t child;
	int i;
	char *child_arg[10], *child_env[20];

	child_arg[0] = "test_pipe";
	child_arg[1] = "=";		/* flag to be sink */
	child_arg[2] = pname[0];
	child_arg[3] = pname[1];
	child_arg[4] = 0;	/* end of list */
	for ( i = 0; env[i] && (i < 17); i++ ) {
	    child_env[i] = env[i]; /* printf ( "env[%d] = '%s'\n",i,env[i]); */
	}
	child_env[i] = malloc ( 80 );
	sprintf ( child_env[i], "PIPEFD0=%d", pfd2[0] );
	i++;
	child_env[i] = malloc ( 80 );
	sprintf ( child_env[i], "PIPEFD1=%d", pfd2[1] );
	child_env[i+1] = 0;

	child = vfork ( );
	if ( child == 0 ) {
	    /* fake child 'fork', issue exec call */
	    execve ( argv[0], child_arg, child_env );
	    perror ( "evecve failue" );
	} else if ( child != -1 ) {
	    /*
	     * Success, proceed to send file.
	     */
	    printf ( "Process, created, sending file\n" );
	    close ( pfd2[0] );
	    close ( pfd2[1] );
	    pipe_source ( dummyf, pfd, mpipe );

	    dump_stats ( "master", &rstats, &wstats );
	} else {
	    perror ( "vfork failure" );
	}

	fclose ( dummyf );
    }
    return 0;
}
