$!
$! Command procedure to link dmpipeshr.exe shareable image via embedded
$! linker options file.
$ link/DEBUG/MAP=dmpipeshr.MAP/DSF=dmpipeshr.DSF/share=dmpipe_exe:dmpipeshr.exe sys$input/option
$ deck
!
! Linker options file for creating dmpipe shareable image.  This image
! is compiled with a replacement implentation of decc$$doprint_xx functions
! so it will link in conjunction with decc$shr instead of starlet.olb
!
identification="DMPIPE/1.0"

GSMATCH=LEQUAL,1,0
!
! Object files implementing dmpipe wrapper functions.
!
dmpipe_lib:dmpipe_libinit.obj
dmpipe_lib:dmpipe-private_doprint.obj
dmpipe_bypass.obj
memstream.obj
dmpipe_poll.obj
doscan.obj
doscan_flt_gx.obj
doscan_flt_dx.obj
doscan_flt_tx.obj
doscan_flt_g.obj
doscan_flt_d.obj
doscan_flt_t.obj
!
! Object files for doprint internal fuction.  Floating point module compiled
! 6 different ways for all the combinations of d-float, g-float, IEEE float
! and x-float
!
dmpipe_lib:doprint.obj
doprint_flt_gx.obj
doprint_flt_g.obj
doprint_flt_tx.obj
doprint_flt_t.obj
doprint_flt_dx.obj
doprint_flt_d.obj
!
! Transfer vector.  Functions are generally CRTL I/O functions with
! a dm_ prefix.
!
CASE_SENSITIVE=YES
SYMBOL_VECTOR=(-
   DM_PIPE=PROCEDURE,-
   DM_READ=PROCEDURE,-
   DM_WRITE=PROCEDURE,-
   DM_OPEN=PROCEDURE,-
   DM_CLOSE=PROCEDURE,-
   DM_GET_STATISTICS=PROCEDURE,-
   DM_DUP=PROCEDURE,DM_DUP2=PROCEDURE,-
   DM_FDOPEN=PROCEDURE,-
   DM_POPEN=PROCEDURE,-
   DM_PCLOSE=PROCEDURE,-
   DM_FOPEN=PROCEDURE,-
   DM_FCLOSE=PROCEDURE,-
   DM_FREAD=PROCEDURE,-
   DM_FWRITE=PROCEDURE,-
   DM_FGETS=PROCEDURE,-
   DM_FGETC=PROCEDURE,-
   DM_UNGETC=PROCEDURE,-
!
   DM_FSCANF_GX=PROCEDURE,-
   DM_FSCANF_DX=PROCEDURE,-
   DM_FSCANF_TX=PROCEDURE,-
   DM_FSCANF_G=PROCEDURE,-
   DM_FSCANF_D=PROCEDURE,-
   DM_FSCANF_T=PROCEDURE,-
   DM_SCANF_GX=PROCEDURE,-
   DM_SCANF_DX=PROCEDURE,-
   DM_SCANF_TX=PROCEDURE,-
   DM_SCANF_G=PROCEDURE,-
   DM_SCANF_D=PROCEDURE,-
   DM_SCANF_T=PROCEDURE,-
!
   DM_FPRINTF_GX=PROCEDURE,-
   DM_FPRINTF_DX=PROCEDURE,-
   DM_FPRINTF_TX=PROCEDURE,-
   DM_FPRINTF_G=PROCEDURE,-
   DM_FPRINTF_D=PROCEDURE,-
   DM_FPRINTF_T=PROCEDURE,-
   DM_PRINTF_GX=PROCEDURE,-
   DM_PRINTF_DX=PROCEDURE,-
   DM_PRINTF_TX=PROCEDURE,-
   DM_PRINTF_G=PROCEDURE,-
   DM_PRINTF_D=PROCEDURE,-
   DM_PRINTF_T=PROCEDURE)
!

SYMBOL_VECTOR=(-
   DM_FCNTL=PROCEDURE,-
   DM_SELECT=PROCEDURE,-
   DM_POLL=PROCEDURE,-
   DM_PERROR=PROCEDURE,-
   DM_FFLUSH=PROCEDURE,-
   DM_FSYNC=PROCEDURE,-
   DM_ISAPIPE=PROCEDURE)
!
! Lower case aliases.
!
SYMBOL_VECTOR=(-
   dm_pipe/DM_PIPE=PROCEDURE,-
   dm_read/DM_READ=PROCEDURE,-
   dm_write/DM_WRITE=PROCEDURE,-
   dm_open/DM_OPEN=PROCEDURE,-
   dm_close/DM_CLOSE=PROCEDURE,-
   dm_get_statistics/DM_GET_STATISTICS=PROCEDURE,-
   dm_dup/DM_DUP=PROCEDURE,dm_dup2/DM_DUP2=PROCEDURE,-
   dm_fdopen/DM_FDOPEN=PROCEDURE,-
   dm_popen/DM_POPEN=PROCEDURE,-
   dm_pclose/DM_PCLOSE=PROCEDURE,-
   dm_fopen/DM_FOPEN=PROCEDURE,-
   dm_fclose/DM_FCLOSE=PROCEDURE,-
   dm_fread/DM_FREAD=PROCEDURE,-
   dm_fwrite/DM_FWRITE=PROCEDURE,-
   dm_fgets/DM_FGETS=PROCEDURE,-
   dm_fgetc/DM_FGETC=PROCEDURE,-
   dm_ungetc/DM_UNGETC=PROCEDURE,-
!
   dm_fscanf_gx/DM_FSCANF_GX=PROCEDURE,-
   dm_fscanf_dx/DM_FSCANF_DX=PROCEDURE,-
   dm_fscanf_tx/DM_FSCANF_TX=PROCEDURE,-
   dm_fscanf_g/DM_FSCANF_G=PROCEDURE,-
   dm_fscanf_d/DM_FSCANF_D=PROCEDURE,-
   dm_fscanf_t/DM_FSCANF_T=PROCEDURE,-
   dm_scanf_gx/DM_SCANF_GX=PROCEDURE,-
   dm_scanf_dx/DM_SCANF_DX=PROCEDURE,-
   dm_scanf_tx/DM_SCANF_TX=PROCEDURE,-
   dm_scanf_g/DM_SCANF_G=PROCEDURE,-
   dm_scanf_d/DM_SCANF_D=PROCEDURE,-
   dm_scanf_t/DM_SCANF_T=PROCEDURE,-
!
   dm_fprintf_gx/DM_FPRINTF_GX=PROCEDURE,-
   dm_fprintf_dx/DM_FPRINTF_DX=PROCEDURE,-
   dm_fprintf_tx/DM_FPRINTF_TX=PROCEDURE,-
   dm_fprintf_g/DM_FPRINTF_G=PROCEDURE,-
   dm_fprintf_d/DM_FPRINTF_D=PROCEDURE,-
   dm_fprintf_t/DM_FPRINTF_T=PROCEDURE,-
   dm_printf_gx/DM_PRINTF_GX=PROCEDURE,-
   dm_printf_dx/DM_PRINTF_DX=PROCEDURE,-
   dm_printf_tx/DM_PRINTF_TX=PROCEDURE,-
   dm_printf_g/DM_PRINTF_G=PROCEDURE,-
   dm_printf_d/DM_PRINTF_D=PROCEDURE,-
   dm_printf_t/DM_PRINTF_T=PROCEDURE)
!

SYMBOL_VECTOR=(-
   dm_fcntl/DM_FCNTL=PROCEDURE,-
   dm_select/DM_SELECT=PROCEDURE,-
   dm_poll/DM_POLL=PROCEDURE,-
   dm_perror/DM_PERROR=PROCEDURE,-
   dm_fflush/DM_FFLUSH=PROCEDURE,-
   dm_fsync/DM_FSYNC=PROCEDURE,-
   dm_isapipe/DM_ISAPIPE=PROCEDURE,-
   DM_FPUTS=PROCEDURE,-
   dm_fputs/DM_FPUTS=PROCEDURE,-
   DMPIPE_RESET_TRACE_FILE=PROCEDURE,-
   dmpipe_reset_trace_file/DMPIPE_RESET_TRACE_FILE=PROCEDURE,-
   DM_PUTS=PROCEDURE,-
   DM_FPUTC=PROCEDURE,-
   dm_puts/DM_PUTS=PROCEDURE,-
   dm_fputc/DM_FPUTC=PROCEDURE,-
   DM_FEOF=PROCEDURE,-
   dm_feof/DM_FEOF=PROCEDURE)

CASE_SENSITIVE=NO

$ eod
