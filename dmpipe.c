/*
 * Wrapper functions for unistd and stdio functions that give pipes a
 * special bypass of normal CRTL I/O.
 *
 * The primary data structure is dm_fd_extension.  One exists for every
 * potential file descripror number and shadows any file descripror argument
 * passed to a dm_xxxx function (not just those returned by dm_open().  A FILE
 * pointer is shadowed by the file descriptor associated by the RTL (i.e.
 * fileno(fp).  When firt referenced, the file is checked to see if it is
 * a pipe and if so the extension links to a dm_bypass structure manage
 * bypassing the I/O operatons.  If there is no bypass (not a pipe or 
 * other process unable to use bypass) then I/O calls pass through
 * transparently to the underlying CRTL functions.
 *
 * dm_poll() and dm_select() and special cases that replace the corresponding
 * crtl functions with enhanced functionality.
 *
 * Special linking must be done because these functions make unsupported
 * calls into the C runtime.
 *
 * Author: David Jones
 * Date:   19-MAR-2014
 * Revised:23-MAR-2014			Add dm_poll(), dm_select(), dm_fcntl().
 * Revised: 3-APR-2014			Add dm_scanf(), dm_fscanf().
 * Revised: 7-APR-2014			Add dm_perror() function.
 * Revised:15-APR-2014			Add support for stderr propagation.
 * Revised:16-APR-2014			Incoporate change to dm_bypass_read.
 * Revised: 17-APR-2014			Support expedite flag (flush).
 * Revised: 19-APR-2014			Support flush all in fflsuh.
 * Revised: 19-APR-2014			Support DM_BYPASS_HINT_POPEN_R flag,
 *					which keeps popen(cmd,"r") from
 *					hanging.
 * Revised: 20-APR-2014			Fix bug in dm_fgetc.
 */
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <unistd.h>		/* pipe definitions, pipe(), close() */
#include <fcntl.h>		/* open() */
#include <unixio.h>		/* isapipe function */
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <errnodef.h>		/* C$_Exxx errno VMS condition codes */
#include <lib$routines.h>

/* TRACE */
#include <stddef.h>
#include <descrip.h>
#include <dscdef.h>
#include <jpidef.h>
/* END TRACE */

#define DM_NO_CRTL_WRAP		/* Don't override CRTL names (pipe, etc) */
#include "dmpipe.h"
#include "dmpipe_bypass.h"	/* memory functions */
#include "dmpipe_poll.h"	/* Caches poll hack */
#include "doscan.h"
/*
 * Inbuf is used as working buffer for fgetc/ungetc, fgets, scanf
 */
#define DM_INBUF_IOSIZE 1024
#define DM_INBUF_LOOKBACK 16
#define DM_INBUF_BUFSIZE (DM_INBUF_IOSIZE+DM_INBUF_LOOKBACK)

struct dm_inbuf {
    int rpos;			/* position of next character to read */
    int length;			/* position of first unread char */
    char buffer[DM_INBUF_BUFSIZE];
    char eob[4];		/* allows writing null to buffer[length] */
};
/*
 * Global variables.  Track auxillary information about open file
 * descriptors (fds).  We have to potentially map 65K of fds, but
 * usually only a few low-numbered fds are used.  Make a table of 1024 rows
 * of 64 extension structures, row 0 (covering fds 0-63) is statically 
 * allocated and others will be allocated on first reference to that row.
 */
struct dm_fd_extension {
    unsigned short int initialized, fd;
    int bypass_flags;
    FILE *fp;
    dm_poll_track poll_ext;
    dm_bypass bp;
    unsigned long write_ops;	/* write operations invoked */
    unsigned long read_ops;     /* read operations invoked */
    struct dm_inbuf *inbuf;
    int fcntl_flags;		/* for fcntl() support */
};
#define FD_EXTENSION_MAP_ROWS 256*4
#define FD_EXTENSION_MAP_COLS 64

static int dm_fd_ext_high_row = 8;	/* highest initialized row pointer */
static struct dm_fd_extension dm_fd_ext_row0[FD_EXTENSION_MAP_COLS] = {
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
    { 0 }, { 0 }, { 0 }, { 0 }
};
static struct dm_fd_extension *dm_fd_extrow[FD_EXTENSION_MAP_ROWS] = {
    dm_fd_ext_row0, 0, 0, 0, 0, 0 ,0, 0, 0 
};
static int inbuf_min_delay_control = 0;
static FILE *tty = 0;
/*
 * For functions that use a *FILE argument, keep a small cache of their
 * corresponding filenos;
 */
#define FP_TO_FD_TLB_SIZE 3
static struct {
    FILE *fp;
    struct dm_fd_extension *fdx;
} dm_fp_map[FP_TO_FD_TLB_SIZE] = { {0,0}, {0,0}, {0,0} };

/* TRACE */
FILE *fp_trace=NULL;
char dmpipe_trace=0;
char CleanupInitialized=0;
char CleanupDone=0;
/* END TRACE */

/*
 * Broken pipe check raises signal if write to pipes fail.  DECC uses
 * SIGPIPE for multiple exceptions, raise with gsignal() to provide
 * the proper subcode for C$_SIGPIPE.
 */
#define BROKEN_PIPE_CHECK(sts,fdesc) \
   if ( ((sts) < 0) && (fdesc<65000) ) /* raise (SIGPIPE); */ \
   gsignal ( SIGPIPE, 1 );
   
/* TRACE */
char *generate_dmpipe_trace_file_name()
{
char *cmd_name=NULL;
$DESCRIPTOR(cmd_name_dsc, cmd_name);
unsigned short cmd_name_len;
unsigned long vms_status;
unsigned long jpi_image_item=JPI$_IMAGNAME;
char *cp_start;
char * dmpipe_dbg_filename=NULL;

cmd_name = (char *)malloc(512);
cmd_name_dsc.dsc$w_length = 511;
cmd_name_dsc.dsc$b_dtype = DSC$K_DTYPE_T;
cmd_name_dsc.dsc$b_class =  DSC$K_CLASS_S;
cmd_name_dsc.dsc$a_pointer = cmd_name;
vms_status = lib$getjpi(&jpi_image_item, NULL, NULL, NULL, &cmd_name_dsc, &cmd_name_len);
cmd_name[cmd_name_len] = '\0';
cp_start = strrchr(cmd_name, ']') + 1;
*strrchr(cmd_name, '.') = '\0';
dmpipe_dbg_filename = (char *)malloc(strlen(cp_start) + 18);
sprintf(dmpipe_dbg_filename, "dmpipe_trace_%s.txt", cp_start);
return dmpipe_dbg_filename;
}

FILE *open_dmpipe_trace_file()
{
char *fname = generate_dmpipe_trace_file_name();
FILE *fptr=NULL;

fptr = fopen(fname, "w+");
free((void *)fname);
fprintf(fptr, "dmpipe trace file opened using file pointer %p\r\n", fptr);
fflush(fptr);
return fptr;
}

void dmpipe_trace_output(const char *cp_format, ...)
{
va_list vargs;

if (dmpipe_trace != 0)
   {
   va_start(vargs, cp_format);
   if (fp_trace == NULL) fp_trace = open_dmpipe_trace_file();
   vfprintf(fp_trace, cp_format, vargs);
   fflush(fp_trace);
   }
}

void dmpipe_reset_trace_file()
{
if (fp_trace != NULL)
   {
   fclose(fp_trace);
   fp_trace = NULL;
   }
fp_trace = open_dmpipe_trace_file();
}
/* END TRACE */

void CloseAllDMPipeBypasses(void)
{
int fdrow;
int fdcol;

for (fdrow=0; (fdrow < (dm_fd_ext_high_row + 1)); fdrow++)
   {
   if (dm_fd_extrow[fdrow] != NULL)
      {
      for (fdcol=0; (fdcol < FD_EXTENSION_MAP_COLS); fdcol++)
         {
         if ( dm_fd_extrow[fdrow][fdcol].initialized )
            {
            /* Rundown bypass */
	    dm_fd_extrow[fdrow][fdcol].initialized = 0;
	    if ( dm_fd_extrow[fdrow][fdcol].bypass_flags && dm_fd_extrow[fdrow][fdcol].bp )
	       {
	       dm_bypass_shutdown ( dm_fd_extrow[fdrow][fdcol].bp );
	       dm_fd_extrow[fdrow][fdcol].bp = 0;
	       }
            }
         }
      }
   }
CleanupDone = 1; 
}

/*************************************************************************/
/*
 * Main functions for managing extension blocks:
 *     init_extension
 *     find_extension
 *     find_fp_extension
 *     rundown_extension
 */
static struct dm_fd_extension *find_fp_extension(FILE *fp, int ini_if);
static void init_extension ( struct dm_fd_extension *fdx, int fd, FILE *fp )
{
    char nambuf[512];
    /*
     * Zero-out and fill in caller's arguments.
     */
    memset ( fdx, 0, sizeof(struct dm_fd_extension) );
    fdx->initialized = 1;
    fdx->fd = fd;
    fdx->fp = fp;
    /*
     * Special handling for stderr (are there macros for std stream fds?).
     */
    if ( fd == 2 ) {
	/*
	 * If we are a child process (have parent) created by popen(), redirect
	 * stderr from the pipe (stdout) to the parent's rather than our stdour.
         */
	pid_t parent;
	struct dm_fd_extension *ofdx;
	parent = getppid ( );
	if ( parent ) {
	    /* Initialize stdout if we haven't done so yet so we can
	       * retrieve the output device */
	    ofdx = find_fp_extension ( stdout, 1 );
	    if ( ofdx && ofdx->initialized && ofdx->bp ) {
	        dm_bypass_stderr_recover ( parent, ofdx->bp );
	    }
	}
    }
    /*
     * Determine if device used by FP capable of being bypassed.
     */
     
    fdx->bp = dm_bypass_init ( fd, &fdx->bypass_flags );
    
#ifdef DEBUG
if ( !tty ) tty = fopen ( "DBG$OUTPUT:", "a", "shr=put" );
    fprintf (tty, "/dmpipe/ init extension for file[%d] (%s), fp=%x, flags=%d\n", 
	fd, getname(fd,nambuf), fp, fdx->bypass_flags);
#endif
}

static void rundown_extension ( struct dm_fd_extension *fdx )
{
    /*
     * Free resources used by extension and mark uninitialized.
     */
    if ( fdx->bypass_flags ) {
	fdx->bypass_flags = 0;
    }
    if ( fdx->bp ) dm_bypass_shutdown ( fdx->bp );
    fdx->bp = 0;
    fdx->initialized = 0;
}

static struct dm_fd_extension *find_extension ( int fd, int init_if )
{
    int fd_row, fd_col, row_ndx;

    if ( fd < 0 ) {
	errno = EINVAL;
	return 0;		/* invalid fd */
    }

    if ( fd < FD_EXTENSION_MAP_COLS ) {
	/* Optimized case, extension in first row */
	if ( init_if && !dm_fd_ext_row0[fd].initialized ) {
	    /*
	     * Initialize extension.
	     */
	    init_extension ( &dm_fd_ext_row0[fd], fd, 0 );
	}
	return &dm_fd_ext_row0[fd];
    }
    /*
     * Split fd into array address and check bounds.
     */
    fd_row = fd / FD_EXTENSION_MAP_COLS;
    fd_col = fd % FD_EXTENSION_MAP_COLS;
    if ( fd_row >= FD_EXTENSION_MAP_ROWS ) {
	errno = EINVAL;
	return 0;
    }
    /*
     * Extend high row if needed, zeroing pointers in intervening rows.
     */
    if ( fd_row > dm_fd_ext_high_row ) {
	for (row_ndx=dm_fd_ext_high_row+1; row_ndx <= fd_row; row_ndx++) {
	    dm_fd_extrow[row_ndx] = 0;
	}
	dm_fd_ext_high_row = fd_row;
    }
    /*
     * Allocate row if first time accessing and fd in this row.
     */
    if ( !dm_fd_extrow[fd_row] ) {
	/*  Allocate row */
	dm_fd_extrow[fd_row] = calloc ( FD_EXTENSION_MAP_COLS,
		sizeof(struct dm_fd_extension) );
	if ( !dm_fd_extrow[fd_row] ) {
	    errno = ENOMEM;
	    return 0;
	}
    }
    /*
     * Check for initialization, then return extension block to caller.
     */
    if ( init_if && !dm_fd_extrow[fd_row][fd_col].initialized ) {
	init_extension ( &dm_fd_extrow[fd_row][fd_col], fd, 0 );
    }

    return (dm_fd_extrow[fd_row])+fd_col;
}

static struct dm_fd_extension *find_fp_extension ( FILE *fp, int init_if )
{
    int fd, i;
    struct dm_fd_extension *fdx;
    /*
     * Search cache.
     */
    for ( i = 0; i < FP_TO_FD_TLB_SIZE; i++ ) {
	if ( fp == dm_fp_map[i].fp ) {
	    fdx = dm_fp_map[i].fdx;
	    if ( !fdx || !fdx->initialized ) break;
	    return fdx;		/* found in cache */
	}
    }
    /*
     * Not in cache, lookup by file descriptor, then associate file pointer
     * with this extension.
     */     
    fd = fileno ( fp );
    fdx = find_extension ( fd, init_if );

    if ( !fdx ) return fdx;
    if ( !fdx->fp ) fdx->fp = fp;
    /*
     * Replace oldest entry in cache current fp.
     */
    if ( !fdx->initialized ) return fdx;
    for ( i = FP_TO_FD_TLB_SIZE-1; i > 0; --i ) {
	dm_fp_map[i] = dm_fp_map[i-1];
    }
    dm_fp_map[0].fp = fp;
    dm_fp_map[0].fdx = fdx;


    return fdx;
}

static void broken_pipe ( int fd )
{
    exit ( EPIPE );
}
/*************************************************************************/
/*
 * Define functional replacements for CRTL I/O routines that bypass
 * pipe device and use shared memory instead.  The dual mode pipe retain
 * legacy mailbox operations unless both sides are using these functions.
 */

int dm_pipe ( int fds[2] )
{
    int status;
    struct dm_fd_extension *fdx;
/* TRACE */
dmpipe_trace_output("dm_pipe()\r\n");
/* END TRACE */
    status = pipe ( fds );
    if ( status != 0 ) return status;
    
    fdx = find_extension ( fds[0], 1 );
    fdx = find_extension ( fds[1], 1 );

    return status;
}

ssize_t dm_read ( int fd, void *buffer_vp, size_t nbytes )
{
    int status, expedite_flag;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_read()\r\n");
/* END TRACE */
    fdx = find_extension ( fd, 1 );
    if ( fdx->bypass_flags ) {
	if ( fdx->read_ops == 0 ) {
	    /* First time reading, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "r", fdx->fcntl_flags );
	}
	fdx->read_ops++;
	if ( fdx->bypass_flags & DM_BYPASS_HINT_READS ) {
	    int count;
	    count = dm_bypass_read ( fdx->bp, buffer_vp, nbytes, nbytes, 0 );
	    if ( count < 0 ) return -1;
	    if ((count == 0) && (fdx->bypass_flags&DM_BYPASS_HINT_POPEN_R)) {
	        /*
	         * See dm_fread for explanation of freopen().
	         */
	        if ( fdx->fp ) fdx->fp = freopen ( "_NL:", "r", fdx->fp );
	        fdx->bypass_flags ^= DM_BYPASS_HINT_POPEN_R;
	    }
	    return count;
	}
    }

    return read ( fd, buffer_vp, nbytes );
}

ssize_t dm_write ( int fd, const void *buffer_vp, size_t nbytes )
{
    int status;
    struct dm_fd_extension *fdx;
    fdx = find_extension ( fd, 1 );

/* TRACE */
dmpipe_trace_output("dm_write()\r\n");
/* END TRACE */
    if ( fdx->bypass_flags ) {
	if ( fdx->write_ops == 0 ) {
	    /* First time writing, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
	}
	fdx->write_ops++;
	/*
	 * Switch to bypass if stream established, otherwise fall through
	 * to regular write.
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_WRITES ) {
	    status = dm_bypass_write ( fdx->bp, buffer_vp, nbytes );
	    BROKEN_PIPE_CHECK ( status, fdx->fd );
	    return status;
	}
    }

    return write ( fd, buffer_vp, nbytes );
}

int dm_close ( int file_desc )
{
    int status, i;
    struct dm_fd_extension *fdx;
    
/* TRACE */
dmpipe_trace_output("dm_close()\r\n");
/* END TRACE */
    if (!CleanupDone)
       {
       fdx = find_extension ( file_desc, 0 );
       if ( fdx->fp ) {
	 /* Invalidate cache entry */
	 for ( i = 0; i < FP_TO_FD_TLB_SIZE; i++ ) {
	     if ( fdx->fp == dm_fp_map[i].fp ) dm_fp_map[i].fp = 0;
	     }
          }
       if ( fdx && fdx->initialized ) {	/* Rundown bypass */
	 fdx->initialized = 0;
	 if ( fdx->bypass_flags && fdx->bp ) {
	    dm_bypass_shutdown ( fdx->bp );
	    fdx->bp = 0;
	    }
          }
       }
    
    return close ( file_desc );
}

ssize_t dm_feof (FILE *fptr)
{
    int status;
    size_t result=0;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_feof()\r\n");
/* END TRACE */

    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->read_ops == 0 ) {
	    /* First time reading, stall reader to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
	}

	/*
	 * Switch for bypass if stream established, otherwise fall through
	 * to regular feof().
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_READS ) {
	    result = is_dm_bypass_peer_done( fdx->bp );
	    
	    BROKEN_PIPE_CHECK ( result, fdx->fd );
	    return result;
	}
    }
    result = feof ( fptr );
    return result;
}

int dm_open ( const char *file_spec, int flags, ... )
{
    int status, count, fd, i;
    struct dm_fd_extension *fdx;
    void *arg3_vp;
    va_list ap;
/* TRACE */
dmpipe_trace_output("dm_open()\r\n");
/* END TRACE */
    /*
     * Support standard open arguments and DEC extension.  VMS doesn't
     * map first page of address space, so value < 512 is assumed to be
     * a bit mask (mode).
     */
    va_count(count);	/* argument count */
    va_start(ap,flags);
    arg3_vp = va_arg(ap, void *);

    if ( ((unsigned int) arg3_vp) < 512 ) {
	fd = open ( file_spec, flags, (mode_t) arg3_vp );
    } else {
	/*
	 * Copy the argument list.
	 */
	union arglist{ void *vp; unsigned long ul; } *args;
	args = calloc ( count+1, sizeof(union arglist) );
	args[0].ul = count;
	args[1].vp = (void *) file_spec;
	args[2].ul = flags;
	args[3].vp = arg3_vp;
	for ( i = 4; i <= count; i++ ) args[i].vp = va_arg(ap, void *);

	fd = LIB$CALLG ( args, open );
	free ( args );
    }
    va_end(ap);

    if ( fd < 0 ) return fd;

    fdx = find_extension ( fd, 1 );

    return fd;
}

int dm_dup ( int file_desc )
{
    int fd;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_dup()\r\n");
/* END TRACE */
    fd = dup ( file_desc );

    if ( fd < 0 ) return fd;

    fdx = find_extension ( fd, 1 );
    if ( fdx && !fdx->initialized ) {
	init_extension ( fdx, fd, 0 );
    }
    return fd;
}

int dm_dup2 ( int file_desc1, int file_desc2 )
{
    int fd;
    struct dm_fd_extension *fdx2;
/* TRACE */
dmpipe_trace_output("dm_dup2()\r\n");
/* END TRACE */
    /*
     * Locate extension for fd that will be closed.
     */
    fdx2 = find_extension ( file_desc2, 0 );
    if ( fdx2 && fdx2->initialized ) {
	/* Rundown currently initialized fdx */
	rundown_extension ( fdx2 );
    }

    fd = dup2 ( file_desc1, file_desc2 );

    if ( fd < 0 ) return fd;

    fdx2 = find_extension ( fd, 1 );
    if ( fdx2 && !fdx2->initialized ) {
	init_extension ( fdx2, fd, 0 );
    }

    return fd;
}

FILE *dm_fdopen ( int file_desc, const char *a_mode )
{
    int status;
    struct dm_fd_extension *fdx;
    FILE *fp;

/* TRACE */
dmpipe_trace_output("dm_fdopen()\r\n");
/* END TRACE */
    fp = fdopen ( file_desc, a_mode );
    
    if ( fp ) {
        fdx = find_extension ( file_desc, 1 );
	fdx->fp = fp;		/* associate fp with file_desc */
    }
    
    return fp;
}

FILE *dm_popen ( const char *command, const char *type )
{
    int status;
    struct dm_fd_extension *fdx;
    FILE *fp;
/* TRACE */
dmpipe_trace_output("dm_popen()\r\n");
/* END TRACE */
    /*
     * Convey this process's stderr device to the child.
     */
    dm_bypass_stderr_propagate ( );

    fp = popen ( command, type );
    if ( fp ) {
        fdx = find_fp_extension ( fp, 0 );
	if ( fdx && !fdx->initialized ) {
	    /*
	     * Init extension and set hints flag if we were open for read.
	     */
	    init_extension ( fdx, fileno(fp), fp );
	    if ((fdx->bypass_flags&DM_BYPASS_HINT_STARTING) && (*type == 'r'))
		fdx->bypass_flags |= DM_BYPASS_HINT_POPEN_R;
	}
    }
    return fp;
}
FILE *dm_fopen ( const char *file_spec, const char *a_mode, ... )
{
    int status, count, i;
    struct dm_fd_extension *fdx;
    FILE *fp;
    va_list ap;
/* TRACE */
dmpipe_trace_output("dm_fopen()\r\n");
/* END TRACE */
    /*
     * Support standard open arguments and DEC extension.  VMS doesn't
     * map first page of address space, so value < 512 is assumed to be
     * a bit mask (mode).
     */
    va_count(count);	/* argument count */

    if ( count < 3 ) {
	fp = fopen ( file_spec, a_mode );
    } else {
	/*
	 * Copy the argument list.
	 */
	union arglist{ void *vp; unsigned long ul; } *args;

	args = calloc ( count+1, sizeof(union arglist) );
	va_start(ap,a_mode);
	args[0].ul = count;
	args[1].vp = (void *) file_spec;
	args[2].vp = (void *) a_mode;
	for ( i = 3; i <= count; i++ ) args[i].vp = va_arg(ap, void *);

	fp = (FILE *) LIB$CALLG ( args, fopen );
	free ( args );
        va_end(ap);
    }
    /*
     * Check for extension.
     */
    if ( fp ) {
        fdx = find_fp_extension ( fp, 0 );
	if ( fdx && !fdx->initialized ) {
	    init_extension ( fdx, fileno(fp), fp );
	}
    }
    return fp;
}

int dm_fputc(int ichar, FILE *fptr)
{
    int status;
    size_t result;
    struct dm_fd_extension *fdx;
    char *dbgbuffer;
    unsigned long dbgbuflen;

/* TRACE */
dmpipe_trace_output("dm_fputc()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->write_ops == 0 ) {
	    /* First time writing, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
	}
	fdx->write_ops++;
	/*
	 * Switch for bypass if stream established, otherwise fall through
	 * to regular fputs.
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_WRITES ) {
	    status = dm_bypass_write ( fdx->bp, (void *)&ichar, 1);
	    
	    BROKEN_PIPE_CHECK ( status, fdx->fd );
	    
	    return (((status != -1) ? ichar : EOF));
	}
    }
    result = fputc ( ichar, fptr );
    if (result < 0)
       {
       status = errno;
       dbgbuflen = (long)log10((float)result) + (long)log10((float)ichar) + (long)log10((float)status) + (long)(log10((float)((unsigned long)fptr))/log10(16.0)) + 4 + 54 ;
       dbgbuffer = (char *)calloc(1, dbgbuflen + 1);
       sprintf(dbgbuffer, "\r\ndm_fputc: returned %d, expected %d, errno = %d, fptr = %p\n", result, ichar, status, fptr);
       write(STDERR_FILENO, dbgbuffer,dbgbuflen);
       free((void *)dbgbuffer);
       }
	    
    return result;
}

int dm_puts(const char *str)
{
    int status, status2;
    unsigned long slen=strlen(str);
    size_t result;
    struct dm_fd_extension *fdx;
    char *dbgbuffer;
    unsigned long dbgbuflen;
    static const char new_line = '\n';

/* TRACE */
dmpipe_trace_output("dm_puts()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( stdout, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->write_ops == 0 ) {
	    /* First time writing, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
	}
	fdx->write_ops++;
	/*
	 * Switch for bypass if stream established, otherwise fall through
	 * to regular fputs.
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_WRITES ) {
	    status = dm_bypass_write ( fdx->bp, (void *)str, slen );
	    if (dm_bypass_write ( fdx->bp, (void *)&new_line, 1 ) == -1) status = -1;
	    
	    BROKEN_PIPE_CHECK ( status, fdx->fd );
	    
	    return ((status != -1) ? status : EOF);
	}
    }
    result = puts ( str );
    if (result < 0)
       {
       status = errno;
       dbgbuflen = (long)log10((float)result) + (long)log10((float)slen) + (long)log10((float)status) + (long)(log10((float)((unsigned long)stdout))/log10(16.0)) + 4 + 54 ;
       dbgbuffer = (char *)calloc(1, dbgbuflen + 1);
       sprintf(dbgbuffer, "\r\ndm_puts: returned %d, expected %d, errno = %d, fptr = %p\n", result, slen, status, stdout);
       write(STDERR_FILENO, dbgbuffer,dbgbuflen);
       free((void *)dbgbuffer);
       }
	    
    return result;
}


int dm_fputs(const char *str, FILE *fptr)
{
    int status;
    unsigned long slen=strlen(str);
    size_t result;
    struct dm_fd_extension *fdx;
    char *dbgbuffer;
    unsigned long dbgbuflen;

/* TRACE */
dmpipe_trace_output("dm_fputs()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->write_ops == 0 ) {
	    /* First time writing, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
	}
	fdx->write_ops++;
	/*
	 * Switch for bypass if stream established, otherwise fall through
	 * to regular fputs.
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_WRITES ) {
	    status = dm_bypass_write ( fdx->bp, (void *)str, slen );
	    
	    BROKEN_PIPE_CHECK ( status, fdx->fd );
	    
	    return (status);
	}
    }
    result = fputs ( str, fptr );
    if (result < 0)
       {
       status = errno;
       dbgbuflen = (long)log10((float)result) + (long)log10((float)slen) + (long)log10((float)status) + (long)(log10((float)((unsigned long)fptr))/log10(16.0)) + 4 + 54 ;
       dbgbuffer = (char *)calloc(1, dbgbuflen + 1);
       sprintf(dbgbuffer, "\r\ndm_fputs: returned %d, expected %d, errno = %d, fptr = %p\n", result, slen, status, fptr);
       write(STDERR_FILENO, dbgbuffer,dbgbuflen);
       free((void *)dbgbuffer);
       }
	    
    return result;
}

size_t dm_fread ( void *ptr, size_t itmsize, size_t nitems, FILE *fptr )
{
    int status;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_fread()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->read_ops == 0 ) {
	    /* First time reading, stall for writer to give peer a chance
	     * to negotiate bypass */
	    status = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "r", fdx->fcntl_flags );
#ifdef DEBUG
fprintf(tty, "/dmpipe/ fd[%d] fread startup stall flags: %d -> %d\n", 
fdx->fd, fdx->bypass_flags, status);
#endif
	    fdx->bypass_flags = status;
	}
	fdx->read_ops++;
	if ( fdx->bypass_flags & DM_BYPASS_HINT_READS ) {
	    /*
	     * Bypass the CRTL and read via share memory pipe (or other means).
	     */
	    int count;
	    
	    count = dm_bypass_read ( fdx->bp, ptr, itmsize*nitems, itmsize,0 );
/* TRACE */
	    if (count == 0)
	       {
/*	       dmpipe_trace = 1; */
	       dmpipe_trace_output("dm_bypass_read returned 0!\r\n");
	       }
/* END TRACE */
	    if ( count < 0 )
	       {
/*	       dmpipe_trace = 1; */
	       return 0;
	       }
	    /*
	     * After first EOF (count==0), check for special POPEN_R case and
	     * repoen file on null device, deassiging the bidirectional
	     * channel on the mailbox.  This lets the bypass module detect
	     * when mailbox has no writers.  We can't do this before the EOF
	     * because freopen drains the mailbox before closing the channel.
	     */
	    if ((count == 0) && (fdx->bypass_flags&DM_BYPASS_HINT_POPEN_R)) {
		fdx->fp = freopen ( "_NL:", "r", fptr );
		fdx->bypass_flags ^= DM_BYPASS_HINT_POPEN_R;
	    }
	    
	    return count /itmsize;
	}
    }

    return fread ( ptr, itmsize, nitems, fptr );
}


size_t dm_fwrite ( const void *ptr, size_t itmsize, size_t nitems,
	FILE *fptr )
{
    int status;
    size_t result;
    struct dm_fd_extension *fdx;
    char *dbgbuffer;
    unsigned long dbgbuflen;

/* TRACE */
dmpipe_trace_output("dm_fwrite()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->write_ops == 0 ) {
	    /* First time writing, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
	}
	fdx->write_ops++;
	/*
	 * Switch fo bypass if stream established, otherwise fall through
	 * to regular write.
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_WRITES ) {
	    status = dm_bypass_write ( fdx->bp, ptr, itmsize*nitems );
	    
	    BROKEN_PIPE_CHECK ( status, fdx->fd );
	    
	    return (status/itmsize);
	}
    }
    result = fwrite ( ptr, itmsize, nitems, fptr );
    if (result != nitems)
       {
       status = errno;
       dbgbuflen = (long)log10((float)result) + (long)log10((float)nitems) + (long)log10((float)status) + (long)(log10((float)((unsigned long)fptr))/log10(16.0)) + 4 + 54 ;
       dbgbuffer = (char *)calloc(1, dbgbuflen + 1);
       sprintf(dbgbuffer, "\r\ndm_fwrite: returned %d, expected %d, errno = %d, fptr = %p\n", result, nitems, status, fptr);
       write(STDERR_FILENO, dbgbuffer,dbgbuflen);
       free((void *)dbgbuffer);
       }
	    
    return result;
}

static int load_inbuf ( struct dm_fd_extension *fdx, int needed )
{
    struct dm_inbuf *inbuf;
    int available, count, expedite_flag;
    /*
     * Create inbuf if first call.  Also set min_delay_control flag
     * to 1 (yes) or 2 (no) by checking environment variable.
     */
    if ( !fdx->inbuf ) fdx->inbuf = calloc ( sizeof(struct dm_inbuf), 1 );
    if ( !fdx->inbuf ) return -1;
    if ( inbuf_min_delay_control == 0 ) {
	char *envvar = getenv ( "DMPIPE_INBUF_MIN_DELAY" );
	inbuf_min_delay_control = 2;
	if ( envvar ) {
	    if ( isdigit ( *envvar ) ) {
		inbuf_min_delay_control  = atoi ( envvar );
		if ( inbuf_min_delay_control != 1 ) inbuf_min_delay_control =2;
	    } else if ( strncasecmp ( envvar, "E", 1 ) == 0 ) {
		inbuf_min_delay_control = 1;
	    } else if ( strncasecmp ( envvar, "T", 1 ) == 0 ) {
		inbuf_min_delay_control = 1;
	    }
	}
    }

    inbuf = fdx->inbuf;
    available = inbuf->length - inbuf->rpos;
    while ( available < needed ) {
	/* Read up to IOSIZE bytes to end of buffer, discard */
	if ( inbuf->rpos > DM_INBUF_LOOKBACK ) {
	    /* move last 16 chars to begining of buffer to allow for ungetc */
	    inbuf->length = available + DM_INBUF_LOOKBACK;
	   memmove(inbuf->buffer, &inbuf->buffer[inbuf->rpos-DM_INBUF_LOOKBACK],
		inbuf->length );
	    inbuf->rpos = DM_INBUF_LOOKBACK;
	}
	count = dm_bypass_read ( fdx->bp, &inbuf->buffer[inbuf->length],
		DM_INBUF_BUFSIZE-inbuf->length, (inbuf_min_delay_control==1) ?
		needed : (DM_INBUF_BUFSIZE-inbuf->length), &expedite_flag );

	if ( count < 0 ) return -1;
	if ((count == 0) && (fdx->bypass_flags&DM_BYPASS_HINT_POPEN_R)) {
	    /*
	     * See dm_fread for explanation of freopen().
	     */
	    fdx->fp = freopen ( "_NL:", "r", fdx->fp );
	    fdx->bypass_flags ^= DM_BYPASS_HINT_POPEN_R;
	}
	inbuf->length += count;
        available = inbuf->length - inbuf->rpos;
	/*
	 * Cut short attempt to fill buffer if peer set expedite flag.
	 */
	if ( expedite_flag ) break;
    }
    return 0;
}


char *dm_fgets ( char *str, int maxchar, FILE *fptr )
{
    int status, count, remaining, segsize, i, found_newline;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_fgets()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->read_ops == 0 ) {
	    /* First time reading, stall for writer to give peer a chance
	     * to negotiate bypass */
	    i = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "r", fdx->fcntl_flags );
#ifdef DEBUG
fprintf(tty, "/dmpipe/ fd[%d] fgets startup stall result flags: %d->%d\n", 
fdx->fd, fdx->bypass_flags, i );
#endif
	    fdx->bypass_flags = i;
	}
	fdx->read_ops++;
	if ( fdx->bypass_flags & DM_BYPASS_HINT_READS ) {
	    /*
	     * Loop until caller's buffer filled.
	     */
	    char *out;
	    struct dm_inbuf *inbuf;
	    found_newline = 0;
	    out = str;
	    for ( remaining = maxchar-1; remaining > 0; remaining -= segsize) {
		/*
		 * Scan inbuf->buffer[rpos..length] for newline and set
		 * segsize.
		 */
	        if ( 0 != load_inbuf ( fdx, 1 ) ) return 0;
		inbuf = fdx->inbuf;
		segsize = inbuf->length - inbuf->rpos;
		if ( segsize > remaining ) segsize = remaining;
		for ( i = 0; i < segsize; i++ ) {
		    if ( inbuf->buffer[inbuf->rpos+i] == '\n' ) {
			found_newline = 1;
			segsize = i+1;
			break;
		    }
		}
		memcpy ( out, &inbuf->buffer[inbuf->rpos], segsize );
		inbuf->rpos += segsize;
		out += segsize;
		if ( found_newline ) break;
	    }
	    *out = '\0';
	    return str;
	}
    }
    return fgets ( str, maxchar, fptr );
}

int dm_ungetc ( int c, FILE *fptr )
{
    int status, count;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_ungetc()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->read_ops == 0 ) {
	    /* First time reading, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "r", fdx->fcntl_flags );
	}
	fdx->read_ops++;
	if ( fdx->bypass_flags & DM_BYPASS_HINT_READS ) {
	    unsigned char *ucp;
	    if ( 0 == load_inbuf ( fdx, 0 ) ) {
		if ( fdx->inbuf->rpos > 0 ) {
		    --fdx->inbuf->rpos;
	            ucp = (unsigned char *)&fdx->inbuf->buffer[fdx->inbuf->rpos];
		    *ucp = c;
		} else return EOF;
	    } else return -1;   /* callers set errno? */
	}
    }
    return ungetc ( c, fptr );
}

int dm_fgetc ( FILE *fptr )
{
    int status, count;
    struct dm_fd_extension *fdx;
    int ucp;

/* TRACE */
dmpipe_trace_output("dm_fgetc()\r\n");
/* END TRACE */
    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->read_ops == 0 ) {
	    /* First time reading, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "r", fdx->fcntl_flags );
	}
	fdx->read_ops++;
	if ( fdx->bypass_flags & DM_BYPASS_HINT_READS ) {
	    
	    if ( 0 == load_inbuf ( fdx, 1 ) ) {
	        ucp = (unsigned char) fdx->inbuf->buffer[fdx->inbuf->rpos];
		fdx->inbuf->rpos++;	    
		return ucp;
	    } else return -1;   /* callers set errno? */
	}
    }

    return fgetc ( fptr );
}

int dm_fclose ( FILE *fptr )
{
    int status;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_fclose()\r\n");
/* END TRACE */
    if (!CleanupDone)
       {
       fdx = find_fp_extension ( fptr, 0 );
       if ( fdx && fdx->initialized ) {
	 rundown_extension ( fdx );
          }
       }
    return fclose ( fptr );
}
int dm_pclose ( FILE *fptr )
{
    int status;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_pclose()\r\n");
/* END TRACE */
    if (!CleanupDone)
       {
       fdx = find_fp_extension ( fptr, 0 );
       if ( fdx && fdx->initialized ) {
	 rundown_extension ( fdx );
          }
       }
    return pclose ( fptr );
}
/*
 * dmpipe version of isapipe includes additional information abount
 * status of pipe bypass.  Second argument is optional, specify null
 * pointer to omit.
 */
int dm_isapipe ( int fd, int *bypass_status )
{
    int status;
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_isapipe()\r\n");
/* END TRACE */
    fdx = find_extension ( fd, 1 );
    if ( fdx && fdx->initialized && fdx->bp ) {
	/*
	 * Bypass structure was allocated, so we are a pipe and capable of 
	 * using shared memory bypass.  Return flags if argumnent supplied
	 * by caller.
         */
	if ( bypass_status ) {
	    *bypass_status = fdx->bypass_flags;
	    if ( fdx->bypass_flags & (DM_BYPASS_HINT_STARTING|
		DM_BYPASS_HINT_READS|DM_BYPASS_HINT_WRITES) ) return 2;
	}
	return 1;
    }
    /*
     * Invalid fd or device not a mailbox, let CRTL handle it.
     */
    if ( bypass_status ) *bypass_status = 0;
    return isapipe ( fd );
}
/*
 * Return I/O counts so applications can verify bypass is working.
 */
int dm_get_statistics ( int fd, struct dm_bypass_statistics *blk )
{
    struct dm_fd_extension *fdx;

    fdx = find_extension ( fd, 0 );

    memset ( blk, 0, sizeof (struct dm_bypass_statistics) );
    if ( !fdx || !fdx->initialized ) return -1;

    blk->valid_flags = fdx->bypass_flags/2;
    blk->write_ops = fdx->write_ops;
    blk->read_ops = fdx->read_ops;

    return blk->valid_flags;
}

static int unistd_partial_cb ( void *fdx_vp, char *buffer, int length )
{
    struct dm_fd_extension *fdx;
    int count;

    fdx = fdx_vp;
    if ( fdx->initialized ) {
	if ( fdx->write_ops == 0 ) {
	    /* First time writing, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
#ifdef DEBUG
if ( !tty ) tty = fopen ( "DBG$OUTPUT:", "a", "shr=put" );
fprintf (tty, "/dmpipe/ FD[%d] partial(,,%d) startup set bypass_flags: %d\n", 
		 fdx->fd, length, fdx->bypass_flags );
#endif
	}
	fdx->write_ops++;
	/*
	 * Switch fo bypass if stream established, otherwise fall through
	 * to regular write.
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_WRITES ) {
	    count = dm_bypass_write ( fdx->bp, buffer, length );
	    BROKEN_PIPE_CHECK ( count, fdx->fd );
	    return count;
	}
    }
    count = write ( fdx->fd, buffer, length );

    return count;
}
/*
 * generic function for processing printf.  Different engines are used
 * for the different floating point formats the compiler can use.
 */
#ifndef USE_SYSTEM_DOPRINT
#include "doprint.h"
#endif
#ifdef DOPRINT_H
#define GX_DOPRINT doprint_engine, &doprnt_gx_float_formatters
#define DX_DOPRINT doprint_engine, &doprnt_dx_float_formatters
#define TX_DOPRINT  doprint_engine, &doprnt_tx_float_formatters
#define G_DOPRINT  doprint_engine, &doprnt_g_float_formatters
#define D_DOPRINT doprint_engine, &doprnt_d_float_formatters
#define T_DOPRINT doprint_engine, &doprnt_t_float_formatters
#else
int decc$$gxdoprint(), decc$$dxdoprint(), decc$$txdoprint();
int decc$$gdoprint(), decc$$ddoprint(), decc$$tdoprint();
#define GX_DOPRINT decc$$gxdoprint
#define DX_DOPRINT decc$$dxdoprint
#define TX_DOPRINT decc$$txdoprint
#define G_DOPRINT decc$$gdoprint
#define D_DOPRINT decc$$ddoprint
#define T_DOPRINT decc$$tdoprint
#endif

static int vxfprintf ( FILE *fptr, const char *format, va_list ap,
	int (doprint_engine) ( char *, const char *, va_list, size_t,
	void *, int (*callback)(), int *
#ifdef DOPRINT_H
	, doprnt_float_formatters ), doprnt_float_formatters flt_vec )
#define FLT_VEC_ARG , flt_vec
#else
						) )
#define FLT_VEC_ARG
#endif
{
    struct dm_fd_extension *fdx;
    int status, status2, bytes_left;
    char *buffer=(char *)malloc(16385);
/* TRACE */
dmpipe_trace_output("vxfprintf()\r\n");
/* END TRACE */
    /*
     * See if file pointer has extended attributes.
     */
    fdx = find_fp_extension ( fptr, 1 );
    if (buffer != NULL) {
      if ( fdx && fdx->initialized && 
	  (fdx->bypass_flags&(DM_BYPASS_HINT_WRITES|DM_BYPASS_HINT_STARTING)) ) {
	  /*
	   * Give engine special output routine than can handle bypass.
           */
	  status = doprint_engine ( buffer, format, ap, 16385,
		  fdx, unistd_partial_cb, &bytes_left FLT_VEC_ARG );

          if ( (status >= 0) && bytes_left > 0 ) {
	      /* Flush buffer */
	      status2 = unistd_partial_cb ( fdx, buffer, bytes_left );
	      if ( status2 > 0 ) status += status2;
	      else if ( status2 < 0 ) status = status2;  /* error */
	  }
	  BROKEN_PIPE_CHECK ( status, fdx->fd );
      } else {
	  /*
	   * Not bypassing, use routine that goes directly to CRTL.
           */
	  status = doprint_engine ( buffer, format, ap, 16385,
		  fdx, unistd_partial_cb, &bytes_left FLT_VEC_ARG );

          if ( (status >= 0) && bytes_left > 0 ) {
	      /* Flush buffer */
	      status2 = unistd_partial_cb ( fdx, buffer, bytes_left );
	      if ( status2 > 0 ) status += status2;
	  }
      }
    free(buffer);
    } else {
      status = -1;
      errno = ENOMEM;
    }
    return status;
}
/*
 * Printf functions interpet floats multiple ways, so there are 6
 * different entry points to handle the __XFLOAT + __G_FLOAT, __D_FLOAT,
 * __IEEE_FLOAT combinations.  Dmpipe.h defines fprintf and printf to
 * different wrapper functions based upon compile-time macros.  Implement
 * the wrappers with a common function.
 */

int dm_fprintf_gx ( FILE *fptr, const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( fptr, format_spec, ap, GX_DOPRINT );
    va_end(ap);
    return status;
}
int dm_printf_gx ( const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( stdout, format_spec, ap, GX_DOPRINT );
    va_end(ap);
    return status;
}
/** D-X floating **/
int dm_fprintf_dx ( FILE *fptr, const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( fptr, format_spec, ap, DX_DOPRINT );
    va_end(ap);
    return status;
}
int dm_printf_dx ( const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( stdout, format_spec, ap, DX_DOPRINT );
    va_end(ap);
    return status;
}
/** IEEE-X floating **/
int dm_fprintf_tx ( FILE *fptr, const char *format_spec, ... )
{
    int status;
    va_list ap;

/* TRACE */
dmpipe_trace_output("dm_fprintf_tx()\r\n");
/* END TRACE */
    va_start(ap,format_spec);
    status = vxfprintf ( fptr, format_spec, ap, TX_DOPRINT );
    va_end(ap);
    return status;
}
int dm_printf_tx ( const char *format_spec, ... )
{
    int status;
    va_list ap;

/* TRACE */
dmpipe_trace_output("dm_printf_tx()\r\n");
/* END TRACE */
    va_start(ap,format_spec);
    status = vxfprintf ( stdout, format_spec, ap, TX_DOPRINT );
    va_end(ap);
    return status;
}
/**  G floating **/
int dm_fprintf_g ( FILE *fptr, const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( fptr, format_spec, ap, G_DOPRINT );
    va_end(ap);
    return status;
}
int dm_printf_g ( const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( stdout, format_spec, ap, G_DOPRINT );
    va_end(ap);
    return status;
}
/** D floating **/
int dm_fprintf_d ( FILE *fptr, const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( fptr, format_spec, ap, D_DOPRINT );
    va_end(ap);
    return status;
}
int dm_printf_d ( const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( stdout, format_spec, ap, D_DOPRINT );
    va_end(ap);
    return status;
}
/** IEEE floating **/
int dm_fprintf_t ( FILE *fptr, const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( fptr, format_spec, ap, T_DOPRINT );
    va_end(ap);
    return status;
}
int dm_printf_t ( const char *format_spec, ... )
{
    int status;
    va_list ap;

    va_start(ap,format_spec);
    status = vxfprintf ( stdout, format_spec, ap, T_DOPRINT );
    va_end(ap);
    return status;
}
/**********************************************************************/
/* Functions for poll/select interception, also requires fcntl to set
 * streams non-blocking.
 */
static int fcntl_hack ( int fd, int cmd, va_list ap )
{
    int x11_fd;
    struct dm_fd_extension *fdx;
    /*
     * Special hacks for magic files:
     *   F_SETLKW+100:  Open new file on NL: that associates X11 event flags
     *                  named by fd argument.
     */
    if ( (cmd == (F_SETLKW+100)) && (fd >= 0) && (fd < 128) ) {
	x11_fd = open ( "NL:", O_RDONLY, 0660 );
	if ( x11_fd < 0 ) return -1;
	fdx = find_extension ( fd, 1 );
	if ( !fdx ) return -1;

	fdx->fcntl_flags = (fd << 24);

	return x11_fd;
    } else {
       errno = EINVAL;
       return -1;
    }
}

int dm_fcntl ( int fd, int cmd, ... )
{
    va_list ap;
    struct dm_fd_extension *fdx;
    int i_arg, prev_flags, status, old_rattr, old_wattr, new_attr;
    struct flock *lk_arg; 
/* TRACE */
dmpipe_trace_output("dm_fcntl()\r\n");
/* END TRACE */
    /*
     * Check bypass status.
     */
    va_start(ap, cmd);

    if ( cmd >= (F_SETLKW+100) ) {
	status = fcntl_hack ( fd, cmd, ap );
	va_end(ap);
	return status;
    }

    fdx = find_extension ( fd, 1 );
    if ( fdx && fdx->initialized &&
	(fdx->bypass_flags&(DM_BYPASS_HINT_WRITES|DM_BYPASS_HINT_STARTING)) ) {
	memstream rstream, wstream;

	dm_bypass_current_streams ( fdx->bp, &rstream, &wstream );
	/*
	 * Set non-blocking.
	 */
	switch ( cmd ) {
	  case F_DUPFD:
	    i_arg = va_arg(ap,int); va_end(ap);
	    return dm_dup ( i_arg );

	  case F_GETFD:
	  case F_SETFD:
	    /* Let CRTL handle it */
	    break;

	  case F_GETFL:
	    /*  Return our own copy */
	    va_end(ap);
	    return fdx->fcntl_flags;

	  case F_SETFL:
	    i_arg = va_arg(ap,int); va_end(ap);
	    prev_flags = fdx->fcntl_flags;
	    fdx->fcntl_flags = i_arg;
	    /*
	     * map fcntl flags to memstream attributes.
	     */
	    new_attr = old_rattr = old_wattr = 0;
	    if ( i_arg & O_NONBLOCK ) new_attr = MEMSTREAM_ATTR_NONBLOCK;
	    if ( rstream ) {
		status = memstream_control (rstream, &new_attr, &old_rattr );
	    }
	    if ( wstream ) {
		status = memstream_control (wstream, &new_attr, &old_wattr );
	    }
#ifdef DEBUG
if ( tty ) fprintf(tty, "/dmpipe/ fcntl setfl %x, oldr: %x=%x, w: %x=%x sts: %d\n",
new_attr, rstream, old_rattr, wstream, old_wattr, status );
#endif
	    return prev_flags;

	  case F_GETLK:
	  case F_SETLK:
	  case F_SETLKW:
	  default:
	    break;
	}
    }
    /*
     * Pass through, argument list varies.
     */
    switch ( cmd ) {
	case F_DUPFD:
	case F_SETFD:
	case F_SETFL:
	    i_arg = va_arg ( ap, int ); va_end ( ap );
	    i_arg =  fcntl ( fd, cmd, i_arg );
	    return i_arg;

	case F_GETFD:
	case F_GETFL:
	    va_end ( ap );
	    return fcntl ( fd, cmd );

	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
	    lk_arg = va_arg(ap,struct flock *); va_end(ap);
	    return fcntl ( fd, cmd, lk_arg );

	default:
	    break;
    }
    errno = EINVAL;
    return -1;
}

int dm_poll ( struct pollfd filedes[], nfds_t nfds, int timeout )
{
    int i, status;
    struct dm_poll_group group;
    struct dm_fd_extension *fdx;
/* TRACE */
dmpipe_trace_output("dm_poll()\r\n");
/* END TRACE */
    /*
     * Setup, create group and add every FD in filedes list to it.
     */
    status = dm_poll_group_begin ( &group, timeout, POLL_TYPE_POLL );
    for ( i = 0; i < nfds; i++ ) {
        /*
         * See if file pointer has extended attributes.
         */
	if ( filedes[i].fd < 0 ) continue;		/* ignore invalid FDs*/
	filedes[i].revents = 0;
        fdx = find_extension ( filedes[i].fd, 1 );
	if ( fdx ) {
	    /*
	     * Ensure bypass initialized since we may never have done I/O yet.
	     * There are 2 streams, only initialize if events requests
	     * the appropriate status.
	     */
	     if ( fdx->bypass_flags &&
			(filedes[i].events & POLLIN) && (fdx->read_ops==0) ) {
		fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
			fdx->bp, "r", fdx->fcntl_flags );
		fdx->read_ops++;	/* only count once! */
	     }
	     if ( fdx->bypass_flags &&
			(filedes[i].events & POLLOUT) && (fdx->write_ops==0) ) {
		fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
			fdx->bp, "w", fdx->fcntl_flags );
		fdx->write_ops++;	/* only count once! */
	     }
	    /*
	     * Add poll extension structure, creating on first call.  If
	     * successful, add link to the caller's filedes entry.
	     */
	    if ( !fdx->poll_ext ) 
		fdx->poll_ext = dm_poll_create_track
			( filedes[i].fd, fdx->bp, fdx->fcntl_flags );

	    if ( fdx->poll_ext ) {
		dm_poll_group_add_pollfd (&group, &filedes[i], fdx->poll_ext);
	    } else {
		/* failed to create track */
		status = -1;
		break;
	    }
	} else {
	    break;
	}
    }
    /*
     * If all in group are sockets, just hand off to CRTL.
     */
    if ( group.total_fds == group.histogram[DMPIPE_POLL_DEV_SOCKET] ) {
	return poll ( filedes, nfds, timeout );
    }
    /*
     * Now scan group, waiting for event or timeout.
     */
    if ( status >= 0 ) status = dm_poll_scan_group ( &group );
    /*
     * Cleanup.
     */
    dm_poll_group_end ( &group );
    return status;
}

int dm_select ( int nfds, fd_set *readfds, fd_set *writefds,
	fd_set *exceptfds, struct timeval *timeout )
{
    struct dm_fd_extension *fdx;
    int timeout_ms, fd, summary_mask, status, count;
    struct dm_poll_extension *member;
    struct dm_poll_group group;
/* TRACE */
dmpipe_trace_output("dm_select()\r\n");
/* END TRACE */
    /*
     * Convert timeout to millseconds.
     */
    timeout_ms = (timeout->tv_usec/1000) + (timeout->tv_sec*1000);
    status = dm_poll_group_begin ( &group, timeout_ms, POLL_TYPE_SELECT );
    if ( status < 0 ) return status;
    /*
     * Check each bit in each fd_set and setup.  Consolidate accross the
     * fd_set structures to make a single 3-bit mask for each fd.
     */
    for ( fd = 0; fd < nfds; fd++ ) {
	/*
	 * See if we are interested in this fd.
	 */
	summary_mask = 0;
	if ( readfds ) if ( FD_ISSET(fd,readfds) ) 
	    { summary_mask |= DM_POLL_SELECT_READ; FD_CLR(fd,readfds); }
	if ( writefds ) if ( FD_ISSET(fd,writefds)) 
	    { summary_mask |= DM_POLL_SELECT_WRITE; FD_CLR(fd,writefds); }
	if ( exceptfds ) if ( FD_ISSET(fd,exceptfds) ) 
	    { summary_mask |= DM_POLL_SELECT_EXCEPT; FD_CLR(fd,exceptfds); }

	if ( summary_mask == 0 ) continue;	/* nope, not interested */
        /*
	 * Add to group.
	 */
        fdx = find_extension ( fd, 1 );
	if ( fdx ) {
	    /* Ensure initialized since we may never have done I/O yet */
	     if ( fdx->bypass_flags &&
			(summary_mask&DM_POLL_SELECT_READ) && (fdx->read_ops==0) ) {
		fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
			fdx->bp, "r", fdx->fcntl_flags );
		fdx->read_ops++;	/* only count once! */
	     }
	     if ( fdx->bypass_flags &&
			(summary_mask&DM_POLL_SELECT_WRITE) && (fdx->write_ops==0) ) {
		fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
			fdx->bp, "w", fdx->fcntl_flags );
		fdx->write_ops++;	/* only count once! */
	     }
	    /* Add poll structure, creating on first call. */
	    if ( !fdx->poll_ext ) 
		fdx->poll_ext = dm_poll_create_track
			( fd, fdx->bp, fdx->fcntl_flags );

	    if ( fdx->poll_ext ) {
		dm_poll_group_add_selectfd (&group, fd, summary_mask,
			fdx->poll_ext);
	    } else {
		/* failed to create track */
		status = -1;
		break;
	    }
	} else {
	    break;
	}
    }
    /*
     * Wait and when done, step through group members and convert the
     * event summaries back to fd_set bits.
     */
    status = dm_poll_scan_group ( &group );
    count = 0;
    for ( member = group.first_member; member; member=member->next_member ) {
	summary_mask = (member->select_event & member->select_mask);
	if (summary_mask == 0 ) continue;	/* nothing to report */
	fd = member->fd;
	count++;
	if ( summary_mask & DM_POLL_SELECT_READ ) FD_SET(fd,readfds);
	if ( summary_mask & DM_POLL_SELECT_WRITE ) FD_SET(fd,writefds);
	if ( summary_mask & DM_POLL_SELECT_EXCEPT ) FD_SET(fd,exceptfds);
    }
    /*
     * Cleanup.
     */
    dm_poll_group_end ( &group );
    
    return count;
}
/*
 * Scan functions.  Scan_input() support routine does basic read of input
 * stream, stopping at various break conditions (mitigating need for
 * unget()).  Return value is number of characters transfered to outbuf or
 * -1 on error.
 */
static int scan_input ( void *fdx_vp, 
	int scan_control,		/* bit mask of scan options */
	const char *matchset,		/* characters to match in token */
	char *outbuf,			/* output buffer */
	int bufsize,			/* Buffer size or -1 for unlimited */
	char **term_char )		/* points to delimiter char */
{
    struct dm_inbuf *inbuf;
    struct dm_fd_extension *fdx;
    int outlen, span;
    /*
     * Create inbuf if first call.
     */
    fdx = fdx_vp;
    if ( !fdx->inbuf ) fdx->inbuf = calloc ( sizeof(struct dm_inbuf), 1 );
    if ( !fdx->inbuf ) return -1;
    inbuf = fdx->inbuf;
    /*
     * If bit 0 of code set, skip whitespace in stream.
     */
    *term_char = 0;
    outlen = 0;
    if ( scan_control & DOSCAN_CTL_SKIP_WS ) {
	if ( load_inbuf ( fdx, 1 ) < 0 ) return -1;
	while ( isspace(inbuf->buffer[inbuf->rpos]) ) {
	    inbuf->rpos++;
	    if ( inbuf->rpos >= inbuf->length ) {
		if ( load_inbuf ( fdx, 1 ) < 0 ) return -1;
	    }
	}
    }
    /*
     * Bits <1> and <2> determine
     */
    if ( scan_control & DOSCAN_CTL_MATCH_TO_WS ) {
	/*
	 * Token characters are non whitespace chars.
	 */
	do {
	    /*
	     * Ensure at lease one characetr in stream buffer and
	     * count number of non-space chars starting at inbuf->rpos.
	     */
	    if ( load_inbuf ( fdx, 1 ) < 0 ) return -1;
	    inbuf->buffer[inbuf->length] = '\0';
	    span = strcspn ( &inbuf->buffer[inbuf->rpos], " \t\r\n" );
	    /*
	     * Add non-white chars we found to output buffer.
	     */
	    if ( span > 0 ) {
		if ( (bufsize!=-1) && (span > (bufsize-outlen)) ) {
		    span = bufsize - outlen;
		}
		memcpy (&outbuf[outlen], &inbuf->buffer[inbuf->rpos], span);
		inbuf->rpos += span;
		outlen += span;
		if ( inbuf->buffer[inbuf->rpos] ) {
		    *term_char = &inbuf->buffer[inbuf->rpos];
		    break;
		}
	    } else {
		/* character at rpos was whitespace. */
		*term_char = &inbuf->buffer[inbuf->rpos];
		break;
	    }
	} while ( (outlen < bufsize) || (bufsize==-1) );
    } else if ( !(scan_control&DOSCAN_CTL_INVERT) ) {
	/*
	 * Scan set argument defines characters comprising token.
	 */
	char c;
 	while ( (outlen < bufsize) || (bufsize==-1) ) {
	    if ( inbuf->rpos >= inbuf->length ) {
		if ( load_inbuf ( fdx, 1 ) < 0 ) {
		    if ( outlen > 0 ) return outlen;
		    return -1;
		}
	    }
	    c = inbuf->buffer[inbuf->rpos];
	    if ( !strchr ( matchset, c ) ) {
		/*
		 * Character not in match set, check for override.
		 */
		if ( scan_control & DOSCAN_CTL_MATCH_FIRST ) 
		    scan_control ^= DOSCAN_CTL_MATCH_FIRST;
		else {
		    *term_char = &inbuf->buffer[inbuf->rpos];
	             break;
		}
	    }
	    inbuf->rpos++;
	    outbuf[outlen++] = c;
	}
	
    } else {
	/*
	 * Matchset argument defines delimiter tokens.
	 */
	char c;
 	while ( (outlen < bufsize) || (bufsize==-1) ) {
	    if ( inbuf->rpos >= inbuf->length ) {
		if ( load_inbuf ( fdx, 1 ) < 0 ) {
		    if ( outlen > 0 ) return outlen;
		    return -1;
		}
	    }
	    c = inbuf->buffer[inbuf->rpos];
	    if ( strchr ( matchset, c ) ) {
		if ( scan_control & DOSCAN_CTL_MATCH_FIRST ) 
		    scan_control ^= DOSCAN_CTL_MATCH_FIRST;
		else {
		    *term_char = &inbuf->buffer[inbuf->rpos];
	             break;
		}
	    }
	    inbuf->rpos++;
	    outbuf[outlen++] = c;
	}
	
    }
    return outlen;
}
/*
 * Generalized vfscanf function that lets use call scanf with the
 * argument list pointing to various floating point formats.
 */
int dm_vfscanf_vec ( FILE *fptr, const char *format_spec, va_list ap,
	struct doscan_float_format_functions *flt_vec )
{
    int status, count, remaining, segsize, i, found_newline;
    struct dm_fd_extension *fdx;

    fdx = find_fp_extension ( fptr, 1 );
    if ( fdx && fdx->initialized ) {
	if ( fdx->read_ops == 0 ) {
	    /* First time reading, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "r", fdx->fcntl_flags );
	}
	fdx->read_ops++;
	if ( fdx->bypass_flags & DM_BYPASS_HINT_READS ) {
	    status = doscan_engine ( format_spec, ap, fdx, scan_input,
		flt_vec );
	    return status;
	}
    }
    /*
     * No bypass, pass through to CRTL.
     */
    return flt_vec->fallback ( fptr, format_spec, ap );
}
/*
 * format-specific scanf functions, which one the application calls
 * is determine when the application is compiled.
 */
int dm_fscanf_gx ( FILE *fptr, const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(fptr, format_spec, ap,&doscan_gx_float_formatters);
    va_end ( ap );
    return status;
}
int dm_scanf_gx ( const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(stdin, format_spec, ap,&doscan_gx_float_formatters);
    va_end ( ap );
    return status;
}
int dm_fscanf_dx ( FILE *fptr, const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(fptr, format_spec, ap,&doscan_dx_float_formatters);
    va_end ( ap );
    return status;
}
int dm_scanf_dx ( const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(stdin, format_spec, ap,&doscan_dx_float_formatters);
    va_end ( ap );
    return status;
}
int dm_fscanf_tx ( FILE *fptr, const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

/* TRACE */
dmpipe_trace_output("dm_fscanf_tx()\r\n");
/* END TRACE */
    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(fptr, format_spec, ap,&doscan_tx_float_formatters);
    va_end ( ap );
    return status;
}
int dm_scanf_tx ( const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

/* TRACE */
dmpipe_trace_output("dm_scanf_tx()\r\n");
/* END TRACE */
    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(stdin, format_spec, ap,&doscan_tx_float_formatters);
    va_end ( ap );
    return status;
}
int dm_fscanf_g ( FILE *fptr, const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(fptr, format_spec, ap,&doscan_g_float_formatters);
    va_end ( ap );
    return status;
}
int dm_scanf_g ( const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(stdin, format_spec, ap,&doscan_g_float_formatters);
    va_end ( ap );
    return status;
}
int dm_fscanf_d ( FILE *fptr, const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(fptr, format_spec, ap,&doscan_d_float_formatters);
    va_end ( ap );
    return status;
}
int dm_scanf_d ( const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(stdin, format_spec, ap,&doscan_d_float_formatters);
    va_end ( ap );
    return status;
}
int dm_fscanf_t ( FILE *fptr, const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(fptr, format_spec, ap,&doscan_t_float_formatters);
    va_end ( ap );
    return status;
}
int dm_scanf_t ( const char *format_spec, ... )
{
    int status, count, remaining, segsize, i, found_newline;
     va_list ap;

    va_start ( ap, format_spec );
    status = dm_vfscanf_vec(stdin, format_spec, ap,&doscan_t_float_formatters);
    va_end ( ap );
    return status;
}
/*
 * Perror.  Contstruct output line as 4 separate writes.
 */
void dm_perror ( const char *str )
{
    int status, fd, ecode, vmscode;
    char *errmsg;
    struct dm_fd_extension *fdx;
/* TRACE */
dmpipe_trace_output("dm_perror()\r\n");
/* END TRACE */
    /*
     * Extract error message before we make other calls that could change errno.
     */
    ecode = errno;
    errmsg = strerror ( ecode );
    /*
     * Direct message to stderr or pipe.
     */
    fd = 2;		/* stderr */
    fdx = find_extension ( fd, 1 );

    if ( fdx->bypass_flags ) {
	if ( fdx->write_ops == 0 ) {
	    /* First time writing, stall for writer to give peer a chance
	     * to negotiate bypass */
	    fdx->bypass_flags = dm_bypass_startup_stall (fdx->bypass_flags,
		fdx->bp, "w", fdx->fcntl_flags );
	}
	fdx->write_ops++;
	/*
	 * Switch to bypass if stream established, otherwise fall through
	 * to regular perror().
	 */
	if ( fdx->bypass_flags & DM_BYPASS_HINT_WRITES ) {

	    status = dm_bypass_write ( fdx->bp, str, strlen(str) );
	    if ( status >= 0 ) status = dm_bypass_write ( fdx->bp, ": ", 2 );
	    if ( status >= 0 ) status = dm_bypass_write ( fdx->bp, errmsg,
			strlen(errmsg) );
	    if ( status >= 0 ) status = dm_bypass_write ( fdx->bp, "\n", 1 );
	    BROKEN_PIPE_CHECK ( status, fdx->fd );
	    return;
	}
    }

    perror ( str );
    return;
}
/*
 * Flush functions.  Go around bypass layer to get the write stream and call
 * memstream_flush directly.  If no write stream set up, call CRTL function.
 */
static int flush_extension ( struct dm_fd_extension *fdx )
{
    memstream rstream, wstream;
    int status;

    dm_bypass_current_streams ( fdx->bp, &rstream, &wstream );
    if ( wstream ) {
	status = memstream_flush ( wstream );
	BROKEN_PIPE_CHECK ( status, fdx->fd );
    } else status = EOF;	/* stream not open for write */
    return status;
}

int dm_fsync ( int fd )
{
    struct dm_fd_extension *fdx;

/* TRACE */
dmpipe_trace_output("dm_fsync()\r\n");
/* END TRACE */
    fdx = find_extension ( fd, 0 );
    if ( fdx && fdx->initialized &&
	(fdx->bypass_flags&(DM_BYPASS_HINT_WRITES|DM_BYPASS_HINT_STARTING)) ) {

	return flush_extension ( fdx );
    }
    /*
     * Fallback to CRTL function.
     */
    return fsync ( fd );
}

int dm_fflush ( FILE *fptr )
{
    struct dm_fd_extension *fdx;
/* TRACE */
dmpipe_trace_output("dm_fflush()\r\n");
/* END TRACE */
    /*
     * Check for flush-all case of fptr null
     */
    if ( !fptr ) {
	int row, col;
	struct dm_fd_extension *extrow;
	/*
	 * Scan fdx table for initialized extensions and recursivelflush.
	 */
	for ( row = 0; row < FD_EXTENSION_MAP_ROWS; row++ ) {
	    /*
	     * Rows are dynamically allocated, skip if never created.
	     */
	    extrow = dm_fd_extrow[row];
	    if ( !extrow ) continue;
	    /*
	     * Scan row looking for active (initialized) extensions with fp 
	     * that we have written to at least once and flush.
	     */
	    for ( col = 0; col < FD_EXTENSION_MAP_COLS; col++ ) {
		fdx = &extrow[col];
		if ( fdx->initialized && fdx->fp && (fdx->write_ops>0) ) {
		    dm_fflush ( fdx->fp );
		}
	    }
	}
    }
    /*
     * Flush single stream.
     */
    fdx = find_fp_extension ( fptr, 0 );
    if ( fdx && fdx->initialized &&
	(fdx->bypass_flags&(DM_BYPASS_HINT_WRITES|DM_BYPASS_HINT_STARTING)) ) {

	return flush_extension ( fdx );
    }
    /*
     * Fallback to CRTL function.
     */
    return fflush ( fptr );
}
