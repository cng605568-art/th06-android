package io.github.cng605568art.th06;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.function.IntConsumer;

/**
 * The launcher's look: shrine red and sakura pink on warm paper, white cards, a brush-style
 * Japanese typeface (Yuji Syuku) for titles and buttons and an antique Mincho (Zen Antique)
 * for text. Both fonts are subsets of SIL OFL 1.1 fonts (licences in assets/fonts).
 */
final class LauncherUi {
    static final int RED = 0xFFC62828;
    static final int RED_DARK = 0xFF8E1B1B;
    static final int RED_DEEP = 0xFF5E0B12;
    static final int PINK = 0xFFFF5C8D;
    static final int PINK_SOFT = 0xFFFFD3E0;
    static final int PINK_WASH = 0xFFFFF0F4;
    static final int PAPER = 0xFFFFF7F5;
    static final int INK = 0xFF2B1B1E;
    static final int MUTED = 0xFF7D5E66;
    static final int CARD_LINE = 0xFFF6CFDA;

    final Context ctx;
    final Typeface brush;
    final Typeface serif;

    LauncherUi(Context ctx) {
        this.ctx = ctx;
        this.brush = load(ctx, "fonts/YujiSyuku-Subset.ttf", Typeface.SERIF);
        this.serif = load(ctx, "fonts/ZenAntique-Subset.ttf", Typeface.SERIF);
    }

    private static Typeface load(Context ctx, String asset, Typeface fallback) {
        try {
            return Typeface.createFromAsset(ctx.getAssets(), asset);
        } catch (RuntimeException e) {
            return fallback;
        }
    }

    int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, ctx.getResources().getDisplayMetrics()));
    }

    static LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    LinearLayout.LayoutParams matchWrap(int top, int bottom) {
        LinearLayout.LayoutParams lp = matchWrap();
        lp.topMargin = dp(top);
        lp.bottomMargin = dp(bottom);
        return lp;
    }

    static GradientDrawable rounded(int fill, float radiusPx) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(radiusPx);
        return g;
    }

    // ---- text ----------------------------------------------------------------------

    TextView title(String text, float sp, int color) {
        TextView t = new TextView(ctx);
        t.setText(text);
        t.setTypeface(brush);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setLetterSpacing(-0.04f);  // the brush face's Latin letters are set wide
        t.setIncludeFontPadding(false);
        return t;
    }

    TextView body(String text, float sp, int color) {
        TextView t = new TextView(ctx);
        t.setText(text);
        t.setTypeface(serif);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setLineSpacing(dp(2), 1.0f);
        return t;
    }

    /** A small caption above a control. */
    TextView caption(String text) {
        TextView t = body(text, 13, MUTED);
        t.setPadding(dp(2), dp(12), 0, dp(4));
        t.setLetterSpacing(0.02f);
        return t;
    }

    /** The monospaced file list, on a pink wash. */
    TextView statusBox() {
        TextView t = new TextView(ctx);
        t.setTypeface(Typeface.MONOSPACE);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        t.setTextColor(INK);
        t.setLineSpacing(dp(1), 1.0f);
        t.setPadding(dp(12), dp(10), dp(12), dp(10));
        GradientDrawable bg = rounded(PINK_WASH, dp(12));
        bg.setStroke(dp(1), CARD_LINE);
        t.setBackground(bg);
        t.setTextIsSelectable(true);
        return t;
    }

    // ---- containers --------------------------------------------------------------------

    /** A white card with a section title (brush, red) and an optional subtitle. */
    LinearLayout card(LinearLayout page, String heading, String subtitle) {
        LinearLayout card = new LinearLayout(ctx);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(16), dp(18), dp(18));
        GradientDrawable bg = rounded(Color.WHITE, dp(22));
        bg.setStroke(dp(1), CARD_LINE);
        card.setBackground(bg);
        card.setElevation(dp(3));
        LinearLayout.LayoutParams lp = matchWrap();
        lp.setMargins(dp(16), dp(8), dp(16), dp(10));
        page.addView(card, lp);

        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        View bar = new View(ctx);  // a short red-to-pink stroke before the title
        GradientDrawable stroke = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] {RED, PINK});
        stroke.setCornerRadius(dp(3));
        bar.setBackground(stroke);
        LinearLayout.LayoutParams barLp = new LinearLayout.LayoutParams(dp(5), dp(24));
        barLp.rightMargin = dp(10);
        row.addView(bar, barLp);
        row.addView(title(heading, 22, RED), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(row, matchWrap());
        if (subtitle != null) {
            TextView sub = body(subtitle, 14, MUTED);
            sub.setPadding(0, dp(6), 0, dp(2));
            card.addView(sub, matchWrap());
        }
        return card;
    }

    // ---- controls ------------------------------------------------------------------------

    private Drawable buttonBackground(int fill, int stroke, int disabledFill, float radius) {
        GradientDrawable normal = rounded(fill, radius);
        if (stroke != 0) normal.setStroke(dp(1.5f), stroke);
        GradientDrawable disabled = rounded(disabledFill, radius);
        if (stroke != 0) disabled.setStroke(dp(1.5f), 0xFFE8D5DA);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {-android.R.attr.state_enabled}, disabled);
        states.addState(new int[] {}, normal);
        return new RippleDrawable(ColorStateList.valueOf(stroke != 0 ? 0x33FF5C8D : 0x55FFFFFF), states, rounded(Color.WHITE, radius));
    }

    private static ColorStateList textColors(int enabled, int disabled) {
        return new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}}, new int[] {disabled, enabled});
    }

    private Button button(String text, float sp) {
        Button b = new Button(ctx);
        b.setText(text);
        b.setAllCaps(false);
        b.setTypeface(brush);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        b.setLetterSpacing(-0.02f);
        b.setStateListAnimator(null);
        b.setMinHeight(dp(52));
        b.setPadding(dp(16), dp(10), dp(16), dp(10));
        return b;
    }

    /** Filled shrine red, white text. */
    Button primary(String text) {
        Button b = button(text, 18);
        b.setBackground(buttonBackground(RED, 0, 0xFFE9C2C6, dp(26)));
        b.setTextColor(textColors(Color.WHITE, 0xFFFFF5F7));
        b.setElevation(dp(2));
        return b;
    }

    /** The big Play button: a red-to-pink sweep. */
    Button hero(String text) {
        Button b = button(text, 24);
        b.setMinHeight(dp(64));
        GradientDrawable sweep = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {RED_DARK, RED, PINK});
        sweep.setCornerRadius(dp(32));
        GradientDrawable disabled = rounded(0xFFE9C2C6, dp(32));
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {-android.R.attr.state_enabled}, disabled);
        states.addState(new int[] {}, sweep);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x66FFFFFF), states, rounded(Color.WHITE, dp(32))));
        b.setTextColor(textColors(Color.WHITE, 0xFFFFF5F7));
        b.setElevation(dp(6));
        b.setShadowLayer(dp(2), 0, dp(1), 0x66000000);
        return b;
    }

    /** White with a pink outline, red text. */
    Button secondary(String text) {
        Button b = button(text, 17);
        b.setBackground(buttonBackground(Color.WHITE, PINK, 0xFFFBF5F6, dp(26)));
        b.setTextColor(textColors(RED, 0xFFC9AEB4));
        return b;
    }

    CheckBox check(String text, boolean checked) {
        CheckBox c = new CheckBox(ctx);
        c.setText(text);
        c.setChecked(checked);
        c.setTypeface(serif);
        c.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        c.setTextColor(INK);
        c.setButtonTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {PINK, 0xFFC9A3AD}));
        c.setPadding(dp(6), dp(8), 0, dp(8));
        c.setMinHeight(dp(44));
        return c;
    }

    private GradientDrawable fieldBackground() {
        GradientDrawable g = rounded(PINK_WASH, dp(14));
        g.setStroke(dp(1.5f), 0xFFF7B3C8);
        return g;
    }

    EditText field(String hint, int inputType) {
        EditText e = new EditText(ctx);
        e.setHint(hint);
        e.setInputType(inputType == 0 ? InputType.TYPE_CLASS_TEXT : inputType);
        e.setTypeface(serif);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        e.setTextColor(INK);
        e.setHintTextColor(0xFFB28F98);
        e.setBackground(fieldBackground());
        e.setPadding(dp(14), dp(12), dp(14), dp(12));
        return e;
    }

    /** An adapter whose rows use the launcher's fonts and colours. */
    ArrayAdapter<String> adapter(String[] items) {
        return new ArrayAdapter<String>(ctx, android.R.layout.simple_spinner_item, new java.util.ArrayList<>(java.util.Arrays.asList(items))) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTypeface(serif);
                t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
                t.setTextColor(INK);
                t.setPadding(dp(2), dp(4), dp(2), dp(4));
                return t;
            }

            @Override
            public View getDropDownView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getDropDownView(position, convertView, parent);
                t.setTypeface(serif);
                t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
                t.setTextColor(INK);
                t.setPadding(dp(18), dp(14), dp(18), dp(14));
                return t;
            }
        };
    }

    Spinner spinner(ArrayAdapter<String> adapter, int selected, IntConsumer onSelect) {
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        Spinner s = new Spinner(ctx, Spinner.MODE_DROPDOWN);
        s.setAdapter(adapter);
        s.setBackground(fieldBackground());
        s.setPadding(dp(12), dp(8), dp(12), dp(8));
        s.setPopupBackgroundDrawable(rounded(Color.WHITE, dp(14)));
        s.setDropDownVerticalOffset(dp(4));
        s.setMinimumHeight(dp(48));
        if (selected >= 0 && selected < adapter.getCount()) s.setSelection(selected);
        s.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (onSelect != null) onSelect.accept(position);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        return s;
    }

    /** Clips a view to an oval (the header picture). */
    static void clipOval(View v) {
        v.setOutlineProvider(new ViewOutlineProvider() {
            @Override
            public void getOutline(View view, Outline outline) {
                outline.setOval(0, 0, view.getWidth(), view.getHeight());
            }
        });
        v.setClipToOutline(true);
    }

    static Drawable transparent() {
        return new ColorDrawable(Color.TRANSPARENT);
    }
}
