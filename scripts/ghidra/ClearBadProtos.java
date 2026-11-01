// ClearBadProtos.java — remove parameters whose type mentions a name, so the
// decompiler does not fail on a bogus by-value struct parameter.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Parameter;

public class ClearBadProtos extends GhidraScript {
    @Override
    public void run() throws Exception {
        String needle = getScriptArgs().length > 0 ? getScriptArgs()[0] : "ActorDropdownAction";
        int touched = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            boolean bad = false;
            for (Parameter p : f.getParameters()) {
                if (p.getDataType().getName().contains(needle)) { bad = true; break; }
            }
            if (!bad) continue;
            println("clearing " + f.getEntryPoint() + " " + f.getName());
            for (int i = f.getParameterCount() - 1; i >= 0; --i) {
                try { f.removeParameter(i); } catch (Exception e) { }
            }
            touched++;
        }
        println("touched " + touched);
    }
}
