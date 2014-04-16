$ sv = 'f$verify(0)'
$!
$! Generate and FDL file to sys$output for a stream-LF file with a DATE
$! section specifying the creation and revision dates for the file
$! specified by P1.  Designed for use in a pipeline:
$!
$!    pipe @stream_fdl 'src_file' | convert/fdl=sys$input 'src_file' 'dst_file'
$!
$ create sys$output
$ deck
IDENT	FDL_VERSION 02 "12-APR-2014 08:48:44   OpenVMS ANALYZE/RMS_FILE Utility"

SYSTEM
	SOURCE                  OpenVMS

RECORD
	BLOCK_SPAN              yes
	CARRIAGE_CONTROL        carriage_return
	FORMAT                  stream_lf
	SIZE                    0

$ eod
$!
$ if p1 .eqs. "" then goto done
$ if f$search(p1) .eqs. "" then goto done
$ credat = f$file_attributes(P1,"CDT")
$ revdat = f$file_attributes(P1,"RDT")
$ write sys$output "DATE"
$ line = f$fao("!_CREATION!_""!AS""",credat)
$ write/symbol  sys$output line
$ line = f$fao("!_REVISION!_""!AS""",revdat)
$ write/symbol  sys$output line
$ done:
$ sv = f$verify(sv)
