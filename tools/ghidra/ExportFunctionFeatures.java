// Export per-function features for cross-binary matching (see match_functions.py).
// Usage (headless): -postScript ExportFunctionFeatures.java <outdir>   (writes <outdir>/<program>.features.tsv)
// Columns: entry name ninstr size mnemhash calls(callee entries) imports strings scalars mnems
// Scalars: immediate operands >= 0x100 that are not addresses inside the program.
//@category BBTime
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.security.MessageDigest;
import java.util.*;

public class ExportFunctionFeatures extends GhidraScript {
    private static String clean(String s) {
        return s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').replace('|', '/');
    }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0] + "/" + currentProgram.getName() + ".features.tsv";
        Listing listing = currentProgram.getListing();
        try (PrintWriter pw = new PrintWriter(out)) {
            pw.println("entry\tname\tninstr\tsize\tmnemhash\tcalls\timports\tstrings\tscalars\tmnems");
            for (Function f : listing.getFunctions(true)) {
                StringBuilder mn = new StringBuilder();
                List<String> calls = new ArrayList<>();
                TreeSet<String> imports = new TreeSet<>(), strings = new TreeSet<>(), scalars = new TreeSet<>();
                int n = 0;
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    n++;
                    mn.append(ins.getMnemonicString()).append(' ');
                    for (int i = 0; i < ins.getNumOperands(); i++) {
                        for (Object o : ins.getOpObjects(i)) {
                            if (o instanceof Scalar sc) {
                                long v = sc.getUnsignedValue();
                                if (v >= 0x100 && currentProgram.getMemory().getBlock(toAddr(v)) == null)
                                    scalars.add(Long.toHexString(v));
                            }
                        }
                    }
                    for (Reference r : ins.getReferencesFrom()) {
                        Address to = r.getToAddress();
                        if (r.getReferenceType().isCall()) {
                            Function cf = getFunctionAt(to);
                            if (cf == null) { // call through IAT pointer
                                Data d = getDataAt(to);
                                if (d != null && d.isPointer() && d.getValue() instanceof Address a) cf = getFunctionAt(a);
                            }
                            if (cf != null && cf.isExternal()) imports.add(cf.getName());
                            else if (cf != null && cf.isThunk() && cf.getThunkedFunction(true).isExternal())
                                imports.add(cf.getThunkedFunction(true).getName());
                            else if (cf != null) calls.add(cf.getEntryPoint().toString());
                            continue;
                        }
                        Data d = getDataAt(to);
                        if (d != null && d.hasStringValue()) strings.add(clean(String.valueOf(d.getValue())));
                        else if (d != null && d.isPointer() && d.getValue() instanceof Address a) {
                            Function ef = getFunctionAt(a);
                            if (ef != null && ef.isExternal()) imports.add(ef.getName());
                        }
                    }
                }
                byte[] h = MessageDigest.getInstance("SHA-1").digest(mn.toString().getBytes());
                pw.printf("%s\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s%n",
                    f.getEntryPoint(), f.getName(), n, f.getBody().getNumAddresses(),
                    HexFormat.of().formatHex(h, 0, 8), String.join(",", calls), String.join(",", imports),
                    String.join("|", strings), String.join(",", scalars), mn.toString().trim());
            }
        }
        println("wrote " + out);
    }
}
