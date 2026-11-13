package io.github.cng605568art.th06;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.Configuration;

import java.io.File;

/**
 * Where TH06 keeps its files. In the standalone app: the app's own storage, as always. Inside
 * Touhou Launcher (one app holding TH06, TH07 and TH08): a "th06" folder of the launcher's
 * storage, so the games' files (the chosen game folder, score.dat, replays, the translation, logs) never
 * mix. The launcher app says so with the resource bool th06_in_touhou_launcher.
 *
 * Every activity of the game wraps its base context with {@link #wrap}, and the wrapper
 * hands out wrapped application and configuration contexts, so code that saves through any
 * of them (background imports, the crash recorder, SDL's storage path for the native engine)
 * lands in the same folder.
 */
public final class GameStorage extends ContextWrapper {
    /** The standalone app's id: the game's identity in backups, whichever app made them. */
    public static final String APP_ID = "io.github.cng605568art.th06";
    private static final String FOLDER = "th06";

    private GameStorage(Context base) {
        super(base);
    }

    /** Running inside Touhou Launcher rather than as the standalone app. */
    public static boolean inLauncher(Context ctx) {
        return ctx.getResources().getBoolean(R.bool.th06_in_touhou_launcher);
    }

    /** The context the game's code should use (unchanged in the standalone app). */
    public static Context wrap(Context base) {
        if (base == null || base instanceof GameStorage || !inLauncher(base)) return base;
        return new GameStorage(base);
    }

    /** For the launcher's picker: a game folder was chosen and its data files found. */
    public static boolean ready(Context ctx) {
        return !DataSources.load(wrap(ctx)).isEmpty();
    }

    /** For the launcher app's process start: records Java crashes in this game's logs. */
    public static void installCrashLog(Context app) {
        CrashReports.install(wrap(app));
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
