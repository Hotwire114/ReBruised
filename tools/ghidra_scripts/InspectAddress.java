// @category MKDA
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class InspectAddress extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        DecompInterface decompiler=new DecompInterface();
        decompiler.openProgram(currentProgram);
        try(PrintWriter out=new PrintWriter(args[0])) {
            for(int n=1;n<args.length;n++) {
                Function function=getFunctionContaining(toAddr(Long.decode(args[n])));
                if(function==null) {out.println("NO FUNCTION "+args[n]);continue;}
                out.println("FUNCTION "+function.getName()+" "+function.getEntryPoint());
                DecompileResults result=decompiler.decompileFunction(function,30,monitor);
                if(result.decompileCompleted()) out.println(result.getDecompiledFunction().getC());
                else out.println(result.getErrorMessage());
            }
        }
        decompiler.dispose();
    }
}
