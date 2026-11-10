package io.github.cng605568art.th08;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

import java.io.File;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Hosts the SDL2 native engine (libmain.so). The static methods below are called from
 * native code over JNI (engine/native/src/platform/AndroidStorage.cpp, and th08's start
 * options in Th08Game.cpp) to reach the game files imported in {@link LauncherActivity}.
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "th08";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Upright or sideways, as chosen in the launcher (before the window exists).
        setRequestedOrientation(Options.display(this)[0] != 0 ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(savedInstanceState);
        requestSixtyHertz();
    }

    /**
     * The game advances exactly one tick per 1/60 s. On 90/120/144 Hz phones, ask for a
     * 60 Hz (or exact-multiple) display mode so every tick lands on a vsync.
     */
    @SuppressWarnings("deprecation")
    private void requestSixtyHertz() {
        Display display = Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
        if (display == null) return;
        Display.Mode current = display.getMode();
        Display.Mode best = null;
        float bestScore = Float.MAX_VALUE;
        for (Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight()) {
                continue;
            }
            float hz = m.getRefreshRate();
            float ratio = hz / 60f;
            float score;
            if (Math.abs(hz - 60f) < 1f) {
                score = 0f;
            } else if (Math.abs(ratio - Math.round(ratio)) < 0.02f) {
                score = 1f + hz / 1000f;  // 120 Hz beats 180 Hz
            } else {
                score = 10f + Math.abs(hz - 60f) / 1000f;
            }
            if (score < bestScore) {
                bestScore = score;
                best = m;
            }
        }
        if (best != null && best.getModeId() != current.getModeId()) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
            Log.i(TAG, "Requested display mode " + best.getRefreshRate() + " Hz");
        }
    }

    // ---- Called from native code (JNI) -------------------------------------------------

    /**
     * The imported game files, flattened as {name, path, format, ...}: th08.dat (an archive),
     * th08.exe (the program the engine reads its tables from) and thbgm.dat (the music, a
     * loose file). The engine checks each file's contents itself.
     */
    public static String[] getDataSources() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new String[0];
        String[][] files = {
            {GameFiles.DAT, "PBGZ"},
            {GameFiles.EXE, "EXE"},
            {GameFiles.BGM, "ZWAV"},
        };
        List<String> out = new ArrayList<>();
        for (String[] f : files) {
            File file = GameFiles.file(ctx, f[0]);
            if (!file.isFile()) continue;
            out.add(f[0]);
            out.add(file.getAbsolutePath());
            out.add(f[1]);
        }
        return out.toArray(new String[0]);
    }

    /** Opens one imported file read-only; ownership of the fd passes to native code. */
    public static int openDataSource(String location) {
        Context ctx = SDL.getContext();
        if (ctx == null || location == null) return -1;
        try {
            ParcelFileDescriptor pfd = location.startsWith("/")
                    ? ParcelFileDescriptor.open(new File(location), ParcelFileDescriptor.MODE_READ_ONLY)
                    : ctx.getContentResolver().openFileDescriptor(Uri.parse(location), "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Log.w(TAG, "Cannot open " + location + ": " + e);
            return -1;
        }
    }

    /**
     * Launcher choices: {shot type 0..5, difficulty 0..5, stage 1..6, spare lives 0..4, practice,
     * skip the title screen, game text on (getGameTextStack() is not empty), mischief bits (bit
     * 0 infinite lives, 1 infinite bombs, 2 invincible, 3 power always MAX, 4 unlimited
     * continues)}. See Options.game().
     */
    public static int[] getGameOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {0, 1, 1, 2, 0, 0, 0, 0};
        return Options.game(ctx);
    }

    /**
     * The game-text translation's patch folders (absolute, each ending in "/"), lowest priority
     * first, e.g. .../files/thcrap/repos/nmlgc/base_tsa/ ... .../repos/thpatch/lang_en/; an
     * empty array means the original Japanese text. See GameText.
     */
    public static String[] getGameTextStack() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new String[0];
        return GameText.stack(ctx);
    }

    /** The launcher's display settings: {portrait, scaling, scanlines}. */
    public static int[] getDisplayOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {0, 0, 0};
        return Options.display(ctx);
    }

    /**
     * The launcher's touch-control settings and practice tools, 11 values: {drag, sensitivity %,
     * button size %, opacity %, left-handed, focus toggle, auto-fire, vibrate, game speed %,
     * show hitbox, save states}. See Options.controls().
     */
    public static int[] getControlOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {1, 150, 100, 100, 0, 0, 0, 1, 100, 0, 0};
        return Options.controls(ctx);
    }

    /** A short vibration (a miss or a bomb), if the device has a vibrator. */
    @SuppressWarnings("deprecation")
    public static void vibrate(int milliseconds) {
        Context ctx = SDL.getContext();
        if (ctx == null) return;
        Vibrator v = (Vibrator) ctx.getSystemService(Context.VIBRATOR_SERVICE);
        if (v == null || !v.hasVibrator()) return;
        if (Build.VERSION.SDK_INT >= 26) {
            v.vibrate(VibrationEffect.createOneShot(Math.max(1, milliseconds), VibrationEffect.DEFAULT_AMPLITUDE));
        } else {
            v.vibrate(Math.max(1, milliseconds));
        }
    }

    private static Charset sjis;
    private static Paint textPaint;

    /**
     * Renders a Shift-JIS string from the game's files into a width x height ARGB band, the
     * way the original lays it out: every byte is (fontSize + 1) / 2 pixels wide, so a
     * full-width character takes two cells; a shadow sits 1.5 px right, 1 px down.
     */
    public static synchronized int[] renderText(byte[] text, int fontSize, int rgb, int shadow, float startX,
                                                int width, int height) {
        if (sjis == null) {
            try {
                sjis = Charset.forName("windows-31j");
            } catch (Exception e) {
                sjis = Charset.forName("Shift_JIS");
            }
            textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            textPaint.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        }
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        float cell = (fontSize + 1) / 2f;
        textPaint.setTextSize(cell * 2f * 0.92f);
        float baseline = height * 0.8f;
        float x = startX;
        for (int i = 0; i < text.length; ) {
            int b = text[i] & 0xff;
            int n = ((b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc)) && i + 1 < text.length ? 2 : 1;
            String ch = new String(text, i, n, sjis);
            if (!ch.trim().isEmpty()) {
                float w = textPaint.measureText(ch);
                float dx = x + Math.max(0f, (cell * n - w) / 2f);
                textPaint.setColor(0xff000000 | shadow);
                canvas.drawText(ch, dx + 1.5f, baseline + 1f, textPaint);
                textPaint.setColor(0xff000000 | rgb);
                canvas.drawText(ch, dx, baseline, textPaint);
            }
            x += cell * n;
            i += n;
        }
        int[] pixels = new int[width * height];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        bitmap.recycle();
        return pixels;
    }

    private static Paint utf8Paint;
    private static final int ITALIC = 16;  // a flag bit next to the alignment (0 left, 1 centre, 2 right)

    /**
     * The paint for translated (UTF-8) text: a condensed sans-serif for Latin text, the
     * system's CJK font when the text has kana or kanji (an ideographic space alone, as the
     * English patch's endings indent with, does not count), in the game-text language's glyph
     * style (Chinese text in Chinese shapes); a slant for italics.
     */
    private static Paint utf8Paint(String text, int fontSize, boolean italic) {
        if (utf8Paint == null) utf8Paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        Paint p = utf8Paint;
        p.setTextLocale(GameText.textLocale(SDL.getContext()));
        boolean cjk = false;
        for (int i = 0; i < text.length() && !cjk; ++i) {
            char c = text.charAt(i);
            cjk = (c >= 0x3001 && c <= 0x30ff) || (c >= 0x3400 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff)
                    || (c >= 0xff01 && c <= 0xff9f) || (c >= 0xac00 && c <= 0xd7af);
        }
        p.setTypeface(cjk ? Typeface.create(Typeface.DEFAULT, Typeface.BOLD) : Typeface.create("sans-serif-condensed", Typeface.BOLD));
        float cell = (fontSize + 1) / 2f;
        p.setTextSize(cell * 2f * (cjk ? 0.92f : 0.95f));
        p.setTextScaleX(1f);
        p.setTextSkewX(italic ? -0.25f : 0f);
        return p;
    }

    /**
     * Renders translated UTF-8 text (curly quotes, accents, ideographic spaces and all) into a
     * width x height ARGB band, aligned within [left, left + room) and narrowed to fit; the
     * extra last int is the width drawn x 256.
     */
    public static synchronized int[] renderTextUtf8(byte[] utf8, int fontSize, int rgb, int shadow, float left, float room,
                                                    int align, int width, int height) {
        String text = new String(utf8, StandardCharsets.UTF_8);
        Paint p = utf8Paint(text, fontSize, (align & ITALIC) != 0);
        align &= 3;
        float full = p.measureText(text);
        float drawn = Math.min(full, room);
        if (full > room && full > 0) p.setTextScaleX(room / full);
        float x = align == 2 ? left + room - drawn : align == 1 ? left + (room - drawn) / 2f : left;
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        float baseline = height * 0.8f;
        p.setColor(0xff000000 | shadow);
        canvas.drawText(text, x + 1.5f, baseline + 1f, p);
        p.setColor(0xff000000 | rgb);
        canvas.drawText(text, x, baseline, p);
        int[] pixels = new int[width * height + 1];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        bitmap.recycle();
        pixels[width * height] = Math.round(drawn * 256f);
        return pixels;
    }

    /** The natural width of translated text as renderTextUtf8 would draw it (laying out markup). */
    public static synchronized float measureTextUtf8(byte[] utf8, int fontSize, int flags) {
        String text = new String(utf8, StandardCharsets.UTF_8);
        return utf8Paint(text, fontSize, (flags & ITALIC) != 0).measureText(text);
    }
}
