// Auto-analysis misses functions reached only through pointer tables in .data (renderer
// primitive dispatch, logic-condition tables, ...) or installed at runtime via "mov [slot], imm".
// Create a function at every 16-byte-aligned .text address referenced by a 32-bit word in a
// non-executable block or by an instruction immediate, unless it is already inside a function. Run once (not -readOnly) before exporting features/decompiling.
//@category BBTime
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class CreateFunctionsFromDataPointers extends GhidraScript {
    private boolean tryCreate(long v) {
        if ((v & 0xf) != 0) return false;
        Address t;
        try { t = toAddr(v); } catch (Exception e) { return false; }
        MemoryBlock tb = currentProgram.getMemory().getBlock(t);
        if (tb == null || !tb.isExecute() || getFunctionContaining(t) != null) return false;
        if (getInstructionAt(t) == null && getDataAt(t) != null && getDataAt(t).isDefined()) return false;
        new DisassembleCommand(t, null, true).applyTo(currentProgram, monitor);
        if (getInstructionAt(t) == null) return false;
        return new CreateFunctionCmd(t).applyTo(currentProgram, monitor);
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        int created = 0;
        for (int pass = 0; pass < 3; pass++) { // new functions can expose new tables' targets
            int before = created;
            for (MemoryBlock b : mem.getBlocks()) {
                if (b.isExecute() || !b.isInitialized()) continue;
                for (Address p = b.getStart(); p.compareTo(b.getEnd().subtract(3)) < 0; p = p.add(4)) {
                    if (tryCreate(mem.getInt(p) & 0xffffffffL)) created++;
                }
            }
            java.util.List<Long> imms = new java.util.ArrayList<>();
            for (Instruction ins : currentProgram.getListing().getInstructions(true))
                for (int i = 0; i < ins.getNumOperands(); i++)
                    for (Object o : ins.getOpObjects(i))
                        if (o instanceof Scalar sc) imms.add(sc.getUnsignedValue());
            for (long v : imms) if (tryCreate(v)) created++;
            if (created == before) break;
        }
        println("created " + created + " functions from data pointers");
    }
}
