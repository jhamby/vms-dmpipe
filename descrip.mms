!
! MMS description file for dmpipe utlity, sample and test programs.
!
! Build option macros:
!    PRIVATE_DOPRINT    If defined, link programs with private implentation
!                       of doprint engine and decc$shr.exe.  If not defined,
!                       link against static object modules in object library
!			sys$share:starlet.olb
!
!    SHARE		If defined, set PRIVATE_DOPRINT and link against
!                       shareable image dmpipe_exe:dmpipeshr.exe (options
!			file dmpipeshr.opt).
!
! Architeture-specific directories created:
!  [.alpha_exe]
!  [.alpha_lib]
!  [.ia64_exe]
!  [.ia64_lib]
!
! Targets:
!   all
!   'exe_dir'hmac.exe
!   'exe_dir'case_munge.exe
!   'exe_dir'test_poll.exe
!   'exe_dir'pipe_torture.exe
!   'exe_dir'dmpipeshr.exe
!
.IFDEF MMSALPHA
odir_name = alpha_obj
edir_name = alpha_exe
.ELSE
odir_name = ia64_obj
edir_name = ia64_exe
.ENDIF
odir = [.$(odir_name)]
edir = [.$(edir_name)]

.IFDEF TRACEBACK
S_LINKFLAGS = /TRACEBACK/EXEC=$(MM$TARGET_NAME).EXE
.ELSE
S_LINKFLAGS = /NOTRACEBACK/EXEC=$(MMS$TARGET_NAME).EXE
.ENDIF

.IFDEF SHARE
PRIVATE_DOPRINT = TRUE
shareable_image = $(edir)dmpipeshr.exe
.ELSE
shareabable_image = 
.ENDIF

.IFDEF PRIVATE_DOPRINT
doprint_objs = $(odir)doprint.obj $(odir)doprint_flt_gx.obj -
  $(odir)doprint_flt_g.obj $(odir)doprint_flt_dx.obj $(odir)doprint_flt_d.obj -
  $(odir)doprint_flt_tx.obj $(odir)doprint_flt_t.obj
.IFDEF SHARE
doprint_opt_file = []dmpipeshr.opt
.ELSE
doprint_opt_file = []dmpipe_private_doprint.opt
.ENDIF
dmpipe_cc_quals = 
dmpipe_obj = $(odir)dmpipe-private_doprint.obj
.ELSE
doprint_opt_file = []dmpipe.opt
doprint_objs = 
dmpipe_cc_quals = /define=USE_SYSTEM_DOPRINT
dmpipe_obj = $(odir)dmpipe.obj
.ENDIF

LINKFLAGS = /EXEC=$(MMS$TARGET_NAME).EXE/map/cross

.FIRST
  @ IF f$environment("DEPTH") .gt. 0 then sv_vfy = f$verify(1)
  @ if f$getsyi("CPU") .ge. 128 .or. f$trnlnm("DECC$CC_DEFAULT") .eqs. "/DECC" -
	then prefix_all="/prefix=all"
  @ if f$search("$(odir_name).dir;1") .eqs. "" then create/dir/log $(odir)
  @ if f$search("$(edir_name).dir;1") .eqs. "" then create/dir/log $(edir)
  @ define dmpipe_lib $(odir)
  @ define dmpipe_exe $(edir)
  @ define dmpipeshr $(edir)dmpipeshr.exe

.LAST
  @ IF f$environment("DEPTH") .gt. 0 then sv_vfy = f$verify(sv_vfy)

images = $(edir)hmac.exe $(edir)case_munge.exe $(edir)case_munge_0.exe -
	$(edir)hmac_0.exe $(edir)test_poll.exe $(edir)test_poll_0.exe -
	$(edir)pipe_torture.exe $(shareable_image)

doscan_objs = $(odir)doscan.obj $(odir)doscan_flt_gx.obj -
	$(odir)doscan_flt_g.obj $(odir)doscan_flt_dx.obj -
	$(odir)doscan_flt_d.obj $(odir)doscan_flt_tx.obj $(odir)doscan_flt_t.obj

lib_objs = $(dmpipe_obj) $(odir)dmpipe_bypass.obj $(odir)memstream.obj -
	$(odir)dmpipe_poll.obj $(doscan_objs) $(doprint_objs)

all : $(images)
   @ write sys$output "Images built"

$(edir)dmpipeshr.exe : make_dmpipeshr.com $(lib_objs)
   @make_dmpipeshr.com

$(edir)case_munge.exe : $(odir)case_munge.obj $(lib_objs) $(shareable_image) dmpipe.opt
   link $(LINKFLAGS) $(odir)case_munge.obj,$(doprint_opt_file)/option

$(edir)case_munge_0.exe : $(odir)case_munge_0.obj
   link $(LINKFLAGS) $(odir)case_munge_0.obj

$(edir)hmac.exe : $(odir)hmac.obj hmac.opt $(lib_objs) $(shareable_image) dmpipe.opt
   link $(LINKFLAGS) hmac.opt/option,$(doprint_opt_file)/option

$(edir)hmac_0.exe : $(odir)hmac_0.obj hmac_0.opt
   link $(LINKFLAGS) hmac_0.opt/opt

$(edir)test_poll.exe : $(odir)test_poll.obj $(lib_objs) $(shareable_image) dmpipe.opt
   link $(LINKFLAGS) $(odir)test_poll.obj,$(doprint_opt_file)/option

$(edir)test_poll_0.exe : $(odir)test_poll_0.obj $(lib_objs) $(sharable_image) dmpipe.opt
   link $(LINKFLAGS) $(odir)test_poll_0.obj,$(doprint_opt_file)/option

$(edir)pipe_torture.exe : $(odir)pipe_torture.obj $(lib_objs) $(shareable_image) dmpipe.opt
   link $(LINKFLAGS) $(odir)pipe_torture.obj,$(doprint_opt_file)/option

$(doprint_opt_file) : $(dmpipe_obj) $(odir)dmpipe_bypass.obj -
	$(odir)memstream.obj
   set file $(doprint_opt_file)/ext=0		! touch file

!
! Object file rules
!
$(dmpipe_obj) : dmpipe.h dmpipe.c dmpipe_bypass.h descrip.mms
   CC $(CFLAGS) dmpipe.c $(dmpipe_cc_quals)

$(odir)dmpipe_bypass.obj : dmpipe_bypass.c dmpipe_bypass.h memstream.h
   CC $(CFLAGS) dmpipe_bypass.c

!
! User applications should only need to include dmpipe.h
! the '_0' version is compiled without enable_bypass.
!
$(odir)hmac.obj : hmac.c dmpipe.h dmpipe_main.c
   CC $(CFLAGS) hmac.c  /define=(ENABLE_BYPASS,DM_WRAP_MAIN)

$(odir)case_munge.obj : case_munge.c dmpipe.h dmpipe_main.c
  CC $(CFLAGS) case_munge.c  /define=(ENABLE_BYPASS,DM_WRAP_MAIN)

$(odir)test_poll.obj : test_poll.c dmpipe.h
   CC $(CFLAGS) test_poll.c

$(odir)pipe_torture.obj : pipe_torture.c dmpipe.h dmpipe_main.c
  CC $(CFLAGS) pipe_torture.c

$(odir)test_poll_0.obj : test_poll.c dmpipe.h
   CC $(CFLAGS) test_poll.c/object=$(odir)test_poll_0.obj/define=DM_NO_CRTL_WRAP

$(odir)case_munge_0.obj : case_munge.c
  CC $(CFLAGS) case_munge.c /object=$(odir)case_munge_0.obj

$(odir)hmac_0.obj : hmac.c
   CC $(CFLAGS) hmac.c  /object=$(odir)hmac_0.obj
  
$(odir)memstream.obj : memstream.c memstream.h
  CC $(CFLAGS) memstream.c

$(odir)dmpipe_poll.obj : dmpipe_poll.c dmpipe_poll.h dmpipe_bypass.h
  CC $(CFLAGS) dmpipe_poll.c

!
! Modules for private doscan engine for use when fscanf function with pipes.
!
$(odir)doscan.obj : doscan.c doscan.h
  CC $(CFLAGS) doscan.c

$(odir)doscan_flt_gx.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=128/list/show=exp

$(odir)doscan_flt_g.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=64/list/show=exp

$(odir)doscan_flt_dx.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=128/list/show=exp

$(odir)doscan_flt_d.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=64/list/show=exp

$(odir)doscan_flt_tx.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=128/list/show=exp

$(odir)doscan_flt_t.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=64/list/show=exp

!
! Modules for private doprint engine that allow us to link with
! decc$shr shareable image rather than starlat of decc$rtl object library.
!
$(odir)doprint.obj : doprint.c doprint.h
  CC $(CFLAGS) doprint.c

$(odir)doprint_flt_gx.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=128/list/show=exp

$(odir)doprint_flt_g.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=64/list/show=exp

$(odir)doprint_flt_dx.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=128/list/show=exp

$(odir)doprint_flt_d.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=64/list/show=exp

$(odir)doprint_flt_tx.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=128/list/show=exp
                                                                            
$(odir)doprint_flt_t.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=64/list/show=exp
                                                                            

