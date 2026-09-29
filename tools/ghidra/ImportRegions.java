// Adds every other unpacked region (vectors, ITCM code, TCM/OCRAM/SDRAM data)
// to the program as initialized memory blocks, and the i.MX RT1060 peripheral
// space as an uninitialized volatile block, so references resolve.
// Argument: the build/regions directory written by tools/unpack.py.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.*;
import java.io.File;
import java.nio.file.Files;

public class ImportRegions extends GhidraScript {
    @Override
    public void run() throws Exception {
        File dir = new File(getScriptArgs()[0]);
        Memory mem = currentProgram.getMemory();
        for (File f : dir.listFiles()) {
            String n = f.getName();
            if (!n.startsWith("r_") || n.startsWith("r_80000000_")) continue;
            long addr = Long.parseUnsignedLong(n.substring(2, 10), 16);
            byte[] data = Files.readAllBytes(f.toPath());
            Address a = toAddr(addr);
            MemoryBlock b = mem.createInitializedBlock(n.replace(".bin", ""), a,
                new java.io.ByteArrayInputStream(data), data.length, monitor, false);
            b.setRead(true); b.setWrite(true);
            b.setExecute(addr < 0x20000 || n.contains("copy"));
            println("block " + n + " @" + a + " len 0x" + Integer.toHexString(data.length));
        }
        MemoryBlock p = mem.createUninitializedBlock("periph", toAddr(0x40000000L), 0x00400000, false);
        p.setVolatile(true); p.setRead(true); p.setWrite(true);
        MemoryBlock s = mem.createUninitializedBlock("scs", toAddr(0xE0000000L), 0x00100000, false);
        s.setVolatile(true); s.setRead(true); s.setWrite(true);
    }
}
