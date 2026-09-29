#!/bin/sh
# run.sh [SECONDS] [QEMU ARGS...]: boot the firmware headless for SECONDS
# (default 8) with every peripheral access logged, then summarize: exceptions,
# the last new registers touched, and the hottest accesses at the end, which
# is where a polling loop shows up. Full log: build/logs/run.log
root=$(cd "$(dirname "$0")/.." && pwd)
secs=${1:-8}; [ $# -gt 0 ] && shift
log=$root/build/logs/run.log
mkdir -p "$root/build/logs"
SP404_TRACE=${SP404_TRACE:-bmc,lpuart,lpspi,edma} timeout "$secs" "$root/build/qemu/qemu-system-arm.exe" -M sp404mk2,flash="$root/build/flash.bin" \
    -bios "$root/firmware/SP404MKII_APP1.bin" -nographic -monitor none \
    -accel tcg,one-insn-per-tb=on \
    -drive if=sd,index=0,format=raw,file="$root/build/sd.img" \
    -drive if=sd,index=1,format=raw,file="$root/build/emmc.img" \
    -d unimp,guest_errors,int -D "$log" "$@" > "$root/build/logs/run.out" 2>&1
echo "== exit $? after ${secs}s; log $(wc -l < "$log") lines"
echo "== exceptions and errors"
grep -v -E '^(sp404-stub|edma|lpspi|lpuart|bmc|flexspi)|DRBAR' "$log" | sed 's/0x[0-9a-f]*/X/g' | sort | uniq -c | sort -rn | head -12
echo "== device trace lines"
grep -E '^(edma|lpspi|lpuart|bmc|flexspi):' "$log" | awk '{print $1}' | sort | uniq -c
echo "== last new registers"
grep 'first ' "$log" | tail -${TAILN:-12}
echo "== hottest accesses in the final 20000 lines"
tail -20000 "$log" | grep -E '^sp404-stub: (read|write) ' | awk '{print $2, $3, $NF}' | sort | uniq -c | sort -rn | head -6
