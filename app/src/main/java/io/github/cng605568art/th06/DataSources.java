package io.github.cng605568art.th06;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.Locale;

/**
 * Finds the player's own game archives inside a folder they picked with the Storage Access
 * Framework, and remembers them. Nothing is copied: the native engine later opens each
 * archive read-only through a file descriptor and memory-maps it in place.
 */
final class DataSources {
    static final String FORMAT_PBG3 = "PBG3"; // 2002 original (紅魔郷CM.DAT, KOUMAKYO_CM.dat, ...)
    static final String FORMAT_PKGL = "PKGL"; // New Classic, Steam 2026 (th06CM.dat, ...)
    static final String FORMAT_EXE = "EXE";   // the game program (東方紅魔郷.exe): some data tables are read from it
    static final String FORMAT_WAV = "WAV";   // the 2002 music, bgm/th06_NN.wav (loose files, not in the archives)

    private static final String PREFS = "th06_data_sources";
    private static final String KEY_TREE = "tree_uri";
    private static final String KEY_FILES = "files";
    private static final String KEY_PREFER_NC = "prefer_new_classic";
    private static final int MAX_DEPTH = 4;

    static final class Entry {
        final String name;
        final String uri;
        final String format;
        final long size;

        Entry(String name, String uri, String format, long size) {
            this.name = name;
            this.uri = uri;
            this.format = format;
            this.size = size;
        }
    }

    private DataSources() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static String treeUri(Context ctx) {
        return prefs(ctx).getString(KEY_TREE, null);
    }

    static boolean preferNewClassic(Context ctx) {
        return prefs(ctx).getBoolean(KEY_PREFER_NC, false);
    }

    static void setPreferNewClassic(Context ctx, boolean value) {
        prefs(ctx).edit().putBoolean(KEY_PREFER_NC, value).apply();
    }

    static List<Entry> load(Context ctx) {
        List<Entry> out = new ArrayList<>();
        String json = prefs(ctx).getString(KEY_FILES, "[]");
        try {
            JSONArray arr = new JSONArray(json);
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                out.add(new Entry(o.getString("name"), o.getString("uri"),
                        o.getString("format"), o.optLong("size", 0)));
            }
        } catch (JSONException e) {
            out.clear();
        }
        return out;
    }

    static void save(Context ctx, Uri tree, List<Entry> entries) {
        JSONArray arr = new JSONArray();
        try {
            for (Entry e : entries) {
                JSONObject o = new JSONObject();
                o.put("name", e.name);
                o.put("uri", e.uri);
                o.put("format", e.format);
                o.put("size", e.size);
                arr.put(o);
            }
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        prefs(ctx).edit()
                .putString(KEY_TREE, tree.toString())
                .putString(KEY_FILES, arr.toString())
                .apply();
    }

    /** Archives in mount order: the preferred format first (first match wins on lookups). */
    static List<Entry> ordered(Context ctx) {
        String first = preferNewClassic(ctx) ? FORMAT_PKGL : FORMAT_PBG3;
        List<Entry> all = load(ctx);
        List<Entry> out = new ArrayList<>(all.size());
        for (Entry e : all) if (e.format.equals(first)) out.add(e);
        for (Entry e : all) if (!e.format.equals(first)) out.add(e);
        return out;
    }

    interface Progress {
        void onFolder(String displayPath);
    }

    /** Walks the picked folder (a few levels deep) and returns every PBG3/PKGL archive. */
    static List<Entry> scanTree(Context ctx, Uri treeUri, Progress progress) {
        ContentResolver cr = ctx.getContentResolver();
        List<Entry> found = new ArrayList<>();
        Deque<String[]> stack = new ArrayDeque<>(); // {documentId, depth, displayPath}
        stack.push(new String[] {DocumentsContract.getTreeDocumentId(treeUri), "0", ""});
        String[] projection = {
                Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE};

        while (!stack.isEmpty()) {
            String[] dir = stack.pop();
            int depth = Integer.parseInt(dir[1]);
            if (progress != null) progress.onFolder(dir[2].isEmpty() ? "/" : dir[2]);
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, dir[0]);
            try (Cursor c = cr.query(children, projection, null, null, null)) {
                if (c == null) continue;
                while (c.moveToNext()) {
                    String id = c.getString(0);
                    String name = c.getString(1);
                    String mime = c.getString(2);
                    long size = c.isNull(3) ? 0 : c.getLong(3);
                    if (name == null) continue;
                    if (Document.MIME_TYPE_DIR.equals(mime)) {
                        if (depth < MAX_DEPTH) {
                            stack.push(new String[] {id, String.valueOf(depth + 1), dir[2] + "/" + name});
                        }
                        continue;
                    }
                    String lower = name.toLowerCase(Locale.ROOT);
                    boolean program = lower.endsWith(".exe") && size >= 256 * 1024 && size <= 4L * 1024 * 1024;
                    boolean music = lower.endsWith(".wav") && lower.startsWith("th06_") && size > 44;
                    if (!program && !music && (!lower.endsWith(".dat") || size < 8)) continue;
                    Uri doc = DocumentsContract.buildDocumentUriUsingTree(treeUri, id);
                    String format = sniff(cr, doc);
                    if (program && !FORMAT_EXE.equals(format)) continue;
                    if (music != FORMAT_WAV.equals(format)) continue;
                    if (format != null) found.add(new Entry(music ? "bgm/" + name : name, doc.toString(), format, size));
                }
            } catch (RuntimeException ignored) {
                // Unreadable folder (provider error): skip it, keep scanning the rest.
            }
        }
        return found;
    }

    private static String sniff(ContentResolver cr, Uri doc) {
        byte[] magic = new byte[4];
        try (InputStream in = cr.openInputStream(doc)) {
            if (in == null) return null;
            int got = 0;
            while (got < 4) {
                int n = in.read(magic, got, 4 - got);
                if (n < 0) return null;
                got += n;
            }
        } catch (IOException | RuntimeException e) {
            return null;
        }
        String s = new String(magic, java.nio.charset.StandardCharsets.US_ASCII);
        if (s.equals(FORMAT_PBG3)) return FORMAT_PBG3;
        if (s.equals(FORMAT_PKGL)) return FORMAT_PKGL;
        if (magic[0] == 'M' && magic[1] == 'Z') return FORMAT_EXE;
        if (s.equals("RIFF")) return FORMAT_WAV;
        return null;
    }
}
