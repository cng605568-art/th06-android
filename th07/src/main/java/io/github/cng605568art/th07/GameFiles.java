package io.github.cng605568art.th07;

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
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The player's own game files, imported (copied) into the app's storage under
 * files/gamedata/: th07.dat and th07.exe (both needed) and thbgm.dat (the music, optional).
 * Each file is recognised by its contents, the way the engine checks it: th07.dat is a PBG4
 * archive, thbgm.dat starts with the game's ZWAV header, and th07.exe must be the 1.00b
 * program (its SHA-256). The engine opens the copies through GameActivity.getDataSources().
 */
final class GameFiles {
    static final String DAT = "th07.dat";
    static final String EXE = "th07.exe";
    static final String BGM = "thbgm.dat";
    /** th07.exe 1.00b, as the engine checks it (th07/src/main/cpp/src/th07/ExeTables.hpp). */
    static final String EXE_100B_SHA256 = "35467eaf8dc7fc85f024f16fb2037255f151cefda33cf4867bc9122aaa2e80ca";
    // A Windows program of plausible size, as the engine's Vfs::looksLikeGameExecutable.
    private static final long EXE_MIN = 256 * 1024;
    private static final long EXE_MAX = 4L * 1024 * 1024;
    private static final int MAX_DEPTH = 3;
    private static final long MB = 1024 * 1024;
    private static final long SPACE_MARGIN = 16 * MB;

    private static final String PREFS = "th07_files";
    private static final String KEY_EXE_STAMP = "exe_ok_stamp";  // "<length>:<lastModified>" of a checked th07.exe

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

    /** th07.dat and th07.exe are both there and right: the game can start a stage. */
    static boolean ready(Context ctx) {
        return state(ctx, DAT).valid && state(ctx, EXE).valid;
    }

    private static boolean exeChecked(Context ctx, File f) {
        String stamp = f.length() + ":" + f.lastModified();
        SharedPreferences p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (stamp.equals(p.getString(KEY_EXE_STAMP, null))) return true;
        boolean ok = f.length() >= EXE_MIN && f.length() <= EXE_MAX && EXE_100B_SHA256.equals(sha256(f));
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

    private static long le32(byte[] b, int at) {
        return (b[at] & 0xffL) | (b[at + 1] & 0xffL) << 8 | (b[at + 2] & 0xffL) << 16 | (b[at + 3] & 0xffL) << 24;
    }

    /**
     * Which of the game's files these first bytes belong to (DAT, BGM or EXE), or null. `size`
     * is the whole file's (negative: not known yet).
     */
    static String classify(byte[] head, int n, long size) {
        if (n >= 4 && head[0] == 'P' && head[1] == 'B' && head[2] == 'G' && head[3] == '4') return DAT;
        // As the engine's validThbgmHeader: "ZWAV", version 1, 0x700.
        if (n >= 12 && head[0] == 'Z' && head[1] == 'W' && head[2] == 'A' && head[3] == 'V'
                && le32(head, 4) == 1 && le32(head, 8) == 0x700) {
            return BGM;
        }
        if (n >= 2 && head[0] == 'M' && head[1] == 'Z' && (size < 0 || (size >= EXE_MIN && size <= EXE_MAX))) return EXE;
        return null;
    }

    /** Deletes every imported file. */
    static void removeAll(Context ctx) {
        File[] files = dir(ctx).listFiles();
        if (files != null) {
            for (File f : files) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY_EXE_STAMP).apply();
    }

    // ---- where to import from -------------------------------------------------------------

    /** A file the player chose (directly, or found in the folder they chose). */
    static final class Source {
        final String name;
        final Uri uri;
        final long size;  // negative: unknown
        String kind;      // DAT, EXE, BGM or null, from its first bytes

        Source(String name, Uri uri, long size) {
            this.name = name;
            this.uri = uri;
            this.size = size;
        }
    }

    interface Progress {
        void report(String message);
    }

    /** Walks the picked folder (a few levels deep) for .dat and .exe files. */
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
                        if (depth < MAX_DEPTH) stack.push(new String[] {id, String.valueOf(depth + 1), dir[2] + "/" + name});
                        continue;
                    }
                    String lower = name.toLowerCase(Locale.ROOT);
                    if (!lower.endsWith(".dat") && !lower.endsWith(".exe")) continue;
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
        Source dat = null, bgm = null;
        List<Source> exes = new ArrayList<>();
        List<String> skipped = new ArrayList<>();
        for (Source s : sources) {
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
            } else {
                skipped.add(s.name);
            }
        }
        if (dat == null && bgm == null && exes.isEmpty()) {
            return ctx.getString(R.string.import_nothing);
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
            if (length >= EXE_MIN && length <= EXE_MAX && EXE_100B_SHA256.equals(hex(md.digest()))) {
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
            // only when it was picked directly or is called th07.exe.
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
        if (pickedOneByOne && !skipped.isEmpty()) {
            line(report, ctx.getString(R.string.import_skipped, join(skipped)));
        }
        return report.toString().trim();
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
            // (not for th07.exe, whose new copy may still turn out to be the wrong version).
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
