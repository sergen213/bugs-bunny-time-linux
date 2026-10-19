// Decompile every function (or only the given entry addresses) to one C text file.
// Usage (headless): -postScript DecompileAll.java <outdir> [hexaddr ...]   (writes <outdir>/<program>.c)
// Each function is preceded by "//==== <entry> <name>" so the output can be grepped.
//@category BBTime
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class DecompileAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<Function> fns = new ArrayList<>();
        if (args.length > 1) {
            for (int i = 1; i < args.length; i++) {
                Function f = getFunctionContaining(toAddr(args[i]));
                if (f != null) fns.add(f); else println("no function at " + args[i]);
            }
        } else {
            currentProgram.getListing().getFunctions(true).forEach(fns::add);
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        String out = args[0] + "/" + currentProgram.getName() + ".c";
        try (PrintWriter pw = new PrintWriter(out)) {
            for (Function f : fns) {
                if (f.isExternal() || f.isThunk()) continue;
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                pw.printf("//==== %s %s%n", f.getEntryPoint(), f.getName());
                pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// FAILED: " + r.getErrorMessage());
            }
        }
        di.dispose();
        println("wrote " + out);
    }
}
