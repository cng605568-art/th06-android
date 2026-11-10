package io.github.cng605568art.th08;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * Looks up the newest full release on GitHub (only the release's version and page; the player
 * downloads and installs the APK themselves from that page). Both apps are released together
 * from the same repository with one version number (the VERSION file), so this compares the
 * same way the th06 app does. Nothing about the player or the device is sent beyond what any
 * web request carries.
 */
final class UpdateChecker {
    static final String REPO = "cng605568-art/th06-android";
    static final String RELEASES_PAGE = "https://github.com/" + REPO + "/releases";
    static final String ISSUES_PAGE = "https://github.com/" + REPO + "/issues";
    // "latest": the newest release that is neither a draft nor a pre-release.
    private static final String LATEST_API = "https://api.github.com/repos/" + REPO + "/releases/latest";
    private static final String PREFS = "th08_updates";
    private static final long DAY_MS = 24L * 60 * 60 * 1000;

    /** What a check found. */
    static final class Result {
        final String latest;   // e.g. "1.0.0"; null when nothing is published yet
        final String page;     // the release's web page
        final boolean newer;   // newer than this app

        Result(String latest, String page, boolean newer) {
            this.latest = latest;
            this.page = page;
            this.newer = newer;
        }
    }

    private UpdateChecker() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static boolean autoCheck(Context ctx) { return prefs(ctx).getBoolean("auto", true); }
    static void setAutoCheck(Context ctx, boolean on) { prefs(ctx).edit().putBoolean("auto", on).apply(); }

    /** True once a day while automatic checks are on. */
    static boolean due(Context ctx) {
        return autoCheck(ctx) && System.currentTimeMillis() - prefs(ctx).getLong("last", 0) > DAY_MS;
    }

    /** The last check's result (no network), or null. */
    static Result cached(Context ctx) {
        SharedPreferences p = prefs(ctx);
        String latest = p.getString("latest", null);
        if (latest == null) return null;
        return new Result(latest, p.getString("page", RELEASES_PAGE), isNewer(latest, Logs.versionName(ctx)));
    }

    /** Asks GitHub (blocking; call off the UI thread). */
    static Result check(Context ctx) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(LATEST_API).openConnection();
        try {
            c.setConnectTimeout(10000);
            c.setReadTimeout(15000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("User-Agent", "th08-android/" + Logs.versionName(ctx));
            int code = c.getResponseCode();
            prefs(ctx).edit().putLong("last", System.currentTimeMillis()).apply();
            if (code == 404) {  // no release published yet
                prefs(ctx).edit().remove("latest").remove("page").apply();
                return new Result(null, RELEASES_PAGE, false);
            }
            if (code != 200) throw new IOException("GitHub answered " + code);
            String body;
            try (InputStream in = c.getInputStream()) {
                body = readAll(in);
            }
            JSONObject json = new JSONObject(body);
            String tag = json.optString("tag_name", "");
            String latest = tag.startsWith("v") ? tag.substring(1) : tag;
            String page = json.optString("html_url", RELEASES_PAGE);
            if (latest.isEmpty()) throw new IOException("the release has no version");
            prefs(ctx).edit().putString("latest", latest).putString("page", page).apply();
            return new Result(latest, page, isNewer(latest, Logs.versionName(ctx)));
        } catch (JSONException e) {
            throw new IOException("unexpected answer from GitHub");
        } finally {
            c.disconnect();
        }
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            if (out.size() > (1 << 20)) throw new IOException("answer too large");
        }
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    /**
     * Semantic-version order: "1.2.0" > "1.1.9"; a release beats its own pre-releases and dev
     * builds ("1.0.0" > "1.0.0-dev+abc1234" and "1.0.0-beta"); build metadata is ignored.
     */
    static boolean isNewer(String candidate, String current) {
        return compare(candidate, current) > 0;
    }

    static int compare(String a, String b) {
        String[] pa = split(a), pb = split(b);
        String[] na = pa[0].split("\\."), nb = pb[0].split("\\.");
        for (int i = 0; i < Math.max(na.length, nb.length); ++i) {
            long x = i < na.length ? number(na[i]) : 0, y = i < nb.length ? number(nb[i]) : 0;
            if (x != y) return x < y ? -1 : 1;
        }
        if (pa[1].isEmpty() != pb[1].isEmpty()) return pa[1].isEmpty() ? 1 : -1;  // release > pre-release
        return Integer.signum(pa[1].compareTo(pb[1]));
    }

    /** {core, pre-release} without build metadata. */
    private static String[] split(String v) {
        if (v == null) v = "";
        int plus = v.indexOf('+');
        if (plus >= 0) v = v.substring(0, plus);
        int dash = v.indexOf('-');
        return dash >= 0 ? new String[] {v.substring(0, dash), v.substring(dash + 1)} : new String[] {v, ""};
    }

    private static long number(String s) {
        try {
            return Long.parseLong(s.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }
}
