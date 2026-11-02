package io.github.cng605568art.th06;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Choices made in the launcher: the mischief options, and stage select (skip the game's
 * title screen and start at a stage with the shot type and difficulty chosen here, as
 * practice: nothing from that game is saved). Read by the native engine through
 * GameActivity.
 */
final class GameOptions {
    static final String[] SHOT_TYPES = {"Reimu A (homing amulets)", "Reimu B (needles)",
            "Marisa A (missiles)", "Marisa B (laser)"};
    static final String[] DIFFICULTIES = {"Easy", "Normal", "Hard", "Lunatic"};
    static final String[] STAGES = {"Stage 1", "Stage 2", "Stage 3", "Stage 4", "Stage 5",
            "Stage 6", "Extra"};

    private static final String PREFS = "th06_game_options";
    private static final String KEY_SHOT = "shot_type";
    private static final String KEY_DIFFICULTY = "difficulty";
    private static final String KEY_STAGE = "stage";
    private static final String KEY_MISCHIEF = "mischief";
    private static final String KEY_QUICK_START = "quick_start";
    private static final String KEY_CONTROLS = "controls_";

    /** Mischief options (bits, same order as the engine's): not in the original game. */
    static final String[] MISCHIEF = {"Infinite lives", "Infinite bombs", "Invincible (nothing hits you)",
            "Power always MAX", "Unlimited continues"};

    private GameOptions() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static int shotType(Context ctx) { return clamp(prefs(ctx).getInt(KEY_SHOT, 0), 0, 3); }
    static int difficulty(Context ctx) { return clamp(prefs(ctx).getInt(KEY_DIFFICULTY, 1), 0, 3); }
    /** 1..6, 7 = Extra. */
    static int stage(Context ctx) { return clamp(prefs(ctx).getInt(KEY_STAGE, 1), 1, 7); }

    static void setShotType(Context ctx, int v) { prefs(ctx).edit().putInt(KEY_SHOT, v).apply(); }
    static void setDifficulty(Context ctx, int v) { prefs(ctx).edit().putInt(KEY_DIFFICULTY, v).apply(); }
    static void setStage(Context ctx, int v) { prefs(ctx).edit().putInt(KEY_STAGE, v).apply(); }
    static boolean quickStart(Context ctx) { return prefs(ctx).getBoolean(KEY_QUICK_START, false); }
    static void setQuickStart(Context ctx, boolean v) { prefs(ctx).edit().putBoolean(KEY_QUICK_START, v).apply(); }
    static int mischief(Context ctx) { return prefs(ctx).getInt(KEY_MISCHIEF, 0) & 31; }
    static void setMischief(Context ctx, int bit, boolean on) {
        int v = mischief(ctx);
        v = on ? (v | (1 << bit)) : (v & ~(1 << bit));
        prefs(ctx).edit().putInt(KEY_MISCHIEF, v).apply();
    }

    // Touch controls (read by the engine as getControlOptions()).
    static final String[] MOVE_MODES = {"Drag anywhere (touchpad)", "D-pad"};
    static final String[] DRAG_SPEEDS = {"1×", "1.25×", "1.5×", "2×", "2.5×", "3×"};
    static final int[] DRAG_SPEED_PCT = {100, 125, 150, 200, 250, 300};
    static final String[] BUTTON_SIZES = {"Small", "Normal", "Large", "Extra large"};
    static final int[] BUTTON_SIZE_PCT = {80, 100, 120, 140};
    static final String[] OPACITIES = {"Faint", "Light", "Normal", "Solid"};
    static final int[] OPACITY_PCT = {40, 60, 80, 100};

    static int controlChoice(Context ctx, String name, int fallback) { return prefs(ctx).getInt(KEY_CONTROLS + name, fallback); }
    static void setControlChoice(Context ctx, String name, int v) { prefs(ctx).edit().putInt(KEY_CONTROLS + name, v).apply(); }
    static boolean controlFlag(Context ctx, String name, boolean fallback) { return prefs(ctx).getBoolean(KEY_CONTROLS + name, fallback); }
    static void setControlFlag(Context ctx, String name, boolean v) { prefs(ctx).edit().putBoolean(KEY_CONTROLS + name, v).apply(); }

    /** {drag, sensitivity %, button size %, opacity %, left-handed, focus toggle, auto-fire, vibrate}. */
    static int[] controls(Context ctx) {
        int speed = clamp(controlChoice(ctx, "speed", 2), 0, DRAG_SPEED_PCT.length - 1);
        int size = clamp(controlChoice(ctx, "size", 1), 0, BUTTON_SIZE_PCT.length - 1);
        int opacity = clamp(controlChoice(ctx, "opacity", 3), 0, OPACITY_PCT.length - 1);
        return new int[] {
            controlChoice(ctx, "mode", 0) == 0 ? 1 : 0, DRAG_SPEED_PCT[speed], BUTTON_SIZE_PCT[size], OPACITY_PCT[opacity],
            controlFlag(ctx, "left", false) ? 1 : 0, controlFlag(ctx, "focusToggle", false) ? 1 : 0,
            controlFlag(ctx, "autoFire", false) ? 1 : 0, controlFlag(ctx, "vibrate", true) ? 1 : 0,
        };
    }

    // Display (read by the engine as getDisplayOptions()).
    static final String[] LAYOUTS = {"Landscape (sideways)", "Portrait (upright; playfield fills the width)"};
    static final String[] SCALINGS = {"Smooth", "Sharp pixels", "Pixel-perfect (whole multiples)"};

    /** {portrait, scaling (0 smooth, 1 sharp, 2 pixel-perfect), scanlines}. */
    static int[] display(Context ctx) {
        return new int[] {
            controlChoice(ctx, "layout", 0) == 1 ? 1 : 0, clamp(controlChoice(ctx, "scaling", 0), 0, 2),
            controlFlag(ctx, "scanlines", false) ? 1 : 0,
        };
    }

    private static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }
}
