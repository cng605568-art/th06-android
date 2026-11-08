package io.github.cng605568art.th07;

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
import java.util.ArrayList;
import java.util.List;

/**
 * Hosts the SDL2 native engine (libmain.so). The static methods below are called from
 * native code over JNI (engine/native/src/platform/AndroidStorage.cpp, and th07's start
 * options in Th07Game.cpp) to reach the game files imported in {@link LauncherActivity}.
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "th07";

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
     * The imported game files, flattened as {name, path, format, ...}: th07.dat (an archive),
     * th07.exe (the program the engine reads its tables from) and thbgm.dat (the music, a
     * loose file). The engine checks each file's contents itself.
     */
    public static String[] getDataSources() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new String[0];
        String[][] files = {
            {GameFiles.DAT, "PBG4"},
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
     * skip the title screen}.
     */
    public static int[] getGameOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {0, 1, 1, 2, 0, 0};
        return Options.game(ctx);
    }

    /** The launcher's display settings: {portrait, scaling, scanlines}. */
    public static int[] getDisplayOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {0, 0, 0};
        return Options.display(ctx);
    }

    /** The launcher's touch-control settings, see Options.controls(). */
    public static int[] getControlOptions() {
        Context ctx = SDL.getContext();
        if (ctx == null) return new int[] {1, 150, 100, 100, 0, 0, 0, 1, 100, 0, 0};
        return Options.controls(ctx);
    }

    /** A short vibration, if the device has a vibrator. */
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
}
