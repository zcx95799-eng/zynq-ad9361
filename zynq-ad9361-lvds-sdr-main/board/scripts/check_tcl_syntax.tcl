# XSCT checks syntax completeness only; does not source hardware scripts.
set board [file normalize [file join [file dirname [info script]] ..]]
set report [open [file join $board validation tcl_syntax.txt] w]
foreach script [glob [file join $board scripts *.tcl]] {
    set f [open $script r]
    set source [read $f]
    close $f
    if {![info complete $source]} { error "Incomplete Tcl syntax: $script" }
    puts $report "COMPLETE [file tail $script]"
}
puts $report "Syntax completeness only; no board operations executed."
close $report
puts "TCL_COMPLETENESS_PASS"
exit
