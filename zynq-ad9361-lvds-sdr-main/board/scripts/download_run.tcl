# XSCT 2019.1 script. Invoking this script resets/programs the selected board.
# Usage: xsct scripts/download_run.tcl [hw_server_url] [ELF_path]
set board [file normalize [file join [file dirname [info script]] ..]]
set url "tcp:127.0.0.1:3121"
if {$argc >= 1} { set url [lindex $argv 0] }
set elf [file join $board artifacts sdk AD9361.elf]
if {$argc >= 2} { set elf [file normalize [lindex $argv 1]] }
set bit [file join $board artifacts fpga system_top.bit]
set init [file join $board artifacts fpga ps7_init.tcl]
foreach f [list $bit $init $elf] {
    if {![file isfile $f]} { error "Missing input: $f" }
}
connect -url $url
set filter {name =~ "Cortex-A9*#0"}
set cores [targets -target-properties -filter $filter]
if {[llength $cores] != 1} {
    puts [targets]
    error "Expected exactly one Cortex-A9 core 0. Disconnect extra boards or use a dedicated hw_server."
}
targets -set -filter $filter
stop
rst -system
after 3000
targets -set -filter $filter
fpga -file $bit
source $init
ps7_init
ps7_post_config
targets -set -filter $filter
dow $elf
con
puts "RUNNING: $elf"
disconnect
exit
