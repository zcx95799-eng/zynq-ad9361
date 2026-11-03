# Run with Vivado 2019.1 -mode batch -source scripts/build_vivado.tcl
# Only operates on this delivery copy. Does not upgrade IP or change coefficients.
set board [file normalize [file join [file dirname [info script]] ..]]
set project [file join $board project vivado AD9361_LVDS_PRJ AD9361_LVDS_PRJ.xpr]
set output [file join $board build fpga [clock format [clock seconds] -format %Y%m%d_%H%M%S]]
file mkdir $output
open_project $project
if {[get_property PART [current_project]] ne "xc7z020clg400-1"} { error "Unexpected FPGA part" }
report_ip_status -file [file join $output ip_status.rpt]
reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {![string match "*Complete*" [get_property STATUS [get_runs synth_1]]]} { error "Synthesis failed" }
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {![string match "*Complete*" [get_property STATUS [get_runs impl_1]]]} { error "Implementation failed" }
open_run impl_1
report_utilization -file [file join $output utilization.rpt]
report_timing_summary -file [file join $output timing_summary.rpt]
write_debug_probes -force [file join $output system_top.ltx]
set rundir [get_property DIRECTORY [get_runs impl_1]]
file copy -force [file join $rundir system_top.bit] [file join $output system_top.bit]
puts "BUILD_OUTPUT=$output"
close_project
exit
