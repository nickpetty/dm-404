# Log every string the firmware draws (DrawString, FUN_800ee530(this, x, y,
# str, len), per tallfree), and every file it opens.
set pagination off
set confirm off
target remote 127.0.0.1:1234
break *0x800ee530
commands
silent
printf "DRAW x=%d y=%d \"%s\"\n", $r1, $r2, (char *)$r3
continue
end
break *0x800b5d88
commands
silent
printf "OPEN %s\n", (char *)$r1
continue
end
continue
