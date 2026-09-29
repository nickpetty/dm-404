# Log every file the firmware opens (FUN_800b5d88(handle, path, mode)).
set pagination off
set confirm off
target remote 127.0.0.1:1234
break *0x800b5d88
commands
silent
printf "OPEN mode=%d %s\n", $r2, (char *)$r1
continue
end
continue
