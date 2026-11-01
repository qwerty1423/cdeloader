// DecompileAt.java — print decompiled C for functions at given addresses.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Variable;

public class DecompileAt extends GhidraScript {
    private void stripSignature(Function f) {
        try { f.setCustomVariableStorage(false); } catch (Exception e) { }
        try { f.setCallingConvention("__cdecl"); } catch (Exception e) { }
        for (int i = f.getParameterCount() - 1; i >= 0; --i) {
            try { f.removeParameter(i); } catch (Exception e) { }
        }
        try {
            for (Variable v : f.getLocalVariables()) {
                try { f.removeVariable(v); } catch (Exception e) { }
            }
        } catch (Exception e) { }
        try { f.setVarArgs(false); } catch (Exception e) { }
    }

    private boolean tryDecompile(DecompInterface ifc, Function f) {
        DecompileResults res = ifc.decompileFunction(f, 120, monitor);
        if (res != null && res.decompileCompleted()) {
            println(res.getDecompiledFunction().getC());
            return true;
        }
        println("!! " + (res == null ? "null" : res.getErrorMessage()));
        return false;
    }

    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.toggleCCode(true);
        ifc.toggleSyntaxTree(true);
        ifc.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        for (String arg : getScriptArgs()) {
            Address addr = currentProgram.getAddressFactory().getAddress(arg);
            if (addr == null) { println("!! bad address " + arg); continue; }
            Function f = getFunctionAt(addr);
            if (f == null) f = getFunctionContaining(addr);
            if (f == null) { println("!! no function at " + arg); continue; }
            Address entry = f.getEntryPoint();
            println("===== " + arg + " " + f.getName() + " @ " + entry + " =====");
            stripSignature(f);
            if (tryDecompile(ifc, f)) continue;

            // The PDB-applied signature may be unusable.  Recreate the function
            // from the existing disassembly with a plain cdecl signature.
            println("-- recreating function from disassembly --");
            String old = f.getName();
            try { fm.removeFunction(entry); } catch (Exception e) { println("remove failed " + e); }
            Function nf = null;
            try { nf = createFunction(entry, "decomp_" + old); } catch (Exception e) { println("create failed " + e); }
            if (nf == null) nf = getFunctionAt(entry);
            if (nf == null) { println("!! no replacement function"); continue; }
            stripSignature(nf);
            tryDecompile(ifc, nf);
        }
        ifc.dispose();
    }
}
