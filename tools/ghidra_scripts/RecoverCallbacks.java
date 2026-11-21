// Recover methods referenced by the executable's initialized data tables.
// @category MKDA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import java.util.*;

public class RecoverCallbacks extends GhidraScript {
    public void run() throws Exception {
        TreeSet<Long> targets=new TreeSet<>();
        for(String target:getScriptArgs()) targets.add(Long.decode(target));
        for(long at=0x328800;at<0x435c80;at+=4) {
            long target=Integer.toUnsignedLong(currentProgram.getMemory().getInt(toAddr(at)));
            if(target<0x100000 || target>=0x328800 || (target&3)!=0) continue;
            Address address=toAddr(target);
            if(getInstructionAt(address)!=null || getFunctionContaining(address)!=null) continue;
            targets.add(target);
        }
        // Static initializers also construct callback addresses with LUI plus
        // ADDIU/ORI instead of storing complete pointers in a data table.
        for(long at=0x100000;at<0x3287d4;at+=4) {
            int high=currentProgram.getMemory().getInt(toAddr(at));
            if((high>>>26)!=15) continue;
            int register=(high>>>16)&31;
            for(int distance=1;distance<=8 && at+distance*4<0x3287d4;distance++) {
                int low=currentProgram.getMemory().getInt(toAddr(at+distance*4));
                int operation=low>>>26;
                if(((low>>>21)&31)!=register || (operation!=9 && operation!=13)) continue;
                long value=(high&0xffffL)<<16;
                long target=operation==9 ? (value+(short)(low&0xffff))&0xffffffffL : value|(low&0xffffL);
                if(target<0x100000 || target>=0x3287d4 || (target&3)!=0) continue;
                Address address=toAddr(target);
                if(getInstructionAt(address)==null && getFunctionContaining(address)==null) targets.add(target);
            }
        }
        int created=0;
        for(long target:targets) {
            if(monitor.isCancelled()) break;
            Address address=toAddr(target);
            if(getFunctionContaining(address)!=null) continue;
            if(disassemble(address) && createFunction(address,String.format("RECOVER_%08x",target))!=null) created++;
        }
        println("Recovered "+created+" data-referenced methods from "+targets.size()+" candidates");
        analyzeChanges(currentProgram);
    }
}
