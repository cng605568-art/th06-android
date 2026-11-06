package io.github.cng605568art.th06;

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

import java.nio.charset.Charset;
import java.util.List;

/**
 * Hosts the SDL2 native engine (libmain.so). The static methods below are called from
 * native code over JNI (engine/native/src/platform/AndroidStorage.cpp, and th06's own
 * options in Th06Game.cpp) to reach the game archives the player selected in
 * {@link ImportActivity}.
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "th06";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Upright or sideways, as chosen in the launcher (before the window exists).
        setRequestedOrientation(GameOptions.display(this)[0] != 0 ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
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
        Display display = Build.VERSION.SDK_INT >= 30 ? getDisplay()
                : getWindowManager().getDefaultDisplay();
        if (display == null) return;
        Display.Mode current = display.getMode();
        Display.Mode best = null;
        float bestScore = Float.MAX_VALUE;
        for (Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() != current.getPhysicalWidth()
                    || m.getPhysicalHeight() != current.getPhysicalHeight()) {
                continue;
            }
            float hz = m.getRefreshRate();
            float ratio = hz / 60f;
            float score;
            if (Math.abs(hz - 60f) < 1f) {
                score = 0f;
            } else if (Math.abs(ratio - Math.round(ratio)) < 0.02f) {
                score = 1f + hz / 1000f;   // 120 Hz beats 180 Hz
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

    /** Archives in mount order, flattened as {name, uri, format, name, uri, format, ...}. */
    public static String[] getDataSources() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new String[0];
        List<DataSources.Entry> list = DataSources.ordered(ctx);
        String[] out = new String[list.size() * 3];
        for (int i = 0; i < list.size(); i++) {
            DataSources.Entry e = list.get(i);
            out[i * 3] = e.name;
            out[i * 3 + 1] = e.uri;
            out[i * 3 + 2] = e.format;
        }
        return out;
    }

    private static Charset sjis;
    private static Paint textPaint;

    /**
     * Renders a Shift-JIS string from the game's files into a width x height ARGB band, the
     * way the original lays it out: every byte is (fontSize + 1) / 2 pixels wide, so a
     * full-width character takes two cells; a black shadow sits 1.5 px right, 1 px down.
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

    /**
     * Renders translated (UTF-8) game text: a proportional bold face (condensed for Latin
     * text, the system's CJK face for Chinese and Japanese, in the game-text language's
     * glyph style), aligned in [left, left + room) and narrowed to fit; the black shadow as
     * in renderText. The last int is the width drawn x 256.
     */
    public static synchronized int[] renderTextUtf8(byte[] utf8, int fontSize, int rgb, int shadow, float left, float room,
                                                    int align, int width, int height) {
        String text = new String(utf8, java.nio.charset.StandardCharsets.UTF_8);
        if (utf8Paint == null) utf8Paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        Paint p = utf8Paint;
        boolean cjk = false;
        for (int i = 0; i < text.length() && !cjk; ++i) cjk = text.charAt(i) >= 0x2e80;
        p.setTypeface(cjk ? Typeface.create(Typeface.DEFAULT, Typeface.BOLD) : Typeface.create("sans-serif-condensed", Typeface.BOLD));
        p.setTextLocale(GameText.textLocale(SDL.getContext()));
        float cell = (fontSize + 1) / 2f;
        p.setTextSize(cell * 2f * (cjk ? 0.92f : 0.95f));
        p.setTextScaleX(1f);
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

    /** The game-text translation's patch folders, lowest priority first (none: the original). */
    public static String[] getGameTextStack() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new String[0];
        return GameText.stack(ctx);
    }

    /** Launcher choices: {shot type 0..3, difficulty 0..3, stage 1..7 (7 = Extra)}. */
    /** The launcher's display settings: {portrait, scaling, scanlines}. */
    public static int[] getDisplayOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {0, 0, 0};
        return GameOptions.display(ctx);
    }

    /** The launcher's touch-control settings, see GameOptions.controls(). */
    public static int[] getControlOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {1, 150, 100, 100, 0, 0, 0, 1, 100, 0, 0};
        return GameOptions.controls(ctx);
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

    public static int[] getGameOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {0, 1, 1, 0, 0};
        return new int[] {GameOptions.shotType(ctx), GameOptions.difficulty(ctx), GameOptions.stage(ctx),
                GameOptions.mischief(ctx), GameOptions.quickStart(ctx) ? 1 : 0};
    }

    /** Opens one selected archive read-only; ownership of the fd passes to native code. */
    public static int openDataSource(String uri) {
        Context ctx = SDL.getContext();
        if (ctx == null) return -1;
        try {
            ParcelFileDescriptor pfd = ctx.getContentResolver()
                    .openFileDescriptor(Uri.parse(uri), "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Log.w(TAG, "Cannot open " + uri + ": " + e);
            return -1;
        }
    }
}
