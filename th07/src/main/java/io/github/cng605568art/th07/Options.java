package io.github.cng605568art.th07;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Choices made in the launcher, read by the native engine through GameActivity: the start
 * options (shot type, difficulty, stage, spare lives, practice, skipping the title screen),
 * the mischief options, the touch controls and practice tools, and the display. Kept in this
 * app's own preferences file (with the launcher's language and game-text choice).
 */
final class Options {
    private static final String PREFS = "th07_options";
    private static final String KEY_SHOT = "shot_type";
    private static final String KEY_DIFFICULTY = "difficulty";
    private static final String KEY_STAGE = "stage";
    private static final String KEY_LIVES = "lives";
    private static final String KEY_PRACTICE = "practice";
    private static final String KEY_SKIP_TITLE = "skip_title";
    private static final String KEY_MISCHIEF = "mischief";
    private static final String KEY_CONTROLS = "controls_";

    /** Difficulty indices as the engine numbers them. */
    static final int EXTRA = 4;
    static final int PHANTASM = 5;

    /**
     * Mischief options (not in the original game), as bits in the engine's order: infinite
     * lives, infinite bombs, invincible, power always MAX, unlimited continues.
     */
    static final int MISCHIEF_COUNT = 5;

    private Options() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** 0..5: Reimu A, Reimu B, Marisa A, Marisa B, Sakuya A, Sakuya B. */
    static int shotType(Context ctx) { return clamp(prefs(ctx).getInt(KEY_SHOT, 0), 0, 5); }
    /** 0..5: Easy, Normal, Hard, Lunatic, Extra, Phantasm. */
    static int difficulty(Context ctx) { return clamp(prefs(ctx).getInt(KEY_DIFFICULTY, 1), 0, 5); }
    /** 1..6 (Extra and Phantasm have their own stage). */
    static int stage(Context ctx) { return clamp(prefs(ctx).getInt(KEY_STAGE, 1), 1, 6); }
    /** Spare lives at the start, 0..4 (the game's default is 2). */
    static int lives(Context ctx) { return clamp(prefs(ctx).getInt(KEY_LIVES, 2), 0, 4); }
    static boolean practice(Context ctx) { return prefs(ctx).getBoolean(KEY_PRACTICE, false); }
    /** Start the chosen stage straight away instead of the game's title screen (default off). */
    static boolean skipTitle(Context ctx) { return prefs(ctx).getBoolean(KEY_SKIP_TITLE, false); }

    static void setShotType(Context ctx, int v) { prefs(ctx).edit().putInt(KEY_SHOT, v).apply(); }
    static void setDifficulty(Context ctx, int v) { prefs(ctx).edit().putInt(KEY_DIFFICULTY, v).apply(); }
    static void setStage(Context ctx, int v) { prefs(ctx).edit().putInt(KEY_STAGE, v).apply(); }
    static void setLives(Context ctx, int v) { prefs(ctx).edit().putInt(KEY_LIVES, v).apply(); }
    static void setPractice(Context ctx, boolean v) { prefs(ctx).edit().putBoolean(KEY_PRACTICE, v).apply(); }
    static void setSkipTitle(Context ctx, boolean v) { prefs(ctx).edit().putBoolean(KEY_SKIP_TITLE, v).apply(); }

    /** The mischief bits (see MISCHIEF_COUNT). */
    static int mischief(Context ctx) { return prefs(ctx).getInt(KEY_MISCHIEF, 0) & ((1 << MISCHIEF_COUNT) - 1); }
    static void setMischief(Context ctx, int bit, boolean on) {
        int v = mischief(ctx);
        v = on ? (v | (1 << bit)) : (v & ~(1 << bit));
        prefs(ctx).edit().putInt(KEY_MISCHIEF, v).apply();
    }

    /**
     * GameActivity.getGameOptions(): {shot type 0..5, difficulty 0..5, stage 1..6, spare lives
     * 0..4, practice 0/1, skip the title screen 0/1, game text 0/1, mischief bits}. The engine
     * starts Extra at stage 7 and Phantasm at stage 8; with the 6th value 0 it opens the title
     * screen and the start values are not used. The 7th value is 1 when the game's text comes
     * from a translation (its patch folders: GameActivity.getGameTextStack()).
     */
    static int[] game(Context ctx) {
        return new int[] {shotType(ctx), difficulty(ctx), stage(ctx), lives(ctx), practice(ctx) ? 1 : 0, skipTitle(ctx) ? 1 : 0,
                GameText.stack(ctx).length > 0 ? 1 : 0, mischief(ctx)};
    }

    // ---- touch controls and practice tools (GameActivity.getControlOptions()) ----------

    static final String[] DRAG_SPEEDS = {"1×", "1.25×", "1.5×", "2×", "2.5×", "3×"};
    static final int[] DRAG_SPEED_PCT = {100, 125, 150, 200, 250, 300};
    static final int[] BUTTON_SIZE_PCT = {80, 100, 120, 140};
    static final int[] OPACITY_PCT = {40, 60, 80, 100};
    /** Game speed (R.array.speeds): normal, or slow motion. */
    static final int[] SPEED_PCT = {100, 75, 50};

    static int controlChoice(Context ctx, String name, int fallback) { return prefs(ctx).getInt(KEY_CONTROLS + name, fallback); }
    static void setControlChoice(Context ctx, String name, int v) { prefs(ctx).edit().putInt(KEY_CONTROLS + name, v).apply(); }
    static boolean controlFlag(Context ctx, String name, boolean fallback) { return prefs(ctx).getBoolean(KEY_CONTROLS + name, fallback); }
    static void setControlFlag(Context ctx, String name, boolean v) { prefs(ctx).edit().putBoolean(KEY_CONTROLS + name, v).apply(); }

    /**
     * {drag, sensitivity %, button size %, opacity %, left-handed, focus toggle, auto-fire,
     * vibrate on a miss and a bomb (default on), game speed % (100, 75, 50), show the hitbox,
     * save states}: the engine's ControlOptions.
     */
    static int[] controls(Context ctx) {
        int speed = clamp(controlChoice(ctx, "speed", 2), 0, DRAG_SPEED_PCT.length - 1);
        int size = clamp(controlChoice(ctx, "size", 1), 0, BUTTON_SIZE_PCT.length - 1);
        int opacity = clamp(controlChoice(ctx, "opacity", 3), 0, OPACITY_PCT.length - 1);
        return new int[] {
            controlChoice(ctx, "mode", 0) == 0 ? 1 : 0, DRAG_SPEED_PCT[speed], BUTTON_SIZE_PCT[size], OPACITY_PCT[opacity],
            controlFlag(ctx, "left", false) ? 1 : 0, controlFlag(ctx, "focusToggle", false) ? 1 : 0,
            controlFlag(ctx, "autoFire", false) ? 1 : 0, controlFlag(ctx, "vibrate", true) ? 1 : 0,
            SPEED_PCT[clamp(controlChoice(ctx, "speed%", 0), 0, SPEED_PCT.length - 1)],
            controlFlag(ctx, "hitbox", false) ? 1 : 0, controlFlag(ctx, "saveStates", false) ? 1 : 0,
        };
    }

    // ---- display (GameActivity.getDisplayOptions()) -----------------------------------

    /** {portrait, scaling (0 smooth, 1 sharp, 2 pixel-perfect), scanlines}. */
    static int[] display(Context ctx) {
        return new int[] {
            controlChoice(ctx, "layout", 0) == 1 ? 1 : 0, clamp(controlChoice(ctx, "scaling", 0), 0, 2),
            controlFlag(ctx, "scanlines", false) ? 1 : 0,
        };
    }

    private static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }
}
