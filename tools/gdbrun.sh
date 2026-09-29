#!/bin/sh
# gdbrun.sh SECONDS SCRIPT.gdb [QEMU ARGS...]: boot under QEMU's GDB stub
# with the eMMC (and SD if SP404_SD is set) and run a GDB script for
# SECONDS. GDB output: build/logs/gdb.log
root=$(cd "$(dirname "$0")/.." && pwd)
secs=$1; script=$2; shift 2
sd=""
[ -n "$SP404_SD" ] && sd="-drive if=sd,index=0,format=raw,file=$root/build/sd.img"
"$root/build/qemu/qemu-system-arm.exe" -M sp404mk2,flash="$root/build/flash.bin" \
    -bios "$root/firmware/SP404MKII_APP1.bin" -nographic -monitor none -serial none \
    $sd -drive if=sd,index=1,format=raw,file="$root/build/emmc.img" \
    -s -S -D "$root/build/logs/gdbqemu.log" "$@" > /dev/null 2>&1 &
qpid=$!
sleep 1
timeout "$secs" /c/msys64/ucrt64/bin/gdb-multiarch.exe -q -batch -x "$script" > "$root/build/logs/gdb.log" 2>&1
kill $qpid 2>/dev/null
