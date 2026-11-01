// DecompileRange.java — fast decompile of known function ranges in an
// unanalyzed import.  Args: path to a file of lines "start end" (hex).
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import java.io.BufferedReader;
import java.io.FileReader;

public class DecompileRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) { println("!! need a ranges file"); return; }
        DecompInterface ifc = new DecompInterface();
        ifc.toggleCCode(true);
        ifc.toggleSyntaxTree(true);
        ifc.openProgram(currentProgram);
        BufferedReader br = new BufferedReader(new FileReader(args[0]));
        String line;
        while ((line = br.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            if (p.length < 2) continue;
            Address start = currentProgram.getAddressFactory().getAddress(p[0]);
            Address end = currentProgram.getAddressFactory().getAddress(p[1]);
            if (start == null || end == null) { println("!! bad range " + line); continue; }
            AddressSet one = new AddressSet(start, end);
            DisassembleCommand dc = new DisassembleCommand(start, one, true);
            dc.applyTo(currentProgram, monitor);
            Function f = getFunctionAt(start);
            if (f == null) {
                try { f = createFunction(start, "fn_" + p[0].replace("0x", "")); }
                catch (Exception e) { println("create failed " + e); }
            }
            if (f == null) f = getFunctionAt(start);
            println("===== " + p[0] + " " + (f == null ? "?" : f.getName()) + " =====");
            if (f == null) continue;
            DecompileResults res = ifc.decompileFunction(f, 120, monitor);
            if (res == null || !res.decompileCompleted()) {
                println("!! decompile failed: " + (res == null ? "null" : res.getErrorMessage()));
                continue;
            }
            println(res.getDecompiledFunction().getC());
        }
        br.close();
        ifc.dispose();
    }
}
