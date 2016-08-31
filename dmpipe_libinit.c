/* File: DMPIPE_LIBINIT.C

 This file contains the DMPIPE library initialization procedure that
 is executed whenever a process executes an image linked against the
 DMPIPE library shareable image DMPIPESHR.EXE. The initialization
 procedure is invoked using the OpenVMS LIB$INITIALIZE mechanism
 which will be invoked as a result of the image activator attempting
 to activate the DMPIPESHR.EXE shareable image as part of image activation
 for a process executing a main image that is linked against DMPIPESHR.EXE
 
 The sole purpose of the DMPIPE library initialization routine is to
 establish a routine (CloseAllDMPipeBypasses) to be executed once all other
 exit routines established by the "main" routine have finished execution.
 This routine is expected to be called only after it is certain that no
 other routines that reference fd's or FILE *'s will execute. Since the
 order of execution for routines established using the CRTL atexit() is
 the reverse order of their registration, the DMPIPE library
 routine CloseAllBypasses must be registered prior to any exit routine
 established by the "main" routine of the image. Because the LIB$INITIALIZE
 mechanism for OpenVMS images executes before the "main" routine of an
 executable image, this mechanism was chosen as the vehicle for registering
 the CloseAllDMPipeBypasses.
*/

#include <descrip.h>
#include <stsdef.h>
#include <lib$routines.h>
#include <stddef.h>

#pragma message disable pragma
#pragma message disable dollarid
#pragma message disable valuepres
/*
  Constant strings, associated descriptors and function pointers
  for use in calls to LIB$FIND_IMAGE_SYMBOL().
*/
static const $DESCRIPTOR(CRTLShrImageNameDscr,"DECC$SHR");
static const $DESCRIPTOR(CRTL_Init_NameDescr, "DECC$CRTL_INIT");
static const $DESCRIPTOR(CRTL_atexit_NameDescr, "DECC$ATEXIT");


typedef int (*atexit_funcptr)(void(*__func)(void));
typedef void (*crtl_init_funcptr)(void);

crtl_init_funcptr crtl_init;
atexit_funcptr atexit;


#pragma member_alignment save
#pragma nomember_alignment longword

extern void CloseAllDMPipeBypasses(void);

static void dmpipe_libinit ( void )
{
   int status;

/*
  Force the activation of the CRTL shareable image by using
  LIB$FIND_IMAGE_SYMBOL() to get the addresses for the CRTL
  initialization function and the atexit function.
*/
   status = lib$find_image_symbol(&CRTLShrImageNameDscr, &CRTL_Init_NameDescr,
                                  &crtl_init, NULL, 0);

   status = lib$find_image_symbol(&CRTLShrImageNameDscr, &CRTL_atexit_NameDescr,
                                  &atexit, NULL, 0);
/*
  Init the CRTL just in case this is the initial activation for the CRTL
  shareable image.
*/
  crtl_init();

/*
  Register the CloseAllDMPipeBypasses() function to run down all of the DMPIPE
  bypasses after the "main" routine exits.
*/
  atexit(CloseAllDMPipeBypasses);
}

#pragma nostandard
#pragma extern_model save
#ifdef __VAX
#pragma extern_model strict_refdef "LIB$INITIALIZE" nowrt, long, nopic
#else
#pragma extern_model strict_refdef "LIB$INITIALIZE" nowrt, long
#    if __INITIAL_POINTER_SIZE
#        pragma __pointer_size __save
#        pragma __pointer_size 32
#    else
#        pragma __required_pointer_size __save
#        pragma __required_pointer_size 32
#    endif
#endif
/* Set our contribution to the LIB$INITIALIZE array */
void (* const iniarray[])(void) = {dmpipe_libinit, } ;
#ifndef __VAX
#    if __INITIAL_POINTER_SIZE
#        pragma __pointer_size __restore
#    else
#        pragma __required_pointer_size __restore
#    endif
#endif


/*
** Force a reference to LIB$INITIALIZE to ensure it
** exists in the image.
*/
int LIB$INITIALIZE(void);
#ifdef __DECC
#pragma extern_model strict_refdef
#endif
    int lib_init_ref = (int) LIB$INITIALIZE;
#ifdef __DECC
#pragma extern_model restore
#pragma standard
#endif
