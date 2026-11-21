// Disassembly listing of the functions containing the given addresses (or a raw range "0xA-0xB").
// @category BNB
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ListAddress extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int n = 1; n < args.length; n++) {
                AddressSetView body;
                String a = args[n];
                if (a.contains("-")) {
                    String[] p = a.split("-");
                    body = new AddressSet(toAddr(Long.decode(p[0])), toAddr(Long.decode(p[1])));
                    out.println("RANGE " + a);
                } else {
                    Function f = getFunctionContaining(toAddr(Long.decode(a)));
                    if (f == null) { out.println("NO FUNCTION " + a); continue; }
                    out.println("FUNCTION " + f.getName() + " " + f.getEntryPoint());
                    body = f.getBody();
                }
                InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
                while (it.hasNext()) {
                    Instruction i = it.next();
                    Symbol s = getSymbolAt(i.getAddress());
                    if (s != null) out.println(s.getName() + ":");
                    out.println("  " + i.getAddress() + "  " + i);
                }
            }
        }
    }
}
