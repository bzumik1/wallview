#!/bin/sh
P=$(for p in $(pidof WPEWebProcess); do for t in /proc/$p/task/*; do case "$(cat $t/comm)" in eadedCompositor) echo "$(awk '{print $14+$15}' $t/stat) $p";; esac; done; done | sort -n | tail -1 | cut -d' ' -f2)
U=$(pidof wallview-browser)
tids=""
for t in /proc/$P/task/*; do c=$(cat $t/comm); case "$c" in WPEWebProcess|eadedCompositor|"Core: Scrolling"|EventDispatcher|ReceiveQueue|SkiaGPUWorker) tids="$tids,$(basename $t)";; esac; done
for t in /proc/$U/task/*; do c=$(cat $t/comm); case "$c" in wallview-browse|SyncObserverDRM) tids="$tids,$(basename $t)";; esac; done
tids=${tids#,}
rm -f /tmp/flip.log /tmp/touch.log /tmp/cap.data
(FLIPMON_TIMES=1 /tmp/flipmon 16 > /dev/null 2> /tmp/flip.log &)
/tmp/perf record -o /tmp/cap.data -e cpu-clock -F 3000 --switch-events -k CLOCK_MONOTONIC -t $tids -- sleep 13 > /tmp/cap.err 2>&1 &
sleep 1
/tmp/touchplay ${1:-scroll} 4 2> /tmp/touch.log
wait
grep -m3 libWPEWebKit /proc/$P/maps
echo "PID $P UI $U"
for t in /proc/$P/task/* /proc/$U/task/*; do echo "TID $(basename $t) $(cat $t/comm)"; done
echo ===TOUCH; cat /tmp/touch.log
echo ===FLIP; cat /tmp/flip.log
echo ===PERF
/tmp/perf script -i /tmp/cap.data --show-switch-events -F tid,time,event,ip,dso 2>/dev/null
