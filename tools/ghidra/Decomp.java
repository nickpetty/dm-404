// Decompiles the functions containing each address given as an argument
// (hex, with or without 0x). Output is framed by ==== lines for tools/gdec.sh.
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decomp extends GhidraScript {
    /* Ghidra misses functions reached only through pointers: walk back to
     * the nearest push and make a function there. */
    Function recover(Address a) throws Exception {
        disassemble(a);
        for (long o = a.getOffset() & ~1L; o > a.getOffset() - 0x1000; o -= 2) {
            ghidra.program.model.listing.Instruction ins = getInstructionAt(toAddr(o));
            if (ins == null) {
                disassemble(toAddr(o));
                ins = getInstructionAt(toAddr(o));
            }
            if (ins != null && ins.getMnemonicString().startsWith("push")) {
                Function f = createFunction(toAddr(o), null);
                if (f != null && f.getBody().contains(a)) return f;
            }
        }
        return null;
    }

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseUnsignedLong(arg.replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {
                f = recover(a);
            }
            System.out.println("==== " + arg + (f == null ? " (no function)" : " in " + f.getName() + " @" + f.getEntryPoint()));
            if (f == null) continue;
            DecompileResults r = d.decompileFunction(f, 60, monitor);
            System.out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : r.getErrorMessage());
        }
        System.out.println("==== end");
    }
}
