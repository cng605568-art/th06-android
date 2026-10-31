package io.github.cng605568art.th06;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

import java.util.List;

/**
 * Hosts the SDL2 native engine (libmain.so). The static methods below are called from
 * native code over JNI (see platform/AndroidStorage.cpp) to reach the game archives the
 * player selected in {@link ImportActivity}.
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "th06";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
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
