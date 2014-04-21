#ifndef DM_PIPE_H
#define DM_PIPE_H
/*
 * Define functional replacements for CRTL I/O routines that bypass
 * pipe device and use shared memory instead.  The dual mode pipe retain
 * legacy mailbox operations unless both sides are using these functions.
 *
 * Conditional compilation macros:
 *    DM_NO_CRTL_WRAP		If defined, do not define macros to wrap
 *				CRTL functions.
 *    DM_WRAP_MAIN		if defined, insert a main function that
 *				wraps application main() in same module.
 */
#include <stdio.h>
#include <unistd.h>		/* pipe definitions, pipe(), close() */
#include <unixio.h>		/* isapipe() */
#include <fcntl.h>		/* open() */
#include <poll.h>		/* poll() and friends */
#include <socket.h>		/* select() was implemented by TCP/IP dev. */
#include <time.h>		/* Select() */

int dm_pipe ( int fds[2] );
ssize_t dm_read ( int fd, void *buffer_vp, size_t nbytes );
ssize_t dm_write ( int fd, const void *buffer_vp, size_t nbytes );
int dm_close ( int file_desc );
int dm_open ( const char *file_spec, int flats, ... );
int dm_dup ( int file_desc );
int dm_dup2 ( int file_desc1, int file_desc2 );

FILE *dm_fdopen ( int file_desc, char *a_mode );
FILE *dm_popen ( const char *command, const char *type );
FILE *dm_fopen ( const char *file_spec, const char *a_mode, ... );
size_t dm_fread ( void *ptr, size_t itmsize, size_t nitems, FILE *fptr );
size_t dm_fwrite ( const void *ptr, size_t itmsize, size_t nitems, FILE *fptr );
char *dm_fgets ( char *str, int maxchar, FILE *fptr );
int dm_pclose ( FILE *stream );
int dm_fgetc ( FILE *fptr );
int dm_ungetc ( int c, FILE *fptr );
int dm_fgettok ( FILE *fptr, char *buffer, int bufsize,
    int type, const char cset, int *term_state );

int dm_fclose ( FILE *fptr );
int dm_fcntl ( int fd, int cmd, ... );
int dm_select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
int dm_poll ( struct pollfd filedes[], nfds_t nfds, int timeout );
void dm_perror ( const char *str );
int dm_fflush ( FILE *fptr );
int dm_fsync ( int fd );
int dm_isapipe ( int fd, int *bypass_status );  /* note additional argument */
/*
 * Statistics retreival.
 */
struct dm_bypass_statistics {
    int valid_flags;		/* <0> reads <1> writes */
    long write_ops, read_ops;
};
int dm_get_statistics ( int fd, struct dm_bypass_statistics *blk );
/*
 * Printf functions interpet floats multiple ways.  Define fprintf and printf
 * macros to select the proper variant.
 */
int dm_fprintf_gx ( FILE *fptr, const char *format_spec, ... );
int dm_fprintf_dx ( FILE *fptr, const char *format_spec, ... );
int dm_fprintf_tx ( FILE *fptr, const char *format_spec, ... );
int dm_printf_gx ( const char *format_spec, ... );
int dm_printf_dx ( const char *format_spec, ... );
int dm_printf_tx ( const char *format_spec, ... );
int dm_fprintf_g ( FILE *fptr, const char *format_spec, ... );
int dm_fprintf_d ( FILE *fptr, const char *format_spec, ... );
int dm_fprintf_t ( FILE *fptr, const char *format_spec, ... );
int dm_printf_g ( const char *format_spec, ... );
int dm_printf_d ( const char *format_spec, ... );
int dm_printf_t ( const char *format_spec, ... );
int dm_fscanf_gx ( FILE *fptr, const char *fmt, ... );
int dm_scanf_gx ( const char *fmt, ... );
int dm_fscanf_dx ( FILE *fptr, const char *fmt, ... );
int dm_scanf_dx ( const char *fmt, ... );
int dm_fscanf_tx ( FILE *fptr, const char *fmt, ... );
int dm_scanf_tx ( const char *fmt, ... );
int dm_fscanf_g ( FILE *fptr, const char *fmt, ... );
int dm_scanf_g ( const char *fmt, ... );
int dm_fscanf_d ( FILE *fptr, const char *fmt, ... );
int dm_scanf_d ( const char *fmt, ... );
int dm_fscanf_t ( FILE *fptr, const char *fmt, ... );
int dm_scanf_t ( const char *fmt, ... );

#pragma assert_m func_attrs(dm_printf_gx,dm_printf_dx,dm_printf_tx) format(printf,1,2)
#pragma assert_m func_attrs(dm_fprintf_gx,dm_fprintf_dx,dm_fprintf_tx) format(printf,2,3)
#pragma assert_m func_attrs(dm_printf_g,dm_printf_d,dm_printf_t) format(printf,1,2)
#pragma assert_m func_attrs(dm_fprintf_g,dm_fprintf_d,dm_fprintf_t) format(printf,2,3)

#pragma assert_m func_attrs(dm_scanf_gx,dm_scanf_dx,dm_scanf_tx) format(scanf,1,2)
#pragma assert_m func_attrs(dm_scanf_g,dm_scanf_d,dm_scanf_t) format(scanf,1,2)
#pragma assert_m func_attrs(dm_fscanf_gx,dm_fscanf_dx,dm_fscanf_tx) format(scanf,2,3)
#pragma assert_m func_attrs(dm_fscanf_g,dm_fscanf_d,dm_fscanf_t) format(scanf,2,3)

#ifndef DM_NO_CRTL_WRAP
#if __X_FLOAT
# if __G_FLOAT
#   define fprintf dm_fprintf_gx		/* double type is G float format */
#   define printf dm_printf_gx		/* double type is G float format */
#   define fscanf dm_fscanf_gx
#   define scanf dm_scanf_gx
# elif __IEEE_FLOAT
#   define fprintf dm_fprintf_tx		/* double type is IEEE float format */
#   define printf dm_printf_tx		/* double type is IEEE float format */
#   define fscanf dm_fscanf_tx
#   define scanf dm_scanf_tx
# else
#   define fprintf dm_fprintf_dx		/* double type is D float format */
#   define printf dm_printf_dx		/* double type is D float format */
#   define fscanf dm_fscanf_dx
#   define scanf dm_scanf_dx
# endif
#else
# if __G_FLOAT
#   define fprintf dm_fprintf_g		/* double type is G float format */
#   define printf dm_printf_g		/* double type is G float format */
#   define fscanf dm_fscanf_g
#   define scanf dm_scanf_g
# elif __IEEE_FLOAT
#   define fprintf dm_fprintf_t		/* double type is IEEE float format */
#   define printf dm_printf_t		/* double type is IEEE float format */
#   define fscanf dm_fscanf_t
#   define scanf dm_scanf_t
# else
#   define fprintf dm_fprintf_d		/* double type is D float format */
#   define printf dm_printf_d		/* double type is D float format */
#   define fscanf dm_fscanf_d
#   define scanf dm_scanf_d
# endif
#endif

#define pipe dm_pipe
#define read dm_read
#define write dm_write
#define close dm_close
#define open dm_open

#define perror dm_perror
#define popen dm_popen
#define pclose dm_pclose
#define fopen dm_fopen
#define fclose dm_fclose
#define fdopen dm_fdopen
#define fread dm_fread
#define fwrite dm_fwrite
#define fgets dm_fgets
#define fgetc dm_fgetc
#define ungetc dm_ungetc
#define fcntl dm_fcntl
#define poll dm_poll
#define select dm_select
#define fflush dm_fflush
#define fsync dm_fsync
#define isapipe(fd) dm_isapipe((fd),0)
#endif /* DM_NO_CRTL_WRAP */

#ifdef DM_WRAP_MAIN
#include "dmpipe_main.c"	/* main application's main a wrapped function */
#endif /* DM_WRAP_MAIN */

#endif /* DM_PIPE_H */
