// Diagnostic: report symbol coverage after a Ghidra import, to verify that the
// matched PDB was applied. Reproduces F11 (notes/FINDINGS.md).
//
//   /opt/ghidra/support/analyzeHeadless <projdir> CDE \
//     -import artifacts/CDE.exe.original \
//     -scriptPath scripts/ghidra -postScript CountSymbols.java \
//     -analysisTimeoutPerFile 1800 -deleteProject
//
// Expected on this target: MANGLED > 0 and a non-null BATTLE sample.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class CountSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        long total = 0, funcs = 0, mangled = 0;
        String combat = null, battle = null;
        while (it.hasNext()) {
            Symbol s = it.next();
            String n = s.getName();
            total++;
            if (s.getSymbolType().toString().equals("Function")) {
                funcs++;
            }
            if (n.indexOf('?') >= 0 && n.indexOf("@@") >= 0) {   // MSVC-mangled C++
                mangled++;
                if (combat == null && n.indexOf("CombatUI") >= 0) combat = n;
                if (battle == null && n.indexOf("Battle") >= 0) battle = n;
            }
        }
        println("COUNTSYMBOLS_TOTAL=" + total);
        println("COUNTSYMBOLS_FUNCTIONS=" + funcs);
        println("COUNTSYMBOLS_MANGLED=" + mangled);
        println("COUNTSYMBOLS_COMBATUI=" + combat);
        println("COUNTSYMBOLS_BATTLE=" + battle);
    }
}
