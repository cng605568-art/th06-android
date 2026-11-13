package io.github.cng605568art.th06;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.CRC32;

/**
 * The game's own text (story, spell cards, music titles and comments, endings) in another
 * language: the Touhou Patch Center (thpatch.net) community translations, downloaded on the
 * player's request into the app's storage and applied by the engine. Nothing is bundled with
 * the app. Only the th06 text files of a patch (and its fallback patches) are fetched.
 */
final class GameText {
    /** Game-text choices: follow the app's language, the original, or a language. */
    static final String[] CODES = {"auto", "ja", "en", "zh-CN", "zh-TW", "ms"};
    private static final String PREFS = "th06_game_options";  // (backed up with the settings)
    private static final String KEY = "game_text";
    private static final String[] REPOS = {"https://srv.thpatch.net/", "https://thcrap.nmlgc.net/repos/thpatch/"};
    static final String CREDIT_URL = "https://www.thpatch.net/";

    interface Progress {
        void update(String message);
    }

    /** What is installed for a patch. */
    static final class Status {
        final boolean installed;
        final int files, dialogue;
        final long updated;

        Status(boolean installed, int files, int dialogue, long updated) {
            this.installed = installed;
            this.files = files;
            this.dialogue = dialogue;
            this.updated = updated;
        }
    }

    private GameText() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static int chosenIndex(Context ctx) {
        String c = prefs(ctx).getString(KEY, "auto");
        for (int i = 0; i < CODES.length; ++i) if (CODES[i].equals(c)) return i;
        return 0;
    }

    static void choose(Context ctx, int index) {
        prefs(ctx).edit().putString(KEY, CODES[Math.max(0, Math.min(CODES.length - 1, index))]).apply();
    }

    /** The language the game's text should be in ("ja" = the original). */
    static String effective(Context ctx) {
        String c = CODES[chosenIndex(ctx)];
        return c.equals("auto") ? Lang.effective(ctx) : c;
    }

    /** The thpatch patch for a language, or null for the original Japanese. */
    static String patchFor(String code) {
        switch (code) {
            case "en": return "lang_en";
            case "zh-CN": return "lang_zh-hans";
            case "zh-TW": return "lang_zh-hant";
            case "ms": return "lang_ms";
            default: return null;
        }
    }

    static Locale textLocale(Context ctx) {
        if (ctx == null) return Locale.JAPANESE;
        switch (effective(ctx)) {
            case "en": return Locale.ENGLISH;
            case "zh-CN": return Locale.SIMPLIFIED_CHINESE;
            case "zh-TW": return Locale.TRADITIONAL_CHINESE;
            case "ms": return new Locale("ms", "MY");
            default: return Locale.JAPANESE;
        }
    }

    private static File root(Context ctx) {
        return new File(ctx.getFilesDir(), "translation");
    }

    private static File patchDir(Context ctx, String id) {
        return new File(root(ctx), id.replaceAll("[^A-Za-z0-9_.-]", "_"));
    }

    private static File stackFile(Context ctx, String id) {
        return new File(root(ctx), id + ".stack");
    }

    /** The engine's patch folders for the chosen language, lowest priority first (none: original). */
    static String[] stack(Context ctx) {
        String id = patchFor(effective(ctx));
        if (id == null) return new String[0];
        List<String> out = new ArrayList<>();
        for (String p : readStack(ctx, id)) {
            File d = patchDir(ctx, p);
            if (d.isDirectory()) out.add(d.getAbsolutePath() + "/");
        }
        return out.toArray(new String[0]);
    }

    private static List<String> readStack(Context ctx, String id) {
        List<String> ids = new ArrayList<>();
        File f = stackFile(ctx, id);
        if (!f.isFile()) return ids;
        try {
            for (String line : new String(readAll(f), StandardCharsets.UTF_8).split("\n")) {
                if (!line.trim().isEmpty()) ids.add(line.trim());
            }
        } catch (IOException ignored) {
            // Treated as not installed.
        }
        return ids;
    }

    static Status status(Context ctx, String code) {
        String id = patchFor(code);
        if (id == null) return new Status(false, 0, 0, 0);
        List<String> ids = readStack(ctx, id);
        if (ids.isEmpty()) return new Status(false, 0, 0, 0);
        int files = 0;
        Set<String> dialogue = new LinkedHashSet<>();
        for (String p : ids) {
            File th06 = new File(patchDir(ctx, p), "th06");
            File[] list = th06.listFiles();
            if (list == null) continue;
            for (File f : list) {
                ++files;
                if (f.getName().matches("msg\\d\\.(v1\\.02h\\.)?dat\\.jdiff")) dialogue.add(f.getName().substring(0, 4));
            }
        }
        return new Status(true, files, dialogue.size(), stackFile(ctx, id).lastModified());
    }

    static void remove(Context ctx, String code) {
        String id = patchFor(code);
        if (id == null) return;
        for (String p : readStack(ctx, id)) deleteTree(patchDir(ctx, p));
        //noinspection ResultOfMethodCallIgnored
        stackFile(ctx, id).delete();
    }

    // ---- downloading ------------------------------------------------------------------------

    /** Thrown when the patch server has no translation for the language. */
    static final class NotAvailable extends IOException {
        private static final long serialVersionUID = 1L;

        NotAvailable() {
            super("no translation");
        }
    }

    /**
     * Downloads (or updates) the language's patch and its fallback patches from thpatch.
     * Returns the number of files fetched. Blocking: call off the UI thread.
     */
    static int download(Context ctx, String code, Progress progress) throws IOException {
        String id = patchFor(code);
        if (id == null) throw new NotAvailable();
        progress.update(ctx.getString(R.string.th06_gametext_connecting));
        JSONObject repo = null;
        String repoBase = null;
        IOException last = null;
        for (String base : REPOS) {
            try {
                repo = new JSONObject(fetchText(ctx, base + "repo.js"));
                repoBase = base;
                break;
            } catch (IOException e) {
                last = e;
            } catch (JSONException e) {
                last = new IOException("the patch list could not be read");
            }
        }
        if (repo == null) throw last != null ? last : new IOException("no server");
        JSONObject patches = repo.optJSONObject("patches");
        if (patches == null || !patches.has(id)) throw new NotAvailable();
        List<String> servers = new ArrayList<>();
        JSONArray s = repo.optJSONArray("servers");
        if (s != null) for (int i = 0; i < s.length(); ++i) servers.add(withSlash(s.optString(i)));
        if (!servers.contains(repoBase)) servers.add(repoBase);

        // The stack: the patch after its dependencies (thpatch's own; the engine has the
        // base patches' defaults built in).
        List<String> stack = new ArrayList<>();
        resolve(ctx, id, patches, servers, stack, new LinkedHashSet<>());
        int fetched = 0;
        for (String p : stack) fetched += syncPatch(ctx, p, servers, progress);
        File root = root(ctx);
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot write the translation");
        writeAtomically(stackFile(ctx, id), String.join("\n", stack).getBytes(StandardCharsets.UTF_8));
        return fetched;
    }

    private static void resolve(Context ctx, String id, JSONObject patches, List<String> servers, List<String> stack,
                                Set<String> visiting) throws IOException {
        if (stack.contains(id) || !visiting.add(id)) return;
        JSONObject patch = fetchJson(ctx, servers, id + "/patch.js");
        JSONArray deps = patch.optJSONArray("dependencies");
        if (deps != null) {
            for (int i = 0; i < deps.length(); ++i) {
                String d = deps.optString(i);
                if (d.contains("/")) {
                    if (!d.startsWith("thpatch/")) continue;  // another repository (base patches)
                    d = d.substring("thpatch/".length());
                }
                if (patches.has(d)) resolve(ctx, d, patches, servers, stack, visiting);
            }
        }
        stack.add(id);
    }

    /** Brings one patch's th06 text files up to date; returns how many were downloaded. */
    private static int syncPatch(Context ctx, String id, List<String> servers, Progress progress) throws IOException {
        JSONObject remote = fetchJson(ctx, servers, id + "/files.js");
        File dir = patchDir(ctx, id);
        File localList = new File(dir, ".files.json");
        JSONObject local;
        try {
            local = localList.isFile() ? new JSONObject(new String(readAll(localList), StandardCharsets.UTF_8)) : new JSONObject();
        } catch (JSONException e) {
            local = new JSONObject();
        }
        int fetched = 0;
        List<String> wanted = new ArrayList<>();
        for (Iterator<String> it = remote.keys(); it.hasNext(); ) {
            String path = it.next();
            if (wantedFile(path)) wanted.add(path);
        }
        int n = 0;
        for (String path : wanted) {
            ++n;
            File target = new File(dir, path);
            if (remote.isNull(path)) {  // removed from the patch
                //noinspection ResultOfMethodCallIgnored
                target.delete();
                local.remove(path);
                continue;
            }
            long crc = remote.optLong(path, -1);
            if (crc >= 0 && local.optLong(path, -2) == crc && target.isFile()) continue;
            progress.update(ctx.getString(R.string.th06_gametext_downloading, id, n, wanted.size()));
            byte[] bytes = fetchFile(ctx, servers, id + "/" + escape(path) + String.format(Locale.ROOT, "?crc32=%08x", crc));
            CRC32 check = new CRC32();
            check.update(bytes);
            if (crc >= 0 && check.getValue() != crc) throw new IOException("a file was damaged in transfer (" + path + ")");
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot write " + path);
            writeAtomically(target, bytes);
            try {
                local.put(path, crc);
            } catch (JSONException e) {
                throw new IOException(e);
            }
            ++fetched;
        }
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot write the translation");
        writeAtomically(localList, local.toString().getBytes(StandardCharsets.UTF_8));
        return fetched;
    }

    /** The th06 text files (and the shared music titles and strings); no images or exe data. */
    static boolean wantedFile(String path) {
        if (!path.contains("/")) return path.equals("stringdefs.js") || path.equals("themes.js");
        if (!path.startsWith("th06/")) return false;
        String name = path.substring(5);
        if (name.contains("/")) return false;
        return name.endsWith(".jdiff") || name.equals("spells.js") || name.equals("stages.js") || name.equals("musiccmt.js");
    }

    // ---- HTTP ---------------------------------------------------------------------------------

    private static String withSlash(String s) {
        return s.endsWith("/") ? s : s + "/";
    }

    private static JSONObject fetchJson(Context ctx, List<String> servers, String path) throws IOException {
        byte[] b = fetchFile(ctx, servers, path);
        try {
            return new JSONObject(new String(b, StandardCharsets.UTF_8));
        } catch (JSONException e) {
            throw new IOException("unexpected answer from the patch server (" + path + ")");
        }
    }

    private static byte[] fetchFile(Context ctx, List<String> servers, String path) throws IOException {
        IOException last = null;
        for (String base : servers) {
            try {
                return fetch(ctx, base + path);
            } catch (IOException e) {
                last = e;
            }
        }
        throw last != null ? last : new IOException("no server");
    }

    private static String fetchText(Context ctx, String url) throws IOException {
        return new String(fetch(ctx, url), StandardCharsets.UTF_8);
    }

    private static byte[] fetch(Context ctx, String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            c.setInstanceFollowRedirects(true);
            c.setRequestProperty("User-Agent", "th06-android/" + CrashReports.versionName(ctx)
                    + " (+https://github.com/" + UpdateChecker.REPO + ")");
            int code = c.getResponseCode();
            if (code != 200) throw new IOException("the server answered " + code + " for " + url);
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    if (out.size() > (16 << 20)) throw new IOException("a file is too large");
                }
                return out.toByteArray();
            }
        } finally {
            c.disconnect();
        }
    }

    /** Percent-escapes everything but A-Z a-z 0-9 - _ . ~ / (as thcrap does). */
    static String escape(String path) {
        StringBuilder sb = new StringBuilder();
        for (byte b : path.getBytes(StandardCharsets.UTF_8)) {
            int c = b & 0xff;
            boolean plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
            if (plain) sb.append((char) c);
            else sb.append('%').append(String.format(Locale.ROOT, "%02x", c));
        }
        return sb.toString();
    }

    // ---- files --------------------------------------------------------------------------------

    private static byte[] readAll(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    private static void writeAtomically(File target, byte[] bytes) throws IOException {
        File tmp = new File(target.getPath() + ".part");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(bytes);
        }
        if (!tmp.renameTo(target)) {
            //noinspection ResultOfMethodCallIgnored
            target.delete();
            if (!tmp.renameTo(target)) throw new IOException("cannot write " + target.getName());
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
