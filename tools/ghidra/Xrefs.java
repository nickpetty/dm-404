// For each address argument: list the code references to it, then
// decompile each distinct referencing function (at most 12 per address).
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.util.LinkedHashSet;

public class Xrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseUnsignedLong(arg.replace("0x", ""), 16));
            LinkedHashSet<Function> fs = new LinkedHashSet<>();
            System.out.println("==== refs to " + a);
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                System.out.println("  " + r.getFromAddress() + " " + r.getReferenceType() +
                    (f == null ? "" : " in " + f.getName() + " @" + f.getEntryPoint()));
                if (f != null) fs.add(f);
            }
            int n = 0;
            for (Function f : fs) {
                if (n++ >= 12) break;
                System.out.println("==== " + f.getName() + " @" + f.getEntryPoint());
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                System.out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : r.getErrorMessage());
            }
        }
        System.out.println("==== end");
    }
}
