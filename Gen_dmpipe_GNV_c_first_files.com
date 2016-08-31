$!
$! Purpose:
$!	This command procedure is intended to generate gnv$XXXXX.c_first files used
$!	by the GNV toolkit compiler wrapper so that a project can be built to use
$!	the dmpipe I/O functions instead of the regular OpenVMS CRTL I/O functions.
$!	These gnv$XXXXX.c_first files setup the dmpipe wrapper functions for CRTL I/O
$!	functions that can be used with UNIX-like pipes. This procedure searches the
$!	current directory tree for *.c files that reference any CRTL I/O function that
$!	can be used with a pipe-based fd. If any such function reference is found, the
$!	procedure generates a corresponding gnv$XXXXX.c_first file which #include's
$!	the dmpipe.h header file which contains the macros which alter the normal CRTL
$!	I/O function name to its corresponding dmpipe name. In order to use this utility
$!	procedure, set the default directory to the top of the project tree that is to
$!	be built using the dmpipe routines and invoke this procedure:
$!
$!		$ SET DEF PRJ_ROOT:[project_path]
$!		$ @VMS_ROOT:[vms-ports-dmpipe]Gen_dmpipe_GNV_c_first_files.com
$!
$! Search for files containing a reference to any of the CRTL I/O functions. Place
$! the names of any such files into a data file.
$!
$ SEARCH/OUTPUT=GNV$dmpipe_files.dat/WINDOW=0 [...]*.c "pipe (","read (","write (","close (","open (","perror (","popen (","pclose (","fopen (","fclose (","feof (","fdopen (","printf (","fprintf (","fread (","fwrite (","fgets (","fgetc (","getc (","ungetc (","fcntl (","poll (","select (","fflush (","fsync (","isapipe (","dup (","dup2 (","fputs (","fputc (","getchar (","putc (","putchar (","puts (","fscanf (","scanf ("
$!
$! Open the data file and, for each file name, generate the corresponding gnv$XXXXX.c_first
$! file if it does not already exist. If it already exists, check to see if it already
$! #include's the dmpipe.h header file and, if not, append the #include "dmpipe.h" statement.
$!
$ OPEN/READ dmpipe_files GNV$dmpipe_files.dat
$ READ_FILENAME:
$    READ/END_OF_FILE=CLOSE_DMPIPE_FILES dmpipe_files gnv_file
$    gnv_cfirst_file = "VMS_ROOT:" + F$PARSE(gnv_file,,,"DIRECTORY","SYNTAX_ONLY") + "gnv$" + F$PARSE(gnv_file,,,"NAME","SYNTAX_ONLY") + ".c_first"
$    existing_cfirst_file = F$SEARCH(gnv_cfirst_file)
$    IF ("''existing_cfirstfile'" .NES. "")
$    THEN
$       SEARCH 'existing_cfirstfile' "#include ""config.h"""
$       SSTATUS1 = $STATUS
$       SEARCH 'existing_cfirstfile' "#include ""dmpipe.h"""
$       SSTATUS2 = $STATUS
$       IF ((SSTATUS1 .NE. 1) .AND. (SSTATUS2 .NE. 1))
$       THEN       
$          COPY SYS$INPUT+'existing_cfirst_file' 'gnv_cfirst_file'
#include "config.h"
#include "dmpipe.h"
$       ENDIF
$    ELSE
$       OPEN/WRITE cfirst_file 'gnv_cfirst_file'
$       WRITE cfirst_file "#include ""config.h"""
$       WRITE cfirst_file "#include ""dmpipe.h"""
$       CLOSE cfirst_file
$    ENDIF
$ GOTO READ_FILENAME
$!
$! Close the data file of file names with CRTL I/O function references.
$!
$ CLOSE_DMPIPE_FILES:
$ CLOSE dmpipe_files
$!
$! Delete the data file.
$!
$ DELETE GNV$dmpipe_files.dat;*