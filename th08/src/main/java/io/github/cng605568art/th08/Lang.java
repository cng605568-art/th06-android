package io.github.cng605568art.th08;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Build;
import android.os.LocaleList;

import java.util.Locale;

/**
 * The launcher's own language (Android resources in values, values-ja, values-zh-rCN,
 * values-zh-rTW, values-ms). "" follows the phone's language. The game's own text has its
 * own choice ({@link GameText}), which by default follows this one.
 */
final class Lang {
    static final String[] CODES = {"", "en", "ja", "zh-CN", "zh-TW", "ms"};
    private static final String PREFS = "th08_options";  // (backed up with the other launcher settings)
    private static final String KEY = "language";

    private Lang() {}

    /** The picker's labels: each language in its own words. */
    static String[] labels(Context ctx) {
        return new String[] {ctx.getString(R.string.lang_system), "English", "日本語", "简体中文", "繁體中文", "Bahasa Melayu"};
    }

    static String chosen(Context ctx) {
        String c = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, "");
        for (String known : CODES) if (known.equals(c)) return c;
        return "";
    }

    static int chosenIndex(Context ctx) {
        String c = chosen(ctx);
        for (int i = 0; i < CODES.length; ++i) if (CODES[i].equals(c)) return i;
        return 0;
    }

    static void choose(Context ctx, int index) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString(KEY, CODES[Math.max(0, Math.min(CODES.length - 1, index))]).apply();
    }

    /** The language in effect: the choice, or the phone's language mapped onto ours (else English). */
    static String effective(Context ctx) {
        String c = chosen(ctx);
        if (!c.isEmpty()) return c;
        Locale l = Build.VERSION.SDK_INT >= 24 ? Resources.getSystem().getConfiguration().getLocales().get(0)
                : Locale.getDefault();
        String lang = l.getLanguage();
        if (lang.equals("ja")) return "ja";
        if (lang.equals("ms")) return "ms";
        if (lang.equals("zh")) {
            String script = l.getScript(), country = l.getCountry();
            boolean traditional = script.equals("Hant") || (script.isEmpty()
                    && (country.equals("TW") || country.equals("HK") || country.equals("MO")));
            return traditional ? "zh-TW" : "zh-CN";
        }
        return "en";
    }

    static Locale locale(String code) {
        switch (code) {
            case "ja": return Locale.JAPANESE;
            case "zh-CN": return Locale.SIMPLIFIED_CHINESE;
            case "zh-TW": return Locale.TRADITIONAL_CHINESE;
            case "ms": return new Locale("ms", "MY");
            default: return Locale.ENGLISH;
        }
    }

    /**
     * For attachBaseContext, and for the messages a background task builds: a context whose
     * resources are in the chosen language (the context itself when following the phone).
     */
    static Context wrap(Context base) {
        String c = chosen(base);
        if (c.isEmpty()) return base;
        Locale l = locale(c);
        Configuration config = new Configuration(base.getResources().getConfiguration());
        if (Build.VERSION.SDK_INT >= 24) config.setLocales(new LocaleList(l));
        else config.setLocale(l);
        return base.createConfigurationContext(config);
    }
}
