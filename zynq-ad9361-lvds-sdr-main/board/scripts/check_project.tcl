# Offline project-open and file-reference check; no synthesis or hardware access.
set board [file normalize [file join [file dirname [info script]] ..]]
set output [file join $board validation]
file mkdir $output
set report [open [file join $output vivado_project_check.txt] w]
set project [file join $board project vivado AD9361_LVDS_PRJ AD9361_LVDS_PRJ.xpr]
open_project $project
puts $report "Version: [version -short]"
puts $report "Part: [get_property PART [current_project]]"
puts $report "Top: [get_property TOP [get_filesets sources_1]]"
set missing {}
set count 0
foreach f [get_files -all] {
    incr count
    set name [get_property NAME $f]
    if {![file exists $name]} { lappend missing $name }
}
puts $report "Files checked: $count"
puts $report "Missing: $missing"
report_ip_status -file [file join $output ip_status.rpt]
close $report
close_project
if {[llength $missing]} { error "Project has missing file references: $missing" }
puts "OFFLINE_PROJECT_CHECK_PASS"
exit
