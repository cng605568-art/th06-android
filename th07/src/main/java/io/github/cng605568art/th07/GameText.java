package io.github.cng605568art.th07;

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
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Deque;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.CRC32;

/**
 * The game's own text (dialogue, spell cards, endings, music titles and comments, menus and
 * the text pictures) in another language: the Touhou Patch Center (thpatch.net) translations,
 * stacked the way the thcrap patcher stacks them. On the player's request a language's patch
 * and the patches it depends on (patch.js "dependencies", depth first: for English that is
 * nmlgc/base_tsa, nmlgc/base_tasofro, nmlgc/script_latin, nmlgc/western_name_order, then
 * thpatch/lang_en) are downloaded, only their th07 folder and the root .js files TH07 reads,
 * into files/thcrap/repos/&lt;repo&gt;/&lt;patch&gt;/ (the layout the import of a PC thcrap folder
 * uses, paths in lower case). The engine gets the stack's folders from
 * GameActivity.getGameTextStack(). Nothing is bundled with the app.
 *
 * English also works without a download: an imported thcrap folder's lang_en (and the patches
 * it depends on) is used as it is.
 *
 * Bookkeeping in files/thcrap/: &lt;patch&gt;.stack lists a downloaded language's stack
 * ("repo/patch" per line, lowest priority first); each downloaded patch folder keeps
 * .files.json (each file's CRC32, so an update fetches only what changed); imported.list
 * (GameFiles) names the patches the last import brought. A folder can serve both.
 */
final class GameText {
    /** Game-text choices: follow the launcher's language, the original, or a language. */
    static final String[] CODES = {"auto", "ja", "en", "zh-CN", "zh-TW", "ms"};
    private static final String PREFS = "th07_options";  // (backed up with the launcher settings)
    private static final String KEY = "game_text";
    private static final String KEY_MIGRATED = "game_text_migrated";
    /** The earlier launcher's "English text" checkbox (on until turned off). */
    private static final String KEY_LEGACY_ENGLISH = "english_text";

    static final String GAME = "th07";
    /** A downloaded patch folder's record of its files' CRC32s (never written by an import). */
    static final String LOCAL_FILES = ".files.json";
    private static final String STACK_SUFFIX = ".stack";
    private static final String HOME = "thpatch";
    private static final String[] HOME_SERVERS = {"https://srv.thpatch.net/", "https://thcrap.nmlgc.net/repos/thpatch/"};
    // Where a repository lives when no neighbour announces it (as the patches' own servers are).
    private static final String[] REPO_FALLBACKS = {"https://mirrors.thpatch.net/%s/", "https://thcrap.nmlgc.net/repos/%s/"};
    static final String CREDIT_URL = "https://www.thpatch.net/";
    private static final int MAX_REPO_FETCHES = 16;
    private static final int MAX_PATCHES = 24;
    private static final int MAX_FILES = 3000;          // per patch
    private static final int MAX_DEPTH = 6;             // folder levels below th07/
    private static final int MAX_FILE = 16 << 20;       // bytes per file
    private static final long MAX_TOTAL = 96L << 20;    // bytes per download

    interface Progress {
        void update(String message);
    }

    /** How a download reaches the servers (replaced on a desktop JVM to check this class). */
    interface Fetcher {
        byte[] get(String url) throws IOException;
    }

    static Fetcher fetcher;  // null: HTTPS

    private static volatile boolean downloading;

    /** What a language has installed. */
    static final class Status {
        final boolean installed;   // the game can use it
        final boolean downloaded;  // downloaded here (Remove applies)
        final int files;           // th07 files in the stack
        final int dialogue;        // stages with dialogue (msg1..msg8: 6 stages, Extra, Phantasm)
        final long updated;

        Status(boolean installed, boolean downloaded, int files, int dialogue, long updated) {
            this.installed = installed;
            this.downloaded = downloaded;
            this.files = files;
            this.dialogue = dialogue;
            this.updated = updated;
        }
    }

    private GameText() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    // ---- the choice -------------------------------------------------------------------------

    /**
     * Once: the earlier "English text" checkbox becomes this choice (on: English, off: the
     * original), and a player who had imported the English patch keeps English.
     */
    private static void migrate(Context ctx) {
        SharedPreferences p = prefs(ctx);
        if (p.getBoolean(KEY_MIGRATED, false)) return;
        SharedPreferences.Editor e = p.edit().putBoolean(KEY_MIGRATED, true).remove(KEY_LEGACY_ENGLISH);
        if (!p.contains(KEY)) {
            if (p.contains(KEY_LEGACY_ENGLISH)) e.putString(KEY, p.getBoolean(KEY_LEGACY_ENGLISH, true) ? "en" : "ja");
            else if (GameFiles.englishPresent(ctx)) e.putString(KEY, "en");
        }
        e.apply();
    }

    static int chosenIndex(Context ctx) {
        migrate(ctx);
        String c = prefs(ctx).getString(KEY, "auto");
        for (int i = 0; i < CODES.length; ++i) if (CODES[i].equals(c)) return i;
        return 0;
    }

    static void choose(Context ctx, int index) {
        migrate(ctx);
        prefs(ctx).edit().putString(KEY, CODES[Math.max(0, Math.min(CODES.length - 1, index))]).apply();
    }

    /** The language the game's text should be in ("ja" = the original). */
    static String effective(Context ctx) {
        String c = CODES[chosenIndex(ctx)];
        return c.equals("auto") ? Lang.effective(ctx) : c;
    }

    /** thpatch's patch for a language, or null for the original Japanese. */
    static String patchFor(String code) {
        switch (code) {
            case "en": return "lang_en";
            case "zh-CN": return "lang_zh-hans";
            case "zh-TW": return "lang_zh-hant";
            case "ms": return "lang_ms";
            default: return null;
        }
    }

    /** A language's name in its own words. */
    static String label(String code) {
        switch (code) {
            case "en": return "English";
            case "zh-CN": return "简体中文";
            case "zh-TW": return "繁體中文";
            case "ms": return "Bahasa Melayu";
            default: return "日本語";
        }
    }

    /** For drawing translated text: Chinese text in Chinese glyph shapes, and so on. */
    static Locale textLocale(Context ctx) {
        if (ctx == null) return Locale.JAPANESE;
        return Lang.locale(effective(ctx));
    }

    // ---- the stack --------------------------------------------------------------------------

    static File repos(Context ctx) {
        return new File(GameFiles.thcrapDir(ctx), "repos");
    }

    /** A patch's folder, by "repo/patch". */
    static File folder(Context ctx, String key) {
        return new File(repos(ctx), key);
    }

    private static File stackFile(Context ctx, String patchId) {
        return new File(GameFiles.thcrapDir(ctx), patchId + STACK_SUFFIX);
    }

    /** The engine's patch folders for the chosen language, lowest priority first (none: the original). */
    static String[] stack(Context ctx) {
        List<String> out = new ArrayList<>();
        for (String key : stackKeys(ctx, effective(ctx))) out.add(folder(ctx, key).getAbsolutePath() + "/");
        return out.toArray(new String[0]);
    }

    /**
     * A language's stack ("repo/patch", lowest priority first): its download, or for English
     * the imported lang_en and what it depends on. Empty when the language has neither.
     */
    static List<String> stackKeys(Context ctx, String code) {
        String id = patchFor(code);
        if (id == null) return Collections.emptyList();
        List<String> downloaded = existing(ctx, readStack(stackFile(ctx, id)));
        if (!downloaded.isEmpty() && downloaded.get(downloaded.size() - 1).endsWith("/" + id)) return downloaded;
        if (id.equals("lang_en") && GameFiles.englishPresent(ctx)) return discover(repos(ctx), id);
        return Collections.emptyList();
    }

    private static List<String> existing(Context ctx, List<String> keys) {
        List<String> out = new ArrayList<>();
        for (String k : keys) if (folder(ctx, k).isDirectory()) out.add(k);
        return out;
    }

    static List<String> readStack(File f) {
        List<String> keys = new ArrayList<>();
        if (!f.isFile()) return keys;
        try {
            for (String line : new String(readAll(f), StandardCharsets.UTF_8).split("\n")) {
                String k = line.trim();
                if (GameFiles.validKey(k) && !keys.contains(k)) keys.add(k);
            }
        } catch (IOException ignored) {
            // Treated as not installed.
        }
        return keys;
    }

    /** Every patch a downloaded language uses (an import keeps these). */
    static Set<String> downloadedPatches(Context ctx) {
        Set<String> out = new LinkedHashSet<>();
        File[] list = GameFiles.thcrapDir(ctx).listFiles();
        if (list == null) return out;
        for (File f : list) {
            if (f.isFile() && f.getName().endsWith(STACK_SUFFIX)) out.addAll(readStack(f));
        }
        return out;
    }

    /**
     * The stack of `patchId` from patch folders already in `repos` (an imported thcrap folder),
     * as the engine's own discovery and thcrap order it: thpatch's (else any repository's)
     * patch; each patch's dependencies first, depth first ("repo/patch", or a bare id looked
     * up in the same repository first); folders that are not there are skipped.
     */
    static List<String> discover(File repos, String patchId) {
        List<String> out = new ArrayList<>();
        File[] dirs = repos.listFiles();
        if (dirs == null) return out;
        List<String> names = new ArrayList<>();
        for (File d : dirs) if (d.isDirectory()) names.add(d.getName());
        Collections.sort(names);
        String langRepo = new File(repos, HOME + "/" + patchId).isDirectory() ? HOME : null;
        for (String r : names) {
            if (langRepo == null && new File(repos, r + "/" + patchId).isDirectory()) langRepo = r;
        }
        if (langRepo != null) addDiscovered(repos, names, langRepo, patchId, new HashSet<>(), out);
        return out;
    }

    private static void addDiscovered(File repos, List<String> names, String repo, String id, Set<String> seen, List<String> out) {
        String key = lower(repo) + "/" + lower(id);
        if (!GameFiles.validKey(key) || !seen.add(key)) return;
        File dir = new File(repos, key);
        File patchJs = new File(dir, "patch.js");
        if (patchJs.isFile()) {
            try {
                for (String dep : GameFiles.parseDependencies(new String(readAll(patchJs), StandardCharsets.UTF_8))) {
                    int slash = dep.lastIndexOf('/');
                    if (slash >= 0) {
                        addDiscovered(repos, names, dep.substring(0, slash), dep.substring(slash + 1), seen, out);
                        continue;
                    }
                    String where = repo;
                    if (!new File(repos, lower(repo) + "/" + lower(dep)).isDirectory()) {
                        for (String r : names) {
                            if (new File(repos, r + "/" + lower(dep)).isDirectory()) {
                                where = r;
                                break;
                            }
                        }
                    }
                    addDiscovered(repos, names, where, dep, seen, out);
                }
            } catch (IOException ignored) {
                // An unreadable patch.js: no dependencies.
            }
        }
        if (dir.isDirectory()) out.add(key);
    }

    static Status status(Context ctx, String code) {
        String id = patchFor(code);
        List<String> keys = stackKeys(ctx, code);
        if (id == null || keys.isEmpty()) return new Status(false, false, 0, 0, 0);
        int[] files = {0};
        Set<String> dialogue = new TreeSet<>();
        for (String k : keys) countGameFiles(new File(folder(ctx, k), GAME), 0, files, dialogue);
        File stack = stackFile(ctx, id);
        boolean downloaded = !readStack(stack).isEmpty();
        long updated = downloaded ? stack.lastModified() : folder(ctx, keys.get(keys.size() - 1)).lastModified();
        return new Status(true, downloaded, files[0], dialogue.size(), updated);
    }

    private static void countGameFiles(File dir, int depth, int[] files, Set<String> dialogue) {
        File[] list = dir.listFiles();
        if (list == null || depth > MAX_DEPTH) return;
        for (File f : list) {
            if (f.isDirectory()) {
                countGameFiles(f, depth + 1, files, dialogue);
            } else if (!f.getName().endsWith(".part")) {
                ++files[0];
                if (depth == 0 && f.getName().matches("msg[1-8]\\.(v1\\.00b\\.)?dat\\.jdiff")) dialogue.add(f.getName().substring(0, 4));
            }
        }
    }

    /** One line for a bug report: the language and its stack. */
    static String describe(Context ctx) {
        String code = effective(ctx);
        List<String> keys = stackKeys(ctx, code);
        String id = patchFor(code);
        if (id == null) return code + " (original)";
        if (keys.isEmpty()) return code + " (not installed: original)";
        return code + " (" + (readStack(stackFile(ctx, id)).isEmpty() ? "imported" : "downloaded") + ": "
                + GameFiles.join(keys) + ")";
    }

    /**
     * Removes a language's download: its stack, and the patch folders no other download and no
     * import uses. Call with the files lock held (GameFiles.beginImport()).
     */
    static void remove(Context ctx, String code) {
        String id = patchFor(code);
        if (id == null) return;
        File stack = stackFile(ctx, id);
        List<String> mine = readStack(stack);
        //noinspection ResultOfMethodCallIgnored
        stack.delete();
        Set<String> keep = new HashSet<>(downloadedPatches(ctx));
        keep.addAll(GameFiles.importedPatches(ctx));
        for (String k : mine) if (!keep.contains(k)) GameFiles.deleteTree(folder(ctx, k));
        GameFiles.pruneEmptyRepos(ctx);
    }

    // ---- downloading ------------------------------------------------------------------------

    /** Thrown when Touhou Patch Center has no TH07 text in the language. */
    static final class NotAvailable extends IOException {
        private static final long serialVersionUID = 1L;

        NotAvailable() {
            super("no translation");
        }
    }

    /**
     * Takes the files lock for a download (imports, downloads and removals of game text never
     * overlap: they all change files/thcrap). False when something else holds it.
     */
    static boolean beginDownload() {
        if (!GameFiles.beginImport()) return false;
        downloading = true;
        return true;
    }

    static void endDownload() {
        downloading = false;
        GameFiles.endImport();
    }

    /** A game-text download holds the files lock (rather than an import). */
    static boolean downloading() {
        return downloading;
    }

    /** A repository, as its repo.js describes it. */
    private static final class Repo {
        final String id;
        final List<String> servers = new ArrayList<>();
        final List<String> neighbors = new ArrayList<>();
        JSONObject patches = new JSONObject();

        Repo(String id) {
            this.id = id;
        }
    }

    /** One patch of the stack being downloaded. */
    private static final class Patch {
        final String key;           // "repo/patch", lower case
        final List<String> servers; // where its files are ("…/<patch>/")
        final byte[] patchJs;
        JSONObject remote;          // its files.js
        final List<String> wanted = new ArrayList<>();

        Patch(String key, List<String> servers, byte[] patchJs) {
            this.key = key;
            this.servers = servers;
            this.patchJs = patchJs;
        }
    }

    /** The repositories met so far, found the way thcrap discovers them (servers, then neighbours). */
    private static final class Repos {
        final Map<String, Repo> known = new LinkedHashMap<>();
        final Deque<String> toVisit = new ArrayDeque<>();
        final Set<String> visited = new HashSet<>();
        int fetches;

        Repo home() throws IOException {
            IOException last = null;
            for (String base : HOME_SERVERS) {
                try {
                    Repo r = load(base, HOME);
                    known.put(HOME, r);
                    return r;
                } catch (IOException e) {
                    last = e;
                }
            }
            throw last != null ? last : new IOException("no server");
        }

        Repo find(String id) {
            Repo r = known.get(id);
            while (r == null && !toVisit.isEmpty() && fetches < MAX_REPO_FETCHES) {
                String url = toVisit.poll();
                try {
                    Repo n = load(url, null);
                    if (!known.containsKey(n.id)) known.put(n.id, n);
                } catch (IOException ignored) {
                    // An unreachable neighbour: keep looking.
                }
                r = known.get(id);
            }
            if (r == null) {  // not announced anywhere reachable: its usual addresses
                r = new Repo(id);
                for (String f : REPO_FALLBACKS) r.servers.add(String.format(Locale.ROOT, f, id));
                known.put(id, r);
            }
            return r;
        }

        /** The repository that has patch `id`, the home one first; null if none says so. */
        String holder(String id) {
            for (Repo r : known.values()) if (r.patches.has(id)) return r.id;
            return null;
        }

        private Repo load(String base, String fallbackId) throws IOException {
            base = withSlash(base);
            visited.add(base);
            ++fetches;
            JSONObject j;
            try {
                j = new JSONObject(new String(fetch(base + "repo.js"), StandardCharsets.UTF_8));
            } catch (JSONException e) {
                throw new IOException("the patch list could not be read (" + base + ")");
            }
            String id = j.optString("id", "");
            if (id.isEmpty()) id = fallbackId != null ? fallbackId : base;
            Repo r = new Repo(lower(id));
            for (String s : strings(j.optJSONArray("servers"))) if (isHttps(s) && !r.servers.contains(withSlash(s))) r.servers.add(withSlash(s));
            if (!r.servers.contains(base)) r.servers.add(base);
            for (String s : strings(j.optJSONArray("neighbors"))) if (isHttps(s)) r.neighbors.add(withSlash(s));
            JSONObject patches = j.optJSONObject("patches");
            if (patches != null) r.patches = patches;
            for (String s : r.servers) if (!visited.contains(s)) toVisit.add(s);
            for (String s : r.neighbors) if (!visited.contains(s)) toVisit.add(s);
            return r;
        }
    }

    /**
     * Downloads (or updates) the language's patch and the patches it depends on. Returns the
     * number of files fetched. Blocking: call off the UI thread, between beginDownload() and
     * endDownload() (so no import changes files/thcrap meanwhile).
     */
    static int download(Context ctx, String code, Progress progress) throws IOException {
        String id = patchFor(code);
        if (id == null) throw new NotAvailable();
        progress.update(ctx.getString(R.string.th07_gametext_connecting));
        Repos repos = new Repos();
        Repo home = repos.home();
        if (!home.patches.has(id)) throw new NotAvailable();
        List<Patch> stack = new ArrayList<>();
        resolve(repos, HOME, id, stack, new HashSet<>());
        int total = 0;
        for (Patch p : stack) {
            p.remote = fetchJson(p.servers, "files.js");
            for (Iterator<String> it = p.remote.keys(); it.hasNext(); ) {
                String path = it.next();
                if (wantedFile(path) && p.wanted.size() < MAX_FILES) p.wanted.add(path);
            }
            Collections.sort(p.wanted);
            total += p.wanted.size();
        }
        Patch lang = stack.get(stack.size() - 1);
        boolean forGame = false;
        for (String path : lang.wanted) forGame |= lower(path).startsWith(GAME + "/") && !lang.remote.isNull(path);
        if (!forGame) throw new NotAvailable();  // the language has nothing for this game

        File root = GameFiles.thcrapDir(ctx);
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot write the game text");
        GameFiles.recordEarlierImport(ctx);  // (before any download touches an imported folder)
        int fetched = 0;
        int[] done = {0};
        long[] bytes = {0};
        StringBuilder keys = new StringBuilder();
        for (Patch p : stack) {
            fetched += sync(ctx, p, done, total, bytes, progress);
            keys.append(p.key).append('\n');
        }
        writeAtomically(stackFile(ctx, id), keys.toString().getBytes(StandardCharsets.UTF_8));
        return fetched;
    }

    /** Adds `id` of repository `repoId` to `stack`, after what it depends on. */
    private static void resolve(Repos repos, String repoId, String id, List<Patch> stack, Set<String> visiting)
            throws IOException {
        String key = lower(repoId) + "/" + lower(id);
        if (!GameFiles.validKey(key)) throw new IOException("unexpected patch name " + repoId + "/" + id);
        for (Patch p : stack) if (p.key.equals(key)) return;
        if (!visiting.add(key)) return;  // (a cycle)
        if (stack.size() >= MAX_PATCHES) throw new IOException("too many patches");
        Repo repo = repos.find(lower(repoId));
        List<String> bases = new ArrayList<>();
        for (String s : repo.servers) bases.add(s + id + "/");
        byte[] patchJs = fetchFile(bases, "patch.js");
        JSONObject patch = parse(patchJs, key + "/patch.js");
        List<String> servers = new ArrayList<>();
        for (String s : strings(patch.optJSONArray("servers"))) if (isHttps(s) && !servers.contains(withSlash(s))) servers.add(withSlash(s));
        for (String b : bases) if (!servers.contains(b)) servers.add(b);
        for (String dep : strings(patch.optJSONArray("dependencies"))) {
            dep = dep.trim();
            if (dep.isEmpty()) continue;
            int slash = dep.lastIndexOf('/');
            if (slash >= 0) {
                resolve(repos, dep.substring(0, slash), dep.substring(slash + 1), stack, visiting);
            } else {
                String where = repo.patches.has(dep) ? repo.id : repos.holder(dep);
                resolve(repos, where != null ? where : repo.id, dep, stack, visiting);
            }
        }
        stack.add(new Patch(key, servers, patchJs));
    }

    /** Brings one patch's folder up to date; returns how many files were downloaded. */
    private static int sync(Context ctx, Patch p, int[] done, int total, long[] bytes, Progress progress) throws IOException {
        File dir = folder(ctx, p.key);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot write the game text");
        File localList = new File(dir, LOCAL_FILES);
        JSONObject local;
        try {
            local = localList.isFile() ? new JSONObject(new String(readAll(localList), StandardCharsets.UTF_8)) : new JSONObject();
        } catch (JSONException e) {
            local = new JSONObject();
        }
        // The folder is a download's from its first file on (even if this one is interrupted).
        if (!localList.isFile()) writeAtomically(localList, local.toString().getBytes(StandardCharsets.UTF_8));
        int fetched = 0;
        String name = p.key.substring(p.key.indexOf('/') + 1);
        for (String path : p.wanted) {
            ++done[0];
            File target = new File(dir, lower(path));
            if (p.remote.isNull(path)) {  // removed from the patch
                //noinspection ResultOfMethodCallIgnored
                target.delete();
                local.remove(path);
                continue;
            }
            long crc = p.remote.optLong(path, -1);
            if (crc >= 0 && local.optLong(path, -2) == crc && target.isFile()) continue;
            progress.update(ctx.getString(R.string.th07_gametext_downloading, name, done[0], total));
            byte[] data = fetchFile(p.servers, escape(path) + (crc >= 0 ? String.format(Locale.ROOT, "?crc32=%08x", crc) : ""));
            CRC32 check = new CRC32();
            check.update(data);
            if (crc >= 0 && check.getValue() != crc) throw new IOException("a file was damaged in transfer (" + path + ")");
            bytes[0] += data.length;
            if (bytes[0] > MAX_TOTAL) throw new IOException("the translation is unexpectedly large");
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot write " + path);
            writeAtomically(target, data);
            try {
                local.put(path, check.getValue());
            } catch (JSONException e) {
                throw new IOException(e);
            }
            ++fetched;
        }
        writeAtomically(new File(dir, "patch.js"), p.patchJs);
        writeAtomically(localList, local.toString().getBytes(StandardCharsets.UTF_8));
        return fetched;
    }

    /**
     * The files TH07 reads from a patch: everything in its th07 folder but fonts and audio,
     * and the root .js files the engine looks up (th07.js and its build variants, themes.js
     * for the music titles, stringdefs.js); not the other games' files.
     */
    static boolean wantedFile(String path) {
        if (!safePath(path)) return false;
        String p = lower(path);
        int slash = p.indexOf('/');
        if (slash < 0) {
            return p.equals("stringdefs.js") || p.equals("themes.js") || p.equals(GAME + ".js")
                    || (p.startsWith(GAME + ".") && p.endsWith(".js"));
        }
        if (!p.startsWith(GAME + "/")) return false;
        String rest = p.substring(GAME.length() + 1);
        int depth = 0;
        for (int i = 0; i < rest.length(); ++i) depth += rest.charAt(i) == '/' ? 1 : 0;
        if (depth > MAX_DEPTH) return false;
        for (String skip : new String[] {".ttf", ".otf", ".ttc", ".wav", ".ogg", ".mp3", ".flac", ".exe", ".dll", ".part"}) {
            if (rest.endsWith(skip)) return false;
        }
        return true;
    }

    /** A relative path with no way out of its folder (no "..", no drive or root, no backslash). */
    static boolean safePath(String path) {
        if (path == null || path.isEmpty() || path.length() > 240 || path.startsWith("/") || path.endsWith("/")) return false;
        for (int i = 0; i < path.length(); ++i) {
            char c = path.charAt(i);
            if (c < 0x20 || c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        }
        for (String part : path.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) return false;
        }
        return true;
    }

    // ---- HTTP ---------------------------------------------------------------------------------

    private static boolean isHttps(String s) {
        return s != null && s.startsWith("https://") && s.length() > 8;
    }

    private static String withSlash(String s) {
        return s.endsWith("/") ? s : s + "/";
    }

    private static List<String> strings(JSONArray a) {
        if (a == null) return Collections.emptyList();
        List<String> out = new ArrayList<>();
        for (int i = 0; i < a.length(); ++i) {
            String s = a.optString(i, null);
            if (s != null) out.add(s);
        }
        return out;
    }

    private static JSONObject parse(byte[] b, String what) throws IOException {
        try {
            return new JSONObject(new String(b, StandardCharsets.UTF_8));
        } catch (JSONException e) {
            throw new IOException("unexpected answer from the patch server (" + what + ")");
        }
    }

    private static JSONObject fetchJson(List<String> servers, String path) throws IOException {
        return parse(fetchFile(servers, path), path);
    }

    private static byte[] fetchFile(List<String> servers, String path) throws IOException {
        IOException last = null;
        for (String base : servers) {
            try {
                return fetch(base + path);
            } catch (IOException e) {
                last = e;
            }
        }
        throw last != null ? last : new IOException("no server");
    }

    private static byte[] fetch(String url) throws IOException {
        Fetcher f = fetcher;
        if (f != null) return f.get(url);
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            c.setInstanceFollowRedirects(true);
            c.setRequestProperty("User-Agent", "th07-android (+https://github.com/" + UpdateChecker.REPO + ")");
            int code = c.getResponseCode();
            if (code != 200) throw new IOException("the server answered " + code + " for " + url);
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    if (out.size() > MAX_FILE) throw new IOException("a file is too large");
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

    private static String lower(String s) {
        return s.toLowerCase(Locale.ROOT);
    }

    static byte[] readAll(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    static void writeAtomically(File target, byte[] bytes) throws IOException {
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
}
