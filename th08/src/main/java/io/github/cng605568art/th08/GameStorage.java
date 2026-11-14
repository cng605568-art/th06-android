package io.github.cng605568art.th08;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.Configuration;

import java.io.File;

/**
 * Where TH08 keeps its files. In the standalone app: the app's own storage, as always. Inside
 * Touhou Launcher (one app holding TH06, TH07 and TH08): a "th08" folder of the launcher's
 * storage, so the games' files (game files, score.dat, replays, the translation, logs) never
 * mix. The launcher app says so with the resource bool th08_in_touhou_launcher.
 *
 * Every activity of the game wraps its base context with {@link #wrap}, and the wrapper
 * hands out wrapped application and configuration contexts, so code that saves through any
 * of them (background imports, the crash recorder, SDL's storage path for the native engine)
 * lands in the same folder.
 */
public final class GameStorage extends ContextWrapper {
    /** The standalone app's id: the game's identity in backups, whichever app made them. */
    public static final String APP_ID = "io.github.cng605568art.th08";
    private static final String FOLDER = "th08";

    private GameStorage(Context base) {
        super(base);
    }

    /** Running inside Touhou Launcher rather than as the standalone app. */
    public static boolean inLauncher(Context ctx) {
        return ctx.getResources().getBoolean(R.bool.th08_in_touhou_launcher);
    }

    /** The context the game's code should use (unchanged in the standalone app). */
    public static Context wrap(Context base) {
        if (base == null || base instanceof GameStorage || !inLauncher(base)) return base;
        return new GameStorage(base);
    }

    /** For the launcher's picker: th08.dat and th08.exe are imported and right. */
    public static boolean ready(Context ctx) {
        return GameFiles.ready(wrap(ctx));
    }

    /** For the launcher app's process start: records Java crashes in this game's logs. */
    public static void installCrashLog(Context app) {
        Logs.install(wrap(app));
    }

    private static File folder(File root) {
        File f = new File(root, FOLDER);
        //noinspection ResultOfMethodCallIgnored
        f.mkdirs();
        return f;
    }

    @Override
    public File getFilesDir() {
        return folder(super.getFilesDir());
    }

    @Override
    public File getCacheDir() {
        return folder(super.getCacheDir());
    }

    @Override
    public Context getApplicationContext() {
        return wrap(super.getApplicationContext());
    }

    @Override
    public Context createConfigurationContext(Configuration overrideConfiguration) {
        return wrap(super.createConfigurationContext(overrideConfiguration));
    }
}
