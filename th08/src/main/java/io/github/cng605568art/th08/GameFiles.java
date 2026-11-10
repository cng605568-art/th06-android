package io.github.cng605568art.th08;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The player's own game files, imported (copied) into the app's storage under
 * files/gamedata/: th08.dat and th08.exe (both needed) and thbgm.dat (the music, optional).
 * Each file is recognised by its contents, the way the engine checks it: th08.dat is a PBGZ
 * archive, thbgm.dat starts with the game's ZWAV header, and th08.exe must be the 1.00d
 * program (its SHA-256). The engine opens the copies through GameActivity.getDataSources().
 *
 * The same import also brings over the player's PC progress: score.dat (scores, unlocks,
 * spell history, play statistics; recognised by its decoded header) goes where the port
 * keeps its own (the app's files folder; an earlier one is kept as score.dat.bak), and
 * replays (th7_NN.rpy, th7_udXXXX.rpy; "T7RP" header) go into its replay folder.
 *
 * And the English patch, when the game folder has thcrap in it (as on the PC, where the
 * community English patch runs through the thcrap patcher): thcrap/repos/thpatch/lang_en
 * and the patches its patch.js depends on, each with its own .js files and its th08 folder,
 * are copied to files/thcrap/repos/ with their relative paths (lower-cased), replacing the
 * earlier copies of those patches; files/thcrap/imported.list names them. Game text the
 * player downloaded (GameText) shares that folder and is kept. The engine reads the patches
 * as the game-text stack (GameActivity.getGameTextStack()).
 */
final class GameFiles {
    static final String DAT = "th08.dat";
    static final String EXE = "th08.exe";
    static final String BGM = "thbgm.dat";
    /** Kinds of the PC's own records (not game files): score.dat and replays. */
    static final String SCORE = "score.dat";
    static final String REPLAY = "replay";
    /** The thcrap patcher's folder (a Source of this kind is that folder). */
    static final String THCRAP = "thcrap";
    /** In files/thcrap/: the patches ("repo/patch") the last import brought, one per line. */
    private static final String IMPORTED = "imported.list";
    /** Folder levels below a patch's th08 folder that are copied (th08/data/ascii/x.png: 2). */
    private static final int PATCH_DEPTH = 6;
    private static final long SCORE_MAX = 1024 * 1024;
    /** th08.exe 1.00d, as the engine checks it (th08/src/main/cpp/src/th08/GameFiles.hpp). */
    static final String EXE_100D_SHA256 = "330fbdbf58a710829d65277b4f312cfbb38d5448b3df523e79350b879213d924";
    // A Windows program of plausible size, as the engine's Vfs::looksLikeGameExecutable.
    private static final long EXE_MIN = 256 * 1024;
    private static final long EXE_MAX = 4L * 1024 * 1024;
    private static final int MAX_DEPTH = 3;
    private static final long MB = 1024 * 1024;
    private static final long SPACE_MARGIN = 16 * MB;

    private static final String PREFS = "th08_files";
    private static final String KEY_EXE_STAMP = "exe_ok_stamp";  // "<length>:<lastModified>" of a checked th08.exe

    private static final AtomicBoolean IMPORTING = new AtomicBoolean();

    private GameFiles() {}

    static File dir(Context ctx) {
        return new File(ctx.getFilesDir(), "gamedata");
    }

    static File file(Context ctx, String name) {
        return new File(dir(ctx), name);
    }

    // ---- what is there ------------------------------------------------------------------

    /** One imported file: present (and its size) and whether it is the right file. */
    static final class State {
        final String name;
        final boolean present;
        final boolean valid;
        final long size;

        State(String name, boolean present, boolean valid, long size) {
            this.name = name;
            this.present = present;
            this.valid = valid;
            this.size = size;
        }
    }

    static State state(Context ctx, String name) {
        File f = file(ctx, name);
        if (!f.isFile()) return new State(name, false, false, 0);
        long size = f.length();
        boolean valid;
        if (EXE.equals(name)) {
            valid = exeChecked(ctx, f);
        } else {
            byte[] head = new byte[16];
            int n = readHead(f, head);
            valid = name.equals(classify(head, n, size));
        }
        return new State(name, true, valid, size);
    }

    /** th08.dat and th08.exe are both there and right: the game can start a stage. */
    static boolean ready(Context ctx) {
        return state(ctx, DAT).valid && state(ctx, EXE).valid;
    }

    private static boolean exeChecked(Context ctx, File f) {
        String stamp = f.length() + ":" + f.lastModified();
        SharedPreferences p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (stamp.equals(p.getString(KEY_EXE_STAMP, null))) return true;
        boolean ok = f.length() >= EXE_MIN && f.length() <= EXE_MAX && EXE_100D_SHA256.equals(sha256(f));
        if (ok) p.edit().putString(KEY_EXE_STAMP, stamp).apply();
        return ok;
    }

    private static int readHead(File f, byte[] head) {
        try (InputStream in = new FileInputStream(f)) {
            return readFully(in, head);
        } catch (IOException e) {
            return 0;
        }
    }

    private static String sha256(File f) {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[256 * 1024];
            int n;
            while ((n = in.read(buf)) != -1) md.update(buf, 0, n);
            return hex(md.digest());
        } catch (IOException | NoSuchAlgorithmException e) {
            return "";
        }
    }

    private static String hex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }

    private static int readFully(InputStream in, byte[] buf) throws IOException {
        int got = 0;
        while (got < buf.length) {
            int n = in.read(buf, got, buf.length - got);
            if (n < 0) break;
            got += n;
        }
        return got;
    }

    private static int le16(byte[] b, int at) {
        return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8;
    }

    private static long le32(byte[] b, int at) {
        return (b[at] & 0xffL) | (b[at + 1] & 0xffL) << 8 | (b[at + 2] & 0xffL) << 16 | (b[at + 3] & 0xffL) << 24;
    }

    /**
     * Which of the game's files these first bytes belong to (DAT, BGM or EXE), or null. `size`
     * is the whole file's (negative: not known yet).
     */
    static String classify(byte[] head, int n, long size) {
        if (n >= 4 && head[0] == 'P' && head[1] == 'B' && head[2] == 'G' && head[3] == 'Z') return DAT;
        // TH08's thbgm.dat: "ZWAV", version 1, 0x800.
        if (n >= 12 && head[0] == 'Z' && head[1] == 'W' && head[2] == 'A' && head[3] == 'V'
                && le32(head, 4) == 1 && le32(head, 8) == 0x800) {
            return BGM;
        }
        if (n >= 2 && head[0] == 'M' && head[1] == 'Z' && (size < 0 || (size >= EXE_MIN && size <= EXE_MAX))) return EXE;
        // A replay: "T8RP". (TH08's score.dat is not recognised yet: the port has no reader for
        // it, so the import leaves it alone.)
        if (n >= 4 && head[0] == 'T' && head[1] == '8' && head[2] == 'R' && head[3] == 'P') {
            return REPLAY;
        }
        return null;
    }

    /**
     * score.dat (format 11): from byte 2 on it is obfuscated with a running key that starts
     * at byte 1 (rotate left by 3, XOR, then add the plain byte). Decoded, the version at
     * offset 4 is 11 and the first record starts at 0x1C.
     */
    private static boolean scoreHeader(byte[] head) {
        byte[] plain = new byte[12];
        int k = head[1] & 0xff;
        for (int p = 2; p < 12; ++p) {
            k = ((k << 3) | (k >>> 5)) & 0xff;
            plain[p] = (byte) ((head[p] & 0xff) ^ k);
            k = (k + (plain[p] & 0xff)) & 0xff;
        }
        return le16(plain, 4) == 11 && le32(plain, 8) == 0x1C;
    }

    /** Where the port keeps its own score.dat (SDL's preferences path on Android). */
    static File scoreFile(Context ctx) {
        return new File(ctx.getFilesDir(), SCORE);
    }

    /** Where the port keeps its replays. */
    static File replayDir(Context ctx) {
        return new File(ctx.getFilesDir(), REPLAY);
    }

    /**
     * Deletes every imported file (the English patch too; patch folders that downloaded game
     * text also uses stay). Call with the files lock held.
     */
    static void removeAll(Context ctx) {
        File[] files = dir(ctx).listFiles();
        if (files != null) {
            for (File f : files) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }
        Set<String> downloaded = GameText.downloadedPatches(ctx);
        if (downloaded.isEmpty()) {
            deleteTree(thcrapDir(ctx));
        } else {
            for (String key : importedPatches(ctx)) {
                if (!downloaded.contains(key)) deleteTree(GameText.folder(ctx, key));
            }
            try {
                writeList(importedList(ctx), new ArrayList<>());
            } catch (IOException ignored) {
                // The folders are gone; a stale list only names folders that no longer exist.
            }
            pruneEmptyRepos(ctx);
        }
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY_EXE_STAMP).apply();
    }

    // ---- the English patch (thcrap) -------------------------------------------------------

    /** Where the patches' files go: files/thcrap/repos/<repo>/<patch>/... */
    static File thcrapDir(Context ctx) {
        return new File(ctx.getFilesDir(), THCRAP);
    }

    private static File importedList(Context ctx) {
        return new File(thcrapDir(ctx), IMPORTED);
    }

    /** "repo/patch" in lower case, nothing that leaves the repos folder. */
    static boolean validKey(String key) {
        if (key == null || key.length() > 160) return false;
        int slash = key.indexOf('/');
        if (slash <= 0 || slash != key.lastIndexOf('/') || slash == key.length() - 1) return false;
        String repo = key.substring(0, slash), patch = key.substring(slash + 1);
        if (repo.equals(".") || repo.equals("..") || patch.equals(".") || patch.equals("..")) return false;
        for (int i = 0; i < key.length(); ++i) {
            char c = key.charAt(i);
            if (c < 0x20 || c == '\\' || c == ':' || Character.isUpperCase(c)) return false;
        }
        return true;
    }

    /**
     * The patches the last import brought ("repo/patch"). From before that was recorded (an
     * earlier version of the app imported everything there is): every patch folder that no
     * download wrote.
     */
    static List<String> importedPatches(Context ctx) {
        List<String> out = new ArrayList<>();
        File list = importedList(ctx);
        if (list.isFile()) {
            for (String k : GameText.readStack(list)) out.add(k);
            return out;
        }
        File[] repos = GameText.repos(ctx).listFiles();
        if (repos == null) return out;
        for (File repo : repos) {
            File[] patches = repo.listFiles();
            if (patches == null) continue;
            for (File p : patches) {
                String key = lower(repo.getName()) + "/" + lower(p.getName());
                if (p.isDirectory() && validKey(key) && !new File(p, GameText.LOCAL_FILES).isFile()) out.add(key);
            }
        }
        java.util.Collections.sort(out);
        return out;
    }

    /**
     * Writes imported.list for an earlier version's import (which had none), so that a
     * download into the same folders cannot later make them look like downloads only.
     */
    static void recordEarlierImport(Context ctx) throws IOException {
        if (importedList(ctx).isFile() || !GameText.repos(ctx).isDirectory()) return;
        writeList(importedList(ctx), importedPatches(ctx));
    }

    /** The English patch (some repository's lang_en) is imported. */
    static boolean englishPresent(Context ctx) {
        for (String key : importedPatches(ctx)) {
            if (key.endsWith("/lang_en") && new File(GameText.folder(ctx, key), "patch.js").isFile()) return true;
        }
        return false;
    }

    /** How many of the imported patches' files there are. */
    static int englishFiles(Context ctx) {
        int n = 0;
        for (String key : importedPatches(ctx)) n += countFiles(GameText.folder(ctx, key), 0);
        return n;
    }

    /** Removes repository folders left empty. */
    static void pruneEmptyRepos(Context ctx) {
        File[] repos = GameText.repos(ctx).listFiles();
        if (repos == null) return;
        for (File r : repos) {
            String[] left = r.list();
            if (r.isDirectory() && left != null && left.length == 0) {
                //noinspection ResultOfMethodCallIgnored
                r.delete();
            }
        }
    }

    private static void writeList(File f, List<String> keys) throws IOException {
        File dir = f.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot write " + f.getName());
        StringBuilder sb = new StringBuilder();
        for (String k : keys) sb.append(k).append('\n');
        GameText.writeAtomically(f, sb.toString().getBytes(StandardCharsets.UTF_8));
    }

    /**
     * Puts the patches an import copied to `part` (part/repos/<repo>/<patch>) in place: each
     * replaces its earlier copy; patches an earlier import brought that this one does not are
     * deleted unless downloaded game text uses them; imported.list names the new ones.
     */
    static void installImportedPatches(Context ctx, File part, List<String> imported) throws IOException {
        File repos = GameText.repos(ctx);
        Set<String> downloaded = GameText.downloadedPatches(ctx);
        for (String old : importedPatches(ctx)) {
            if (!imported.contains(old) && !downloaded.contains(old)) deleteTree(new File(repos, old));
        }
        for (String key : imported) {
            File from = new File(part, "repos/" + key), to = new File(repos, key);
            deleteTree(to);
            if (!from.isDirectory()) continue;
            File parent = to.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException(ctx.getString(R.string.import_cannot_store));
            if (!from.renameTo(to)) throw new IOException(ctx.getString(R.string.import_cannot_store));
        }
        writeList(importedList(ctx), imported);
        deleteTree(part);
        pruneEmptyRepos(ctx);
    }

    private static int countFiles(File dir, int depth) {
        File[] list = dir.listFiles();
        if (list == null || depth > PATCH_DEPTH + 4) return 0;
        int n = 0;
        for (File f : list) n += f.isDirectory() ? countFiles(f, depth + 1) : 1;
        return n;
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) {
            for (File c : list) deleteTree(c);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    // ---- where to import from -------------------------------------------------------------

    /** A file the player chose (directly, or found in the folder they chose). */
    static final class Source {
        final String name;
        final Uri uri;
        final long size;  // negative: unknown
        String kind;      // DAT, EXE, BGM or null, from its first bytes (THCRAP: a folder)
        // A THCRAP folder: the picked tree, the folder's document id and how deep it was found.
        Uri tree;
        String documentId;
        int depth;

        Source(String name, Uri uri, long size) {
            this.name = name;
            this.uri = uri;
            this.size = size;
        }
    }

    interface Progress {
        void report(String message);
    }

    /**
     * Walks the picked folder (a few levels deep) for .dat, .exe and .rpy files, and notes a
     * thcrap folder (not walked here: importEnglish() takes what it needs from it).
     */
    static List<Source> scanFolder(Context ctx, Uri tree, Progress progress) {
        ContentResolver cr = ctx.getContentResolver();
        List<Source> found = new ArrayList<>();
        Deque<String[]> stack = new ArrayDeque<>();  // {documentId, depth, displayPath}
        stack.push(new String[] {DocumentsContract.getTreeDocumentId(tree), "0", ""});
        String[] projection = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
                Document.COLUMN_SIZE};
        while (!stack.isEmpty()) {
            String[] dir = stack.pop();
            int depth = Integer.parseInt(dir[1]);
            if (progress != null) {
                progress.report(ctx.getString(R.string.import_scanning_path, dir[2].isEmpty() ? "/" : dir[2]));
            }
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dir[0]);
            try (Cursor c = cr.query(children, projection, null, null, null)) {
                if (c == null) continue;
                while (c.moveToNext()) {
                    String id = c.getString(0);
                    String name = c.getString(1);
                    String mime = c.getString(2);
                    long size = c.isNull(3) ? -1 : c.getLong(3);
                    if (id == null || name == null) continue;
                    if (Document.MIME_TYPE_DIR.equals(mime)) {
                        if (THCRAP.equalsIgnoreCase(name)) {
                            Source s = new Source(name, DocumentsContract.buildDocumentUriUsingTree(tree, id), -1);
                            s.kind = THCRAP;
                            s.tree = tree;
                            s.documentId = id;
                            s.depth = depth;
                            found.add(s);
                            continue;
                        }
                        if (depth < MAX_DEPTH) stack.push(new String[] {id, String.valueOf(depth + 1), dir[2] + "/" + name});
                        continue;
                    }
                    String lower = name.toLowerCase(Locale.ROOT);
                    if (!lower.endsWith(".dat") && !lower.endsWith(".exe") && !lower.endsWith(".rpy")) continue;
                    found.add(new Source(name, DocumentsContract.buildDocumentUriUsingTree(tree, id), size));
                }
            } catch (RuntimeException ignored) {
                // An unreadable folder (provider error): skip it, keep scanning the rest.
            }
        }
        return found;
    }

    /** The files the player picked one by one: their names and sizes. */
    static List<Source> describe(Context ctx, List<Uri> docs) {
        List<Source> out = new ArrayList<>();
        ContentResolver cr = ctx.getContentResolver();
        for (Uri uri : docs) {
            String name = uri.getLastPathSegment();
            long size = -1;
            try (Cursor c = cr.query(uri, new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    if (!c.isNull(0)) name = c.getString(0);
                    if (!c.isNull(1)) size = c.getLong(1);
                }
            } catch (RuntimeException ignored) {
                // No metadata: keep the URI's own name.
            }
            out.add(new Source(name != null ? name : "?", uri, size));
        }
        return out;
    }

    // ---- importing ------------------------------------------------------------------------

    /** Only one import at a time (the launcher can be recreated while one runs). */
    static boolean beginImport() {
        return IMPORTING.compareAndSet(false, true);
    }

    static void endImport() {
        IMPORTING.set(false);
    }

    static boolean importing() {
        return IMPORTING.get();
    }

    /**
     * Copies the game's files among `sources` into the app's storage, replacing earlier
     * copies; files not among them are kept. Returns a summary for the player.
     */
    static String importFrom(Context ctx, List<Source> sources, boolean pickedOneByOne, Progress progress)
            throws IOException {
        ContentResolver cr = ctx.getContentResolver();
        Source dat = null, bgm = null, score = null, thcrap = null;
        List<Source> exes = new ArrayList<>();
        List<Source> replays = new ArrayList<>();
        List<String> skipped = new ArrayList<>();
        for (Source s : sources) {
            if (THCRAP.equals(s.kind)) {  // a folder: the one nearest the top
                if (thcrap == null || s.depth < thcrap.depth) thcrap = s;
                continue;
            }
            progress.report(ctx.getString(R.string.import_checking, s.name));
            byte[] head = new byte[16];
            int n;
            try (InputStream in = cr.openInputStream(s.uri)) {
                n = in == null ? 0 : readFully(in, head);
            } catch (IOException | RuntimeException e) {
                n = 0;
            }
            s.kind = classify(head, n, s.size);
            String lower = s.name.toLowerCase(Locale.ROOT);
            if (DAT.equals(s.kind)) {
                if (dat == null || (lower.equals(DAT) && !dat.name.equalsIgnoreCase(DAT))) dat = s;
            } else if (BGM.equals(s.kind)) {
                if (bgm == null || (lower.equals(BGM) && !bgm.name.equalsIgnoreCase(BGM))) bgm = s;
            } else if (EXE.equals(s.kind)) {
                if (lower.equals(EXE)) exes.add(0, s);  // the game's own name first
                else exes.add(s);
            } else if (SCORE.equals(s.kind)) {
                if (score == null || (lower.equals(SCORE) && !score.name.equalsIgnoreCase(SCORE))) score = s;
            } else if (REPLAY.equals(s.kind)) {
                replays.add(s);
            } else {
                skipped.add(s.name);
            }
        }
        if (dat == null && bgm == null && exes.isEmpty()) {
            // Only the PC's records (a backup of scores or replays) or the English patch:
            // import just those.
            if (score == null && replays.isEmpty() && thcrap == null) return ctx.getString(R.string.import_nothing);
            StringBuilder report = new StringBuilder();
            importRecords(ctx, score, replays, report, progress);
            importEnglish(ctx, thcrap, report, progress);
            return report.toString().trim();
        }

        File dir = dir(ctx);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException(ctx.getString(R.string.import_cannot_store));
        StringBuilder report = new StringBuilder();
        if (dat != null) {
            long bytes = copy(ctx, dat, file(ctx, DAT), null, progress);
            line(report, ctx.getString(R.string.import_done_file, DAT, bytes / (double) MB));
        } else {
            notFound(ctx, report, DAT);
        }

        boolean exeDone = false;
        Source wrongExe = null;
        for (Source s : exes) {
            MessageDigest md;
            try {
                md = MessageDigest.getInstance("SHA-256");
            } catch (NoSuchAlgorithmException e) {
                throw new IOException(e.toString());
            }
            File target = file(ctx, EXE);
            File part = copyToPart(ctx, s, target, md, progress);
            long length = part.length();
            if (length >= EXE_MIN && length <= EXE_MAX && EXE_100D_SHA256.equals(hex(md.digest()))) {
                commit(part, target);
                ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                        .putString(KEY_EXE_STAMP, target.length() + ":" + target.lastModified()).apply();
                line(report, ctx.getString(R.string.import_done_exe, EXE));
                exeDone = true;
                break;
            }
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            if (wrongExe == null) wrongExe = s;
        }
        if (!exeDone) {
            // A folder holds other programs too (the configuration tool): name the wrong one
            // only when it was picked directly or is called th08.exe.
            if (wrongExe != null && (pickedOneByOne || wrongExe.name.equalsIgnoreCase(EXE))) {
                line(report, ctx.getString(R.string.import_exe_wrong, wrongExe.name));
            } else {
                notFound(ctx, report, EXE);
            }
        }

        if (bgm != null) {
            long bytes = copy(ctx, bgm, file(ctx, BGM), null, progress);
            line(report, ctx.getString(R.string.import_done_file, BGM, bytes / (double) MB));
        } else {
            notFound(ctx, report, BGM);
        }
        importRecords(ctx, score, replays, report, progress);
        importEnglish(ctx, thcrap, report, progress);
        if (pickedOneByOne && !skipped.isEmpty()) {
            line(report, ctx.getString(R.string.import_skipped, join(skipped)));
        }
        return report.toString().trim();
    }

    /** The English patch from a thcrap folder, if one was found (an earlier copy is kept otherwise). */
    private static void importEnglish(Context ctx, Source thcrap, StringBuilder report, Progress progress) throws IOException {
        if (thcrap == null) return;
        List<String> patches = new ArrayList<>();
        int files = importEnglishPatch(ctx, thcrap, patches, progress);
        if (files < 0) {
            line(report, ctx.getString(R.string.import_english_none));
            return;
        }
        List<String> ids = new ArrayList<>();
        for (String p : patches) ids.add(p.substring(p.indexOf('/') + 1));
        line(report, ctx.getString(R.string.import_done_english, files, join(ids)));
    }

    /** A folder's children in a picked tree: {document id, name, MIME type}. */
    private static List<String[]> children(ContentResolver cr, Uri tree, String documentId) {
        List<String[]> out = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        String[] projection = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE};
        try (Cursor c = cr.query(uri, projection, null, null, null)) {
            if (c == null) return out;
            while (c.moveToNext()) {
                String id = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                if (id != null && name != null) out.add(new String[] {id, name, mime == null ? "" : mime});
            }
        } catch (RuntimeException ignored) {
            // An unreadable folder: nothing from it.
        }
        return out;
    }

    private static boolean isFolder(String[] child) {
        return Document.MIME_TYPE_DIR.equals(child[2]);
    }

    /**
     * thcrap/repos/<repo>/<patch>/: lang_en (thpatch's first) and, from each patch.js, the
     * patches it depends on ("repo/patch", or a bare id: the same repository first); each
     * patch's own .js files and its th08 folder are copied. Returns the files copied, or -1
     * when there is no lang_en. `patchesOut` gets the patches ("repo/patch").
     */
    static int importEnglishPatch(Context ctx, Source thcrap, List<String> patchesOut, Progress progress) throws IOException {
        ContentResolver cr = ctx.getContentResolver();
        String repos = null;
        for (String[] c : children(cr, thcrap.tree, thcrap.documentId)) {
            if (isFolder(c) && c[1].equalsIgnoreCase("repos")) repos = c[0];
        }
        if (repos == null) return -1;
        Map<String, String> patches = new LinkedHashMap<>();  // "repo/patch" (lower case) -> document id
        for (String[] repo : children(cr, thcrap.tree, repos)) {
            if (!isFolder(repo)) continue;
            for (String[] p : children(cr, thcrap.tree, repo[0])) {
                String key = lower(repo[1]) + "/" + lower(p[1]);
                if (isFolder(p) && validKey(key)) patches.put(key, p[0]);
            }
        }
        String lang = patches.containsKey("thpatch/lang_en") ? "thpatch/lang_en" : null;
        for (String k : patches.keySet()) {
            if (lang == null && k.endsWith("/lang_en")) lang = k;
        }
        if (lang == null) return -1;
        List<String> wanted = new ArrayList<>();
        Deque<String> todo = new ArrayDeque<>();
        todo.add(lang);
        while (!todo.isEmpty()) {
            String key = todo.poll();
            if (wanted.contains(key) || !patches.containsKey(key)) continue;
            wanted.add(key);
            String repo = key.substring(0, key.indexOf('/'));
            for (String dep : dependencies(ctx, thcrap.tree, patches.get(key))) {
                String depKey = dep.indexOf('/') >= 0 ? lower(dep) : repo + "/" + lower(dep);
                if (!patches.containsKey(depKey) && dep.indexOf('/') < 0) {
                    for (String k : patches.keySet()) {
                        if (k.endsWith("/" + lower(dep))) {
                            depKey = k;
                            break;
                        }
                    }
                }
                todo.add(depKey);
            }
        }
        File part = new File(ctx.getFilesDir(), THCRAP + ".part");
        deleteTree(part);
        int[] copied = {0};
        for (String key : wanted) {
            File dir = new File(part, "repos/" + key);
            for (String[] c : children(cr, thcrap.tree, patches.get(key))) {
                if (!GameText.safePath(c[1]) || c[1].indexOf('/') >= 0) continue;  // (a name, not a path)
                if (isFolder(c)) {
                    if (c[1].equalsIgnoreCase("th08")) copyFolder(ctx, thcrap.tree, c[0], new File(dir, "th08"), 0, copied, progress);
                } else if (lower(c[1]).endsWith(".js")) {
                    copyDocument(ctx, DocumentsContract.buildDocumentUriUsingTree(thcrap.tree, c[0]), new File(dir, lower(c[1])));
                    ++copied[0];
                }
            }
        }
        installImportedPatches(ctx, part, wanted);
        patchesOut.addAll(wanted);
        return copied[0];
    }

    private static void copyFolder(Context ctx, Uri tree, String documentId, File dir, int depth, int[] copied, Progress progress)
            throws IOException {
        for (String[] c : children(ctx.getContentResolver(), tree, documentId)) {
            if (!GameText.safePath(c[1]) || c[1].indexOf('/') >= 0) continue;  // (a name, not a path)
            if (isFolder(c)) {
                if (depth < PATCH_DEPTH) copyFolder(ctx, tree, c[0], new File(dir, lower(c[1])), depth + 1, copied, progress);
                continue;
            }
            copyDocument(ctx, DocumentsContract.buildDocumentUriUsingTree(tree, c[0]), new File(dir, lower(c[1])));
            if (++copied[0] % 10 == 0) progress.report(ctx.getString(R.string.import_copying_english, copied[0]));
        }
    }

    private static void copyDocument(Context ctx, Uri uri, File target) throws IOException {
        File dir = target.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException(ctx.getString(R.string.import_cannot_store));
        try (InputStream in = ctx.getContentResolver().openInputStream(uri); FileOutputStream out = new FileOutputStream(target)) {
            if (in == null) throw new IOException(ctx.getString(R.string.import_cannot_read, target.getName()));
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
        }
    }

    /** A patch's patch.js "dependencies" (the strings in that array; empty if none). */
    private static List<String> dependencies(Context ctx, Uri tree, String patchDocumentId) {
        List<String> out = new ArrayList<>();
        for (String[] c : children(ctx.getContentResolver(), tree, patchDocumentId)) {
            if (isFolder(c) || !c[1].equalsIgnoreCase("patch.js")) continue;
            String text;
            try (InputStream in = ctx.getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, c[0]))) {
                if (in == null) return out;
                byte[] buf = new byte[64 * 1024];
                int n = readFully(in, buf);
                text = new String(buf, 0, n, StandardCharsets.UTF_8);
            } catch (IOException | RuntimeException e) {
                return out;
            }
            return parseDependencies(text);
        }
        return out;
    }

    /** The strings of the "dependencies" array in a patch.js text. */
    static List<String> parseDependencies(String text) {
        List<String> out = new ArrayList<>();
        int at = text.indexOf("\"dependencies\"");
        if (at < 0) return out;
        int open = text.indexOf('[', at), close = text.indexOf(']', at);
        if (open < 0 || close < open) return out;
        String list = text.substring(open + 1, close);
        for (int i = list.indexOf('"'); i >= 0; ) {
            int end = list.indexOf('"', i + 1);
            if (end < 0) break;
            String dep = list.substring(i + 1, end).trim();
            if (!dep.isEmpty()) out.add(dep);
            i = list.indexOf('"', end + 1);
        }
        return out;
    }

    private static String lower(String s) {
        return s.toLowerCase(Locale.ROOT);
    }

    /** score.dat and replays from the PC, where the port keeps its own. */
    private static void importRecords(Context ctx, Source score, List<Source> replays, StringBuilder report,
            Progress progress) throws IOException {
        if (score != null) {
            File target = scoreFile(ctx);
            File part = copyToPart(ctx, score, target, null, progress);
            boolean kept = false;
            if (target.isFile()) {
                File bak = new File(target.getParentFile(), SCORE + ".bak");
                //noinspection ResultOfMethodCallIgnored
                bak.delete();
                kept = target.renameTo(bak);
            }
            commit(part, target);
            line(report, ctx.getString(kept ? R.string.import_done_score_kept : R.string.import_done_score));
        }
        if (replays.isEmpty()) return;
        File dir = replayDir(ctx);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException(ctx.getString(R.string.import_cannot_store));
        int added = 0, already = 0;
        for (Source s : replays) {
            MessageDigest md;
            try {
                md = MessageDigest.getInstance("SHA-256");
            } catch (NoSuchAlgorithmException e) {
                throw new IOException(e.toString());
            }
            File part = copyToPart(ctx, s, new File(dir, "import.rpy"), md, progress);
            String digest = hex(md.digest());
            if (sameReplayThere(dir, part.length(), digest)) {
                //noinspection ResultOfMethodCallIgnored
                part.delete();
                ++already;
                continue;
            }
            String name = replayName(dir, s.name);
            if (name == null) {
                //noinspection ResultOfMethodCallIgnored
                part.delete();
                line(report, ctx.getString(R.string.import_replays_full));
                break;
            }
            commit(part, new File(dir, name));
            ++added;
        }
        line(report, ctx.getString(R.string.import_done_replays, added, already));
    }

    /** A replay with these bytes is already in the folder (imported before). */
    private static boolean sameReplayThere(File dir, long length, String digest) {
        File[] files = dir.listFiles();
        if (files == null) return false;
        for (File f : files) {
            if (f.isFile() && f.getName().endsWith(".rpy") && f.length() == length && digest.equals(sha256(f))) return true;
        }
        return false;
    }

    /**
     * The name the port lists a replay under: the PC's own name when it is one of the port's
     * (th7_01 to th7_15, or th7_ud plus 4 characters) and free; otherwise the next free
     * th7_udNNNN. Null when no name is free.
     */
    static String replayName(File dir, String original) {
        String lower = original.toLowerCase(Locale.ROOT);
        boolean slot = lower.matches("th7_(0[1-9]|1[0-5])\\.rpy");
        boolean user = lower.matches("th7_ud.{4}\\.rpy");
        if ((slot || user) && !new File(dir, lower).exists()) return lower;
        for (int i = 0; i < 10000; ++i) {
            String name = String.format(Locale.ROOT, "th7_ud%04d.rpy", i);
            if (!new File(dir, name).exists()) return name;
        }
        return null;
    }

    private static void line(StringBuilder sb, String s) {
        sb.append(s).append('\n');
    }

    /** "a, b, c" (String.join needs API 26; this app runs from API 24). */
    static String join(Iterable<String> items) {
        StringBuilder sb = new StringBuilder();
        for (String s : items) {
            if (sb.length() > 0) sb.append(", ");
            sb.append(s);
        }
        return sb.toString();
    }

    private static void notFound(Context ctx, StringBuilder report, String name) {
        if (state(ctx, name).present) line(report, ctx.getString(R.string.import_kept, name));
        else if (BGM.equals(name)) line(report, ctx.getString(R.string.import_bgm_not_found));
        else line(report, ctx.getString(R.string.import_not_found, name));
    }

    /** Copies `src` over `target`; returns the bytes copied. */
    private static long copy(Context ctx, Source src, File target, MessageDigest md, Progress progress) throws IOException {
        File part = copyToPart(ctx, src, target, md, progress);
        long bytes = part.length();
        commit(part, target);
        return bytes;
    }

    /** Copies `src` into "<target>.part" (deleted again if anything goes wrong). */
    private static File copyToPart(Context ctx, Source src, File target, MessageDigest md, Progress progress)
            throws IOException {
        File dir = target.getParentFile();
        File part = new File(dir, target.getName() + ".part");
        //noinspection ResultOfMethodCallIgnored
        part.delete();
        if (md == null && src.size > 0 && dir.getUsableSpace() < src.size + SPACE_MARGIN && target.isFile()) {
            // Not enough room for the new copy next to the old one: the old one goes first
            // (not for th08.exe, whose new copy may still turn out to be the wrong version).
            //noinspection ResultOfMethodCallIgnored
            target.delete();
        }
        long free = dir.getUsableSpace();
        if (src.size > 0 && free < src.size + SPACE_MARGIN) {
            throw new IOException(ctx.getString(R.string.import_no_space, src.name,
                    (src.size + SPACE_MARGIN + MB - 1) / MB, free / MB));
        }
        long totalMb = src.size > 0 ? (src.size + MB - 1) / MB : -1;
        boolean ok = false;
        try (InputStream in = ctx.getContentResolver().openInputStream(src.uri);
             FileOutputStream out = new FileOutputStream(part)) {
            if (in == null) throw new IOException(ctx.getString(R.string.import_cannot_read, src.name));
            byte[] buf = new byte[1024 * 1024];
            long done = 0, shown = -1;
            int n;
            while ((n = in.read(buf)) != -1) {
                out.write(buf, 0, n);
                if (md != null) md.update(buf, 0, n);
                done += n;
                long mb = done / MB;
                if (mb != shown && (mb % 4 == 0 || shown < 0)) {
                    shown = mb;
                    progress.report(totalMb > 0
                            ? ctx.getString(R.string.import_copying, src.name, mb, totalMb)
                            : ctx.getString(R.string.import_copying_unknown, src.name, mb));
                }
            }
            out.getFD().sync();
            ok = true;
        } finally {
            if (!ok) {
                //noinspection ResultOfMethodCallIgnored
                part.delete();
            }
        }
        return part;
    }

    private static void commit(File part, File target) throws IOException {
        if (part.renameTo(target)) return;
        //noinspection ResultOfMethodCallIgnored
        target.delete();
        if (!part.renameTo(target)) {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            throw new IOException("cannot store " + target.getName());
        }
    }
}
