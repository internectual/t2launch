// Ghidra headless script: emit decompiler pseudocode for a newline-separated
// list of callback entry addresses.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.FileReader;
import java.io.FileWriter;

public class DumpConsoleCallbacks extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException("Usage: DumpConsoleCallbacks.java <addresses.txt> <output.txt>");
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException("Could not open current program in decompiler");
        }
        FunctionManager functions = currentProgram.getFunctionManager();
        try (BufferedReader input = new BufferedReader(new FileReader(args[0]));
             BufferedWriter output = new BufferedWriter(new FileWriter(args[1]))) {
            String line;
            while ((line = input.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                Address address = toAddr(Long.parseLong(line.replaceFirst("^0x", ""), 16));
                Function function = functions.getFunctionAt(address);
                if (function == null) function = functions.getFunctionContaining(address);
                if (function == null) {
                    try {
                        function = createFunction(address, null);
                    } catch (Exception ignored) {
                        // Some callback addresses may be interior labels or data.
                    }
                }
                output.write("@@ CALLBACK " + line + "\n");
                if (function == null) {
                    output.write("// Ghidra did not identify a function at this address.\n\n");
                    continue;
                }
                output.write("// Ghidra function: " + function.getName() + " at " + function.getEntryPoint() + "\n");
                DecompileResults result = decompiler.decompileFunction(function, 45, monitor);
                if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                    output.write(result.getDecompiledFunction().getC());
                    output.write("\n\n");
                } else {
                    output.write("// Decompilation failed: " + result.getErrorMessage() + "\n\n");
                }
                output.flush();
            }
        } finally {
            decompiler.dispose();
        }
    }
}
