package io.github.cng605568art.th07;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.function.IntConsumer;

/**
 * The launcher's look: dusk plum and cherry-blossom pink on pale lilac paper, white cards,
 * the system's serif face for titles and its sans-serif for text (no bundled fonts).
 */
final class Ui {
    static final int PLUM_DEEP = 0xFF2A1838;
    static final int PLUM = 0xFF4A2A5E;
    static final int PLUM_LIGHT = 0xFF6E4A86;
    static final int SAKURA = 0xFFE8679A;
    static final int SAKURA_SOFT = 0xFFFCE4EC;
    static final int WASH = 0xFFF8F1FA;
    static final int PAPER = 0xFFFBF8FC;
    static final int INK = 0xFF24182B;
    static final int MUTED = 0xFF6F6078;
    static final int LINE = 0xFFEADCF0;

    final Context ctx;
    final Typeface heading = Typeface.create(Typeface.SERIF, Typeface.BOLD);
    final Typeface text = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL);

    Ui(Context ctx) {
        this.ctx = ctx;
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

    TextView title(String s, float sp, int color) {
        TextView t = new TextView(ctx);
        t.setText(s);
        t.setTypeface(heading);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setIncludeFontPadding(false);
        return t;
    }

    TextView body(String s, float sp, int color) {
        TextView t = new TextView(ctx);
        t.setText(s);
        t.setTypeface(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setLineSpacing(dp(2), 1.0f);
        return t;
    }

    /** A small caption above a control. */
    TextView caption(String s) {
        TextView t = body(s, 13, MUTED);
        t.setPadding(dp(2), dp(12), 0, dp(4));
        return t;
    }

    /** The monospaced file list, on a lilac wash. */
    TextView statusBox() {
        TextView t = new TextView(ctx);
        t.setTypeface(Typeface.MONOSPACE);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        t.setTextColor(INK);
        t.setLineSpacing(dp(1), 1.0f);
        t.setPadding(dp(12), dp(10), dp(12), dp(10));
        GradientDrawable bg = rounded(WASH, dp(12));
        bg.setStroke(dp(1), LINE);
        t.setBackground(bg);
        t.setTextIsSelectable(true);
        return t;
    }

    // ---- containers --------------------------------------------------------------------

    /** A white card with a section title and an optional subtitle. */
    LinearLayout card(LinearLayout page, String title, String subtitle) {
        LinearLayout card = new LinearLayout(ctx);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(16), dp(18), dp(18));
        GradientDrawable bg = rounded(Color.WHITE, dp(20));
        bg.setStroke(dp(1), LINE);
        card.setBackground(bg);
        card.setElevation(dp(2));
        LinearLayout.LayoutParams lp = matchWrap();
        lp.setMargins(dp(16), dp(8), dp(16), dp(10));
        page.addView(card, lp);

        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        View bar = new View(ctx);  // a short plum-to-pink stroke before the title
        GradientDrawable stroke = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] {PLUM, SAKURA});
        stroke.setCornerRadius(dp(3));
        bar.setBackground(stroke);
        LinearLayout.LayoutParams barLp = new LinearLayout.LayoutParams(dp(5), dp(22));
        barLp.rightMargin = dp(10);
        row.addView(bar, barLp);
        row.addView(title(title, 20, PLUM), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
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
        if (stroke != 0) disabled.setStroke(dp(1.5f), LINE);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {-android.R.attr.state_enabled}, disabled);
        states.addState(new int[] {}, normal);
        return new RippleDrawable(ColorStateList.valueOf(stroke != 0 ? 0x33E8679A : 0x55FFFFFF), states, rounded(Color.WHITE, radius));
    }

    private static ColorStateList textColors(int enabled, int disabled) {
        return new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}}, new int[] {disabled, enabled});
    }

    private Button button(String s, float sp) {
        Button b = new Button(ctx);
        b.setText(s);
        b.setAllCaps(false);
        b.setTypeface(heading);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        b.setStateListAnimator(null);
        b.setMinHeight(dp(50));
        b.setPadding(dp(16), dp(10), dp(16), dp(10));
        return b;
    }

    /** Filled plum, white text. */
    Button primary(String s) {
        Button b = button(s, 17);
        b.setBackground(buttonBackground(PLUM, 0, 0xFFD9CCE0, dp(25)));
        b.setTextColor(textColors(Color.WHITE, 0xFFF6F0F8));
        b.setElevation(dp(2));
        return b;
    }

    /** White with a pink outline, plum text. */
    Button secondary(String s) {
        Button b = button(s, 16);
        b.setBackground(buttonBackground(Color.WHITE, SAKURA, 0xFFFAF6FB, dp(25)));
        b.setTextColor(textColors(PLUM, 0xFFC2B5C9));
        return b;
    }

    /** The big Play button: a plum-to-pink sweep. */
    Button hero(String s) {
        Button b = button(s, 24);
        b.setMinHeight(dp(64));
        GradientDrawable sweep = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {PLUM_DEEP, PLUM_LIGHT, SAKURA});
        sweep.setCornerRadius(dp(32));
        GradientDrawable disabled = rounded(0xFFD9CCE0, dp(32));
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {-android.R.attr.state_enabled}, disabled);
        states.addState(new int[] {}, sweep);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x66FFFFFF), states, rounded(Color.WHITE, dp(32))));
        b.setTextColor(textColors(Color.WHITE, 0xFFF6F0F8));
        b.setElevation(dp(6));
        return b;
    }

    CheckBox check(String s, boolean checked) {
        CheckBox c = new CheckBox(ctx);
        c.setText(s);
        c.setChecked(checked);
        c.setTypeface(text);
        c.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        c.setTextColor(INK);
        c.setButtonTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {SAKURA, 0xFFB9A7C2}));
        c.setPadding(dp(6), dp(8), 0, dp(8));
        c.setMinHeight(dp(44));
        return c;
    }

    private GradientDrawable fieldBackground() {
        GradientDrawable g = rounded(WASH, dp(14));
        g.setStroke(dp(1.5f), 0xFFDCC6E6);
        return g;
    }

    /** An adapter whose rows use the launcher's font and colours. */
    ArrayAdapter<String> adapter(String[] items) {
        return new ArrayAdapter<String>(ctx, android.R.layout.simple_spinner_item, new ArrayList<>(Arrays.asList(items))) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTypeface(text);
                t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
                t.setTextColor(INK);
                t.setPadding(dp(2), dp(4), dp(2), dp(4));
                return t;
            }

            @Override
            public View getDropDownView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getDropDownView(position, convertView, parent);
                t.setTypeface(text);
                t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
                t.setTextColor(INK);
                t.setPadding(dp(18), dp(14), dp(18), dp(14));
                return t;
            }
        };
    }

    Spinner spinner(String[] items, int selected, IntConsumer onSelect) {
        ArrayAdapter<String> adapter = adapter(items);
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
}
