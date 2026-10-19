// Name the dynamically-loaded OpenGL/WGL/GLU function-pointer globals so decompiled renderer
// code reads "(*p_glVertex4f)(...)" instead of "(*DAT_xxxxxxxx)(...)".
// Two loader shapes are handled:
//  * LiT bugs.exe: static table of {PROC *slot, const char *name} pairs (BugsDecomp rogl.c, 0x468750)
//  * TB bblit2.exe: inline "slot = GetProcAddress(hLib, "glName")" sequences
// Run before DecompileAll.java in the same headless session (works with -readOnly).
//@category BBTime
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class LabelGLPointers extends GhidraScript {
    private int count;

    private String glString(long v) {
        Address a = toAddr(v);
        if (currentProgram.getMemory().getBlock(a) == null) return null;
        try {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 64; i++) {
                byte b = getByte(a.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7e) return null;
                sb.append((char) b);
            }
            String s = sb.toString();
            return s.matches("(gl|wgl|glu)[A-Z]\\w+|(Swap|Set|Get|Choose|Describe)\\w*(Buffers|PixelFormat)") ? s : null;
        } catch (Exception e) {
            return null;
        }
    }

    private void label(Address slot, String name) throws Exception {
        if (currentProgram.getMemory().getBlock(slot) == null) return;
        currentProgram.getSymbolTable().createLabel(slot, "p_" + name, SourceType.USER_DEFINED);
        count++;
    }

    @Override
    public void run() throws Exception {
        // 1) {slot, name} tables in non-executable memory
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isExecute() || !b.isInitialized()) continue;
            for (Address p = b.getStart().add(4); p.compareTo(b.getEnd().subtract(4)) < 0; p = p.add(4)) {
                String s = glString(getInt(p) & 0xffffffffL);
                if (s == null) continue;
                long slot = getInt(p.subtract(4)) & 0xffffffffL;
                if (glString(slot) == null) label(toAddr(slot), s);
            }
        }
        // 2) push "name"; ...; call <GetProcAddress, often via register>; mov [slot], eax
        for (Instruction i : currentProgram.getListing().getInstructions(true)) {
            if (!i.getMnemonicString().equals("PUSH")) continue;
            String name = null;
            for (Object o : i.getOpObjects(0))
                if (o instanceof Scalar sc) name = glString(sc.getUnsignedValue());
            if (name == null) continue;
            boolean called = false;
            Instruction n = i;
            for (int k = 0; k < 6 && (n = n.getNext()) != null; k++) {
                if (n.getFlowType().isCall()) { called = true; continue; }
                if (called && n.getMnemonicString().equals("MOV") && n.getNumOperands() == 2
                        && n.getDefaultOperandRepresentation(1).equals("EAX")) {
                    for (Reference w : n.getReferencesFrom())
                        if (w.getReferenceType().isWrite()) label(w.getToAddress(), name);
                    break;
                }
            }
        }
        println("labelled " + count + " GL pointer slots");
    }
}
