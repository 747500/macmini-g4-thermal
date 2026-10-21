#!/bin/bash
# Heat test: 2 min idle, 8 min one `yes` (100% of the single G4 core), 4 min cool-down.
# One sample every 15 s: local and remote temperature, load average.
# Usage: sudo ./loadtest.sh [path/to/g4temp] > loadtest.log
g4temp=${1:-g4temp}
s() {
  t=$("$g4temp" -p | awk '/^node_hwmon_temp_celsius/ { printf "%s ", $2 }')
  set -- $(sysctl -n vm.loadavg)
  echo "$(date +%H:%M:%S) $phase local/remote: $t load1: $2"
}
phase=idle; for i in $(jot 8); do s; sleep 15; done
phase=load; yes > /dev/null & y=$!
trap 'kill $y 2>/dev/null' EXIT
for i in $(jot 32); do s; sleep 15; done
kill $y; phase=cool
for i in $(jot 17); do s; sleep 15; done
