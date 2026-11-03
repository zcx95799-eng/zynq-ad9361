# Source in the current Vivado GUI Tcl console AFTER capture/upload completes.
# Exports existing in-memory data only. No run_hw_ila, upload, reset or programming.
set board [file normalize [file join [file dirname [info script]] ..]]
set output [file join $board run_outputs [clock format [clock seconds] -format %Y%m%d_%H%M%S]_ila]
set datas [get_hw_ila_datas]
if {[llength $datas] == 0} { error "No in-memory ILA data. Finish capture and Upload Data in Vivado first." }
file mkdir $output
set index 0
set log [open [file join $output export.txt] w]
puts $log "Exported existing Vivado data objects at [clock format [clock seconds]]"
foreach data $datas {
    incr index
    set name [format "capture_%02d" $index]
    write_hw_ila_data -force [file join $output $name.ila] $data
    write_hw_ila_data -force -csv_file [file join $output $name.csv] $data
    puts $log "$name : $data"
}
close $log
puts "Exported $index existing ILA data objects into $output"
