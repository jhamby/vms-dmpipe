!
! MMS description file for dmpipe utlity, sample and test programs.
!
! Macros:
!    PRIVATE_DOPRINT
!
.IFDEF TRACEBACK
S_LINKFLAGS = /TRACEBACK/EXEC=$(MM$TARGET_NAME).EXE
.ELSE
S_LINKFLAGS = /NOTRACEBACK/EXEC=$(MMS$TARGET_NAME).EXE
.ENDIF

.IFDEF PRIVATE_DOPRINT
doprint_opt_file = dmpipe_private_doprint.opt
doprint_objs = doprint.obj doprint_flt_gx.obj doprint_flt_g.obj doprint_flt_dx.obj -
  doprint_flt_d.obj doprint_flt_tx.obj doprint_flt_t.obj
dmpipe_cc_quals = 
dmpipe_obj = dmpipe-private_doprint.obj
.ELSE
doprint_opt_file = dmpipe.opt
doprint_objs = 
dmpipe_cc_quals = /define=USE_SYSTEM_DOPRINT
dmpipe_obj = dmpipe.obj
.ENDIF

LINKFLAGS = /EXEC=$(MMS$TARGET_NAME).EXE/map/cross

.FIRST
  @ IF f$environment("DEPTH") .gt. 0 then sv_vfy = f$verify(1)
  @ if f$getsyi("CPU") .ge. 128 .or. f$trnlnm("DECC$CC_DEFAULT") .eqs. "/DECC" -
	then prefix_all="/prefix=all"

.LAST
  @ IF f$environment("DEPTH") .gt. 0 then sv_vfy = f$verify(sv_vfy)

images = hmac.exe case_munge.exe case_munge_0.exe hmac_0.exe test_poll.exe -
	test_poll_0.exe pipe_torture.exe

doscan_objs = doscan.obj doscan_flt_gx.obj doscan_flt_g.obj doscan_flt_dx.obj -
  doscan_flt_d.obj doscan_flt_tx.obj doscan_flt_t.obj

lib_objs = $(dmpipe_obj) dmpipe_bypass.obj memstream.obj dmpipe_poll.obj -
	$(doscan_objs) $(doprint_objs)

all : $(images)
   @ write sys$output "Images built"

case_munge.exe : case_munge.obj $(lib_objs) dmpipe.opt
   link $(LINKFLAGS) case_munge.obj,$(doprint_opt_file)/option

case_munge_0.exe : case_munge_0.obj
   link $(LINKFLAGS) case_munge_0.obj

hmac.exe : hmac.obj hmac.opt $(lib_objs) dmpipe.opt
   link $(LINKFLAGS) hmac.opt/option,$(doprint_opt_file)/option

hmac_0.exe : hmac_0.obj hmac_0.opt
   link $(LINKFLAGS) hmac_0.opt/opt

test_poll.exe : test_poll.obj $(lib_objs) dmpipe.opt
   link $(LINKFLAGS) test_poll.obj,$(doprint_opt_file)/option

test_poll_0.exe : test_poll_0.obj $(lib_objs) dmpipe.opt
   link $(LINKFLAGS) test_poll_0.obj,$(doprint_opt_file)/option

pipe_torture.exe : pipe_torture.obj $(lib_objs) dmpipe.opt
   link $(LINKFLAGS) pipe_torture.obj,$(doprint_opt_file)/option

$(doprint_opt_file) : $(dmpipe_obj) dmpipe_bypass.obj memstream.obj
   set file $(doprint_opt_file)/ext=0		! touch file

!
! Object file rules
!
$(dmpipe_obj) : dmpipe.h dmpipe.c dmpipe_bypass.h descrip.mms
   CC $(CFLAGS) dmpipe.c $(dmpipe_cc_quals)

dmpipe_bypass.obj : dmpipe_bypass.c dmpipe_bypass.h memstream.h
   CC $(CFLAGS) dmpipe_bypass.c

!
! User applications should only need to include dmpipe.h
! the '_0' version is compiled without enable_bypass.
!
hmac.obj : hmac.c dmpipe.h dmpipe_main.c
   CC $(CFLAGS) hmac.c  /define=(ENABLE_BYPASS,DM_WRAP_MAIN)

case_munge.obj : case_munge.c dmpipe.h dmpipe_main.c
  CC $(CFLAGS) case_munge.c  /define=(ENABLE_BYPASS,DM_WRAP_MAIN)

test_poll.obj : test_poll.c dmpipe.h
   CC $(CFLAGS) test_poll.c

pipe_torture.obj : pipe_torture.c dmpipe.h dmpipe_main.c
  CC $(CFLAGS) pipe_torture.c

test_poll_0.obj : test_poll.c dmpipe.h
   CC $(CFLAGS) test_poll.c/object=test_poll_0.obj/define=DM_NO_CRTL_WRAP

case_munge_0.obj : case_munge.c
  CC $(CFLAGS) case_munge.c /object=case_munge_0.obj

hmac_0.obj : hmac.c
   CC $(CFLAGS) hmac.c  /object=hmac_0.obj
  
memstream.obj : memstream.c memstream.h
  CC $(CFLAGS) memstream.c

dmpipe_poll.obj : dmpipe_poll.c dmpipe_poll.h dmpipe_bypass.h
  CC $(CFLAGS) dmpipe_poll.c

!
! Modules for private doscan engine for use when fscanf function with pipes.
!
doscan.obj : doscan.c doscan.h
  CC $(CFLAGS) doscan.c

doscan_flt_gx.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=128/list/show=exp

doscan_flt_g.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=64/list/show=exp

doscan_flt_dx.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=128/list/show=exp

doscan_flt_d.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=64/list/show=exp

doscan_flt_tx.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=128/list/show=exp

doscan_flt_t.obj : doscan_flt_all.c doscan.h
  CC $(CFLAG) doscan_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=64/list/show=exp

!
! Modules for private doprint engine that allow us to link with
! decc$shr shareable image rather than starlat of decc$rtl object library.
!
doprint.obj : doprint.c doprint.h
  CC $(CFLAGS) doprint.c

doprint_flt_gx.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=128/list/show=exp

doprint_flt_g.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=G/l_double_size=64/list/show=exp

doprint_flt_dx.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=128/list/show=exp

doprint_flt_d.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=D/l_double_size=64/list/show=exp

doprint_flt_tx.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=128/list/show=exp
                                                                            
doprint_flt_t.obj : doprint_flt_all.c doprint.h
  CC $(CFLAG) doprint_flt_all.c -
	/object=$(MMS$TARGET_NAME)/float=IEEE/l_double_size=64/list/show=exp
                                                                            

