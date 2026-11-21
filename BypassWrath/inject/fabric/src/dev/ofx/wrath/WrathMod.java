package dev.ofx.wrath;

import net.fabricmc.api.ClientModInitializer;
import java.io.File;

/**
 * Loads the native payload from inside the game process.
 *
 * No external loader EXE, no double click, no second process: the JVM calls
 * System.load() itself during client startup. DllMain in the DLL does the
 * rest, so by the time the world renders the payload is already live.
 */
public final class WrathMod implements ClientModInitializer {
    private static final String DLL = "bypass_payload.dll";

    @Override
    public void onInitializeClient() {
        String base = System.getProperty("user.dir", ".");
        File[] candidates = {
            new File(base, "natives" + File.separator + DLL),
            new File(base, DLL),
            new File(System.getProperty("java.io.tmpdir", "."), DLL)
        };

        for (File candidate : candidates) {
            if (candidate.isFile()) {
                System.load(candidate.getAbsolutePath());
                return;
            }
        }
    }
}
