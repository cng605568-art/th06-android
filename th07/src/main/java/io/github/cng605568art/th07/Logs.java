package io.github.cng605568art.th07;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.RandomAccessFile;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Logs for bug reports, in the app's storage under logs/: the engine's session log
 * (session.log, and previous.log from the run before), its native crash report (crash.txt)
 * and Java crashes (java-crash.txt, written here). The launcher saves them as one text file
 * the player can attach to a report, and shows a banner after a crash until the log is saved
 * or the banner dismissed; nothing is ever sent anywhere by the app.
 */
final class Logs {
    private static final int MAX_SECTION = 256 * 1024;  // bytes per log in a report (its end)
    private static final String PREFS = "th07_logs";
    private static final String KEY_SEEN = "crash_seen";  // the newest crash the player has seen

    private Logs() {}

    static File dir(Context ctx) {
        return new File(ctx.getFilesDir(), "logs");
    }

    private static File[] crashFiles(Context ctx) {
        File d = dir(ctx);
        return new File[] {new File(d, "crash.txt"), new File(d, "java-crash.txt")};
    }

    /** A crash newer than the last one the player saved or dismissed. */
    static boolean hasUnseenCrash(Context ctx) {
        long seen = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getLong(KEY_SEEN, 0);
        for (File f : crashFiles(ctx)) {
            if (f.isFile() && f.lastModified() > seen) return true;
        }
        return false;
    }

    static void markSeen(Context ctx) {
        long newest = System.currentTimeMillis();
        for (File f : crashFiles(ctx)) newest = Math.max(newest, f.lastModified());
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putLong(KEY_SEEN, newest).apply();
    }

    /** Records uncaught Java exceptions (launcher or game process) in logs/java-crash.txt. */
    static void install(Context ctx) {
        final Context app = ctx.getApplicationContext();
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try {
                File d = dir(app);
                //noinspection ResultOfMethodCallIgnored
                d.mkdirs();
                StringWriter text = new StringWriter();
                PrintWriter out = new PrintWriter(text);
                out.println("th07 Java crash at " + now());
                out.println("thread: " + thread.getName());
                error.printStackTrace(out);
                out.flush();
                try (OutputStream o = new FileOutputStream(new File(d, "java-crash.txt"))) {
                    o.write(text.toString().getBytes(StandardCharsets.UTF_8));
                }
            } catch (Throwable ignored) {
                // Nothing more can be done while crashing.
            }
            if (previous != null) previous.uncaughtException(thread, error);
        });
    }

    private static String now() {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new Date());
    }

    static String versionName(Context ctx) {
        try {
            PackageInfo p = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            return p.versionName != null ? p.versionName : "?";
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }

    @SuppressWarnings("deprecation")
    static long versionCode(Context ctx) {
        try {
            PackageInfo p = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            return Build.VERSION.SDK_INT >= 28 ? p.getLongVersionCode() : p.versionCode;
        } catch (PackageManager.NameNotFoundException e) {
            return 0;
        }
    }

    /** Everything a bug report needs, as one text: versions, device, files, crashes, logs. */
    static String buildReport(Context ctx) {
        StringBuilder sb = new StringBuilder();
        sb.append("TH07 Android port - log for a bug report\n");
        sb.append("saved:    ").append(now()).append('\n');
        sb.append("app:      ").append(versionName(ctx)).append(" (build ").append(versionCode(ctx)).append(")\n");
        sb.append("android:  ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        sb.append("device:   ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        sb.append("abis:     ").append(GameFiles.join(java.util.Arrays.asList(Build.SUPPORTED_ABIS))).append('\n');
        for (String name : new String[] {GameFiles.DAT, GameFiles.EXE, GameFiles.BGM}) {
            GameFiles.State s = GameFiles.state(ctx, name);
            sb.append(String.format(Locale.ROOT, "%-10s%s%n", name + ":",
                    !s.present ? "not imported" : (s.valid ? "ok, " : "WRONG FILE, ") + s.size + " bytes"));
        }
        sb.append("language: ").append(Lang.effective(ctx)).append(" (chosen: ")
                .append(Lang.chosen(ctx).isEmpty() ? "phone" : Lang.chosen(ctx)).append(")\n");
        sb.append("game text: ").append(GameText.describe(ctx)).append('\n');
        sb.append("music:    ").append(MusicReplacer.replacedCount(ctx)).append(" song(s) replaced\n");
        sb.append("options:  game ").append(java.util.Arrays.toString(Options.game(ctx)))
                .append(", controls ").append(java.util.Arrays.toString(Options.controls(ctx)))
                .append(", display ").append(java.util.Arrays.toString(Options.display(ctx))).append('\n');
        File d = dir(ctx);
        String[][] sections = {
            {"crash.txt", "Native crash (the most recent)"},
            {"java-crash.txt", "Java crash (the most recent)"},
            {"session.log", "Game log (the latest run)"},
            {"previous.log", "Game log (the run before)"},
        };
        for (String[] s : sections) {
            File f = new File(d, s[0]);
            if (!f.isFile()) continue;
            sb.append("\n==== ").append(s[1]).append(" - ").append(s[0]).append(", ")
                    .append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date(f.lastModified())))
                    .append(" ====\n");
            sb.append(tail(f));
            if (sb.charAt(sb.length() - 1) != '\n') sb.append('\n');
        }
        return sb.toString();
    }

    private static String tail(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long length = r.length();
            int n = (int) Math.min(length, MAX_SECTION);
            byte[] bytes = new byte[n];
            r.seek(length - n);
            r.readFully(bytes);
            String text = new String(bytes, StandardCharsets.UTF_8);
            return length > n ? "[... " + (length - n) + " earlier bytes left out ...]\n" + text : text;
        } catch (IOException e) {
            return "(could not read: " + e.getMessage() + ")\n";
        }
    }

    static void writeReport(Context ctx, OutputStream out) throws IOException {
        out.write(buildReport(ctx).getBytes(StandardCharsets.UTF_8));
        out.flush();
    }
}
