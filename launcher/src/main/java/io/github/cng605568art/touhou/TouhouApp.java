package io.github.cng605568art.touhou;

import android.app.Application;

import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * Touhou Launcher's application. Each game plays in a process of its own (":th06", ":th07",
 * ":th08", see AndroidManifest.xml); there it records Java crashes in that game's own logs
 * folder, which the game's launcher screen offers to save, as the standalone apps do.
 */
public class TouhouApp extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        String process = processName();
        if (process.endsWith(":th06")) {
            io.github.cng605568art.th06.GameStorage.installCrashLog(this);
        } else if (process.endsWith(":th07")) {
            io.github.cng605568art.th07.GameStorage.installCrashLog(this);
        } else if (process.endsWith(":th08")) {
            io.github.cng605568art.th08.GameStorage.installCrashLog(this);
        }
    }

    /** This process's name ("<package>" or "<package>:th07"), from /proc (any API level). */
    static String processName() {
        byte[] buf = new byte[256];
        try (FileInputStream in = new FileInputStream("/proc/self/cmdline")) {
            int n = in.read(buf);
            int end = 0;
            while (end < Math.max(n, 0) && buf[end] != 0) end++;
            return new String(buf, 0, end, StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }
}
