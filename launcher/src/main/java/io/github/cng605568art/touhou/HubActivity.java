package io.github.cng605568art.touhou;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.function.Predicate;

/**
 * Touhou Launcher's first screen: one card per game. A card opens that game's own launcher
 * (game files, options, translation, music, backups), exactly as in its standalone app; the
 * games' files and settings stay apart (GameStorage in each game).
 */
public class HubActivity extends Activity {
    private static final int INK = 0xFF120F1C;
    private static final int PAPER = 0xFFF6EFE2;
    private static final int PAPER_MUTED = 0xFFB9B0C4;
    private static final int READY = 0xFF7EE0A1;
    private static final int NEEDS = 0xFFFFC970;

    /** One game: its code name, titles, colours (deep, mid, accent), icon, screen, readiness. */
    private static final class Game {
        final String code;
        final int title, subtitle, deep, mid, accent, icon;
        final Class<? extends Activity> screen;
        final Predicate<Context> ready;
        TextView status;

        Game(String code, int title, int subtitle, int deep, int mid, int accent, int icon, Class<? extends Activity> screen,
             Predicate<Context> ready) {
            this.code = code;
            this.title = title;
            this.subtitle = subtitle;
            this.deep = deep;
            this.mid = mid;
            this.accent = accent;
            this.icon = icon;
            this.screen = screen;
            this.ready = ready;
        }
    }

    private final Game[] games = {
        new Game("TH06", R.string.hub_th06_title, R.string.hub_th06_subtitle, 0xFF5E0B12, 0xFFC62828, 0xFFFFD3E0,
                io.github.cng605568art.th06.R.mipmap.th06_ic_launcher, io.github.cng605568art.th06.ImportActivity.class,
                io.github.cng605568art.th06.GameStorage::ready),
        new Game("TH07", R.string.hub_th07_title, R.string.hub_th07_subtitle, 0xFF2A1838, 0xFF4A2A5E, 0xFFFCE4EC,
                io.github.cng605568art.th07.R.drawable.th07_ic_launcher, io.github.cng605568art.th07.LauncherActivity.class,
                io.github.cng605568art.th07.GameStorage::ready),
        new Game("TH08", R.string.hub_th08_title, R.string.hub_th08_subtitle, 0xFF141838, 0xFF262E6B, 0xFFFBF1D8,
                io.github.cng605568art.th08.R.drawable.th08_ic_launcher, io.github.cng605568art.th08.LauncherActivity.class,
                io.github.cng605568art.th08.GameStorage::ready),
    };

    private final Handler main = new Handler(Looper.getMainLooper());
    private final Typeface serif = Typeface.create(Typeface.SERIF, Typeface.BOLD);
    private final Typeface sans = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        drawEdgeToEdge();

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] {0xFF221C33, INK, INK}));
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER_HORIZONTAL);
        scroll.addView(page, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // The header: the orb, the name, what to do.
        ImageView orb = new ImageView(this);
        orb.setImageResource(R.drawable.hub_ic_launcher);
        LinearLayout.LayoutParams orbLp = new LinearLayout.LayoutParams(dp(72), dp(72));
        orbLp.topMargin = dp(36);
        page.addView(orb, orbLp);
        TextView name = text(getString(R.string.hub_app_name), 30, PAPER, serif);
        name.setShadowLayer(dp(6), 0, dp(2), 0x80000000);
        name.setPadding(0, dp(12), 0, 0);
        page.addView(name, wrap());
        TextView choose = text(getString(R.string.hub_choose), 15, PAPER_MUTED, sans);
        choose.setPadding(0, dp(4), 0, dp(20));
        page.addView(choose, wrap());

        // The cards, one column, at most 520 dp wide.
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams colLp = new LinearLayout.LayoutParams(Math.min(getResources().getDisplayMetrics().widthPixels - dp(32), dp(520)),
                ViewGroup.LayoutParams.WRAP_CONTENT);
        page.addView(column, colLp);
        for (Game g : games) column.addView(card(g), cardLp());

        TextView note = text(getString(R.string.hub_note), 13, PAPER_MUTED, sans);
        note.setGravity(Gravity.CENTER_HORIZONTAL);
        note.setPadding(dp(24), dp(20), dp(24), dp(4));
        page.addView(note, wrap());
        TextView version = text(getString(R.string.hub_version, versionName()), 12, 0xFF7E7590, sans);
        version.setPadding(0, 0, 0, dp(24));
        page.addView(version, wrap());

        page.setOnApplyWindowInsetsListener((v, insets) -> {
            int top, bottom, left, right;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                top = bars.top;
                bottom = bars.bottom;
                left = bars.left;
                right = bars.right;
            } else {
                top = insets.getSystemWindowInsetTop();
                bottom = insets.getSystemWindowInsetBottom();
                left = insets.getSystemWindowInsetLeft();
                right = insets.getSystemWindowInsetRight();
            }
            v.setPadding(left, top, right, bottom);
            return insets;
        });
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();  // (files may have been added in a game's launcher)
    }

    /** Each game's "ready" line, worked out away from the UI thread (it reads the game files). */
    private void refreshStatus() {
        final Context app = getApplicationContext();
        new Thread(() -> {
            for (Game g : games) {
                boolean ok;
                try {
                    ok = g.ready.test(app);
                } catch (RuntimeException e) {
                    ok = false;
                }
                final boolean ready = ok;
                main.post(() -> showStatus(g, ready));
            }
        }, "hub-status").start();
    }

    private void showStatus(Game g, boolean ready) {
        if (g.status == null) return;
        g.status.setText(ready ? R.string.hub_ready : R.string.hub_needs_files);
        g.status.setTextColor(ready ? READY : NEEDS);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(ready ? READY : NEEDS);
        dot.setSize(dp(8), dp(8));
        g.status.setCompoundDrawablesRelativeWithIntrinsicBounds(dot, null, null, null);
        g.status.setCompoundDrawablePadding(dp(6));
    }

    private View card(Game g) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(16), dp(16), dp(16), dp(16));
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {g.deep, g.mid});
        bg.setCornerRadius(dp(20));
        bg.setStroke(dp(1), 0x33FFFFFF);
        GradientDrawable mask = new GradientDrawable();
        mask.setColor(Color.WHITE);
        mask.setCornerRadius(dp(20));
        card.setBackground(new RippleDrawable(ColorStateList.valueOf(0x40FFFFFF), bg, mask));
        card.setElevation(dp(4));
        card.setClickable(true);
        card.setFocusable(true);
        card.setOnClickListener(v -> startActivity(new Intent(this, g.screen)));

        ImageView icon = new ImageView(this);
        Drawable d = getDrawable(g.icon);
        icon.setImageDrawable(d);
        card.addView(icon, new LinearLayout.LayoutParams(dp(64), dp(64)));

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.setPadding(dp(16), 0, 0, 0);
        TextView code = text(g.code, 12, g.accent, Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
        code.setLetterSpacing(0.15f);
        texts.addView(code);
        texts.addView(text(getString(g.title), 24, Color.WHITE, serif));
        TextView sub = text(getString(g.subtitle), 14, g.accent, sans);
        sub.setAlpha(0.9f);
        texts.addView(sub);
        g.status = text("", 13, PAPER_MUTED, sans);
        g.status.setPadding(0, dp(8), 0, 0);
        texts.addView(g.status);
        card.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView arrow = text("›", 32, 0xB3FFFFFF, sans);
        card.addView(arrow);
        return card;
    }

    private LinearLayout.LayoutParams cardLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(14);
        return lp;
    }

    private static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private TextView text(String s, float sp, int color, Typeface face) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(face);
        return t;
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private String versionName() {
        try {
            PackageInfo p = getPackageManager().getPackageInfo(getPackageName(), 0);
            return p.versionName != null ? p.versionName : "?";
        } catch (Exception e) {
            return "?";
        }
    }

    @SuppressWarnings("deprecation")
    private void drawEdgeToEdge() {
        Window w = getWindow();
        w.setStatusBarColor(Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }
}
