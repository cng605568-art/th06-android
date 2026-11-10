package io.github.cng605568art.th08;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Pattern;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Backup and restore of everything the port keeps for the player, as one ZIP file they keep
 * wherever they like: the high scores and play records (score.dat), the game's options
 * (th08.cfg), replays, the launcher's settings and, optionally, their replaced music
 * (music/th08_NN.wav and .pos). The game's own files (th08.dat, th08.exe, thbgm.dat, the
 * imported English patch) and downloaded game text are left out: the player imports or
 * downloads those again.
 */
final class Backup {
    static final String MANIFEST = "th08-backup.json";
    private static final String SETTINGS = "settings.json";
    private static final int FORMAT = 1;
    /** Launcher settings that travel with a backup. */
    private static final String[] PREF_FILES = {"th08_options"};
    private static final long MAX_ENTRY = 64L << 20;
    private static final long MAX_MUSIC_ENTRY = 512L << 20;  // a long song as WAV
    private static final int MAX_REPLAYS = 2000;
    // Only these paths are ever written by a restore (no "..", no other folders).
    private static final Pattern REPLAY = Pattern.compile("[A-Za-z0-9_\\-]{1,40}\\.rpy");
    private static final Pattern MUSIC = Pattern.compile("th08_(0[1-9]|1[0-9]|20)\\.(wav|pos)");

    // A backup or restore runs on its own thread; the launcher keeps imports out meanwhile.
    private static final AtomicBoolean BUSY = new AtomicBoolean();

    interface Progress {
        void update(String message);
    }

    private Backup() {}

    static boolean begin() {
        return BUSY.compareAndSet(false, true);
    }

    static void end() {
        BUSY.set(false);
    }

    static boolean busy() {
        return BUSY.get();
    }

    static String suggestedName() {
        return "th08-backup-" + new SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).format(new Date()) + ".zip";
    }

    /** Writes the backup (with the replaced music if `includeMusic`); returns a one-line summary of what it holds. */
    static String create(Context ctx, OutputStream target, boolean includeMusic, Progress progress) throws IOException {
        File files = ctx.getFilesDir();
        List<String> parts = new ArrayList<>();
        try (ZipOutputStream zip = new ZipOutputStream(target)) {
            zip.setLevel(Deflater.BEST_SPEED);
            JSONObject manifest = new JSONObject();
            try {
                manifest.put("app", ctx.getPackageName());
                manifest.put("format", FORMAT);
                manifest.put("version", Logs.versionName(ctx));
                manifest.put("created", new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ssZ", Locale.ROOT).format(new Date()));
            } catch (JSONException e) {
                throw new IOException(e);
            }
            putBytes(zip, MANIFEST, manifest.toString().getBytes(StandardCharsets.UTF_8));
            if (putFile(zip, "files/score.dat", new File(files, "score.dat"))) parts.add(ctx.getString(R.string.backup_part_scores));
            if (putFile(zip, "files/th08.cfg", new File(files, "th08.cfg"))) parts.add(ctx.getString(R.string.backup_part_options));
            int replays = 0;
            File[] rpy = new File(files, "replay").listFiles();
            if (rpy != null) {
                for (File f : rpy) {
                    if (REPLAY.matcher(f.getName()).matches() && putFile(zip, "files/replay/" + f.getName(), f)) ++replays;
                }
            }
            if (replays > 0) parts.add(ctx.getResources().getQuantityString(R.plurals.backup_part_replays, replays, replays));
            putBytes(zip, SETTINGS, exportSettings(ctx).getBytes(StandardCharsets.UTF_8));
            parts.add(ctx.getString(R.string.backup_part_settings));
            if (includeMusic) {
                int songs = 0;
                File[] music = MusicReplacer.dir(ctx).listFiles();
                if (music != null) {
                    for (File f : music) {
                        if (!MUSIC.matcher(f.getName()).matches()) continue;
                        if (f.getName().endsWith(".wav")) {
                            ++songs;
                            if (progress != null) progress.update(ctx.getString(R.string.backup_saving_music, f.getName()));
                        }
                        putFile(zip, "files/music/" + f.getName(), f);
                    }
                }
                if (songs > 0) parts.add(ctx.getResources().getQuantityString(R.plurals.backup_part_songs, songs, songs));
            }
            zip.finish();
        }
        return String.join(ctx.getString(R.string.list_separator), parts);
    }

    /** Restores a backup over the current data; returns a one-line summary. */
    static String restore(Context ctx, InputStream source, Progress progress) throws IOException {
        File files = ctx.getFilesDir();
        File staging = new File(ctx.getCacheDir(), "restore");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("cannot prepare a temporary folder");
        JSONObject manifest = null;
        String settings = null;
        int entries = 0;
        boolean hasMusic = false;
        try {
            try (ZipInputStream zip = new ZipInputStream(source)) {
                for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                    if (e.isDirectory()) continue;
                    String name = e.getName();
                    if (name.equals(MANIFEST)) {
                        manifest = parse(ctx, new String(readLimited(ctx, zip, 1 << 16), StandardCharsets.UTF_8));
                    } else if (name.equals(SETTINGS)) {
                        settings = new String(readLimited(ctx, zip, 1 << 20), StandardCharsets.UTF_8);
                    } else if (allowed(name)) {
                        // (2 records, the replays, 2 files per replaced song)
                        if (++entries > MAX_REPLAYS + 2 + 2 * MusicReplacer.TRACK_COUNT) {
                            throw new IOException(ctx.getString(R.string.backup_err_damaged));
                        }
                        boolean music = name.startsWith("files/music/");
                        if (progress != null && name.startsWith("files/replay/")) {
                            progress.update(ctx.getString(R.string.backup_reading, name.substring(13)));
                        } else if (music) {
                            hasMusic = true;
                            if (progress != null && name.endsWith(".wav")) progress.update(ctx.getString(R.string.backup_reading_music, name.substring(12)));
                        }
                        File out = new File(staging, name);
                        File parent = out.getParentFile();
                        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot write " + name);
                        try (OutputStream o = new FileOutputStream(out)) {
                            copyLimited(ctx, zip, o, music ? MAX_MUSIC_ENTRY : MAX_ENTRY);
                        }
                    }
                    // Anything else is ignored.
                }
            }
            if (manifest == null || !ctx.getPackageName().equals(manifest.optString("app"))) {
                throw new IOException(ctx.getString(R.string.backup_err_not_backup));
            }
            if (manifest.optInt("format", 0) > FORMAT) {
                throw new IOException(ctx.getString(R.string.backup_err_newer));
            }

            // Everything was read: now replace.
            List<String> parts = new ArrayList<>();
            if (moveInto(new File(staging, "files/score.dat"), new File(files, "score.dat"))) parts.add(ctx.getString(R.string.backup_part_scores));
            if (moveInto(new File(staging, "files/th08.cfg"), new File(files, "th08.cfg"))) parts.add(ctx.getString(R.string.backup_part_options));
            File[] replays = new File(staging, "files/replay").listFiles();
            if (replays != null && replays.length > 0) {
                File dir = new File(files, "replay");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot write the replays");
                for (File f : replays) moveInto(f, new File(dir, f.getName()));
                parts.add(ctx.getResources().getQuantityString(R.plurals.backup_part_replays, replays.length, replays.length));
            }
            if (hasMusic) {
                // The backup's replaced music replaces the current set.
                File dir = MusicReplacer.dir(ctx);
                File[] old = dir.listFiles();
                if (old != null) for (File f : old) if (MUSIC.matcher(f.getName()).matches()) deleteTree(f);
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot write the music");
                int songs = 0;
                File[] music = new File(staging, "files/music").listFiles();
                if (music != null) {
                    for (File f : music) {
                        if (f.getName().endsWith(".wav")) ++songs;
                        moveInto(f, new File(dir, f.getName()));
                    }
                }
                parts.add(ctx.getResources().getQuantityString(R.plurals.backup_part_songs, songs, songs));
            }
            if (settings != null) {
                importSettings(ctx, settings);
                parts.add(ctx.getString(R.string.backup_part_settings));
            }
            return parts.isEmpty() ? ctx.getString(R.string.backup_empty) : String.join(ctx.getString(R.string.list_separator), parts);
        } finally {
            deleteTree(staging);
        }
    }

    private static boolean allowed(String name) {
        if (name.equals("files/score.dat") || name.equals("files/th08.cfg")) return true;
        if (name.startsWith("files/replay/")) return REPLAY.matcher(name.substring(13)).matches();
        if (name.startsWith("files/music/")) return MUSIC.matcher(name.substring(12)).matches();
        return false;
    }

    // ---- settings ---------------------------------------------------------------------

    private static String exportSettings(Context ctx) throws IOException {
        JSONObject all = new JSONObject();
        try {
            for (String file : PREF_FILES) {
                JSONObject values = new JSONObject();
                for (Map.Entry<String, ?> e : ctx.getSharedPreferences(file, Context.MODE_PRIVATE).getAll().entrySet()) {
                    Object v = e.getValue();
                    String type = v instanceof Boolean ? "bool" : v instanceof Integer ? "int" : v instanceof Long ? "long"
                            : v instanceof Float ? "float" : v instanceof String ? "string" : null;
                    if (type == null) continue;  // (string sets are not used)
                    JSONObject item = new JSONObject();
                    item.put("type", type);
                    item.put("value", v);
                    values.put(e.getKey(), item);
                }
                all.put(file, values);
            }
            return all.toString(1);
        } catch (JSONException e) {
            throw new IOException(e);
        }
    }

    private static void importSettings(Context ctx, String json) throws IOException {
        JSONObject all = parse(ctx, json);
        for (String file : PREF_FILES) {
            JSONObject values = all.optJSONObject(file);
            if (values == null) continue;
            SharedPreferences.Editor edit = ctx.getSharedPreferences(file, Context.MODE_PRIVATE).edit().clear();
            for (Iterator<String> it = values.keys(); it.hasNext(); ) {
                String key = it.next();
                JSONObject item = values.optJSONObject(key);
                if (item == null) continue;
                switch (item.optString("type")) {
                    case "bool": edit.putBoolean(key, item.optBoolean("value")); break;
                    case "int": edit.putInt(key, item.optInt("value")); break;
                    case "long": edit.putLong(key, item.optLong("value")); break;
                    case "float": edit.putFloat(key, (float) item.optDouble("value")); break;
                    case "string": edit.putString(key, item.optString("value")); break;
                    default: break;
                }
            }
            edit.apply();
        }
    }

    // ---- files ------------------------------------------------------------------------

    private static JSONObject parse(Context ctx, String json) throws IOException {
        try {
            return new JSONObject(json);
        } catch (JSONException e) {
            throw new IOException(ctx.getString(R.string.backup_err_damaged) + " (" + e.getMessage() + ")");
        }
    }

    private static void putBytes(ZipOutputStream zip, String name, byte[] bytes) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(bytes);
        zip.closeEntry();
    }

    private static boolean putFile(ZipOutputStream zip, String name, File f) throws IOException {
        if (!f.isFile()) return false;
        ZipEntry e = new ZipEntry(name);
        e.setTime(f.lastModified());
        zip.putNextEntry(e);
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) zip.write(buf, 0, n);
        }
        zip.closeEntry();
        return true;
    }

    private static byte[] readLimited(Context ctx, InputStream in, int limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            if (out.size() > limit) throw new IOException(ctx.getString(R.string.backup_err_damaged));
        }
        return out.toByteArray();
    }

    private static void copyLimited(Context ctx, InputStream in, OutputStream out, long limit) throws IOException {
        byte[] buf = new byte[1 << 16];
        long total = 0;
        for (int n; (n = in.read(buf)) > 0; ) {
            total += n;
            if (total > limit) throw new IOException(ctx.getString(R.string.backup_err_damaged));
            out.write(buf, 0, n);
        }
    }

    private static boolean moveInto(File from, File to) throws IOException {
        if (!from.isFile()) return false;
        if (to.exists() && !to.delete()) throw new IOException("cannot replace " + to.getName());
        if (from.renameTo(to)) return true;
        // (another file system: copy)
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        }
        //noinspection ResultOfMethodCallIgnored
        from.delete();
        return true;
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
