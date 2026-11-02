package io.github.cng605568art.th06;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.UriPermission;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;
import java.util.Locale;

/**
 * Launcher screen. The player picks the folder that holds their own copy of the game
 * (the 2002 original, or the New Classic Steam install's data folder); we only remember
 * read access to it. No game file is bundled with or copied into the app.
 */
public class ImportActivity extends Activity {
    private static final int REQUEST_TREE = 6;
    private static final int REQUEST_AUDIO = 7;
    private static final int REQUEST_BACKUP = 8;
    private static final int REQUEST_RESTORE = 9;
    private static final int REQUEST_LOG = 10;

    private LauncherUi ui;
    private TextView status;
    private TextView playHint;
    private CheckBox quick;
    private Button chooseButton;
    private Button playButton;
    private CheckBox preferNewClassic;
    private volatile boolean scanning;
    // Replace music
    private Spinner musicTrack;
    private ArrayAdapter<String> musicTracks;
    private EditText loopFrom;
    private Button musicPick;
    private Button musicRestore;
    private TextView musicStatus;
    private volatile boolean converting;
    // Banners, backup, help
    private LinearLayout banners;
    private CheckBox backupMusic;
    private Button backupButton;
    private Button restoreButton;
    private TextView backupStatus;
    private TextView updateStatus;
    private Button updateButton;
    private volatile boolean busy;

    @Override
    protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(Lang.wrap(base));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ui = new LauncherUi(this);
        drawEdgeToEdge();

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        LinearLayout header = buildHeader();
        page.addView(header, LauncherUi.matchWrap());

        // Play, floating over the header's edge, with a line saying what it will do.
        playButton = ui.hero(getString(R.string.play));
        playButton.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        LinearLayout.LayoutParams playLp = LauncherUi.matchWrap();
        playLp.setMargins(ui.dp(28), -ui.dp(32), ui.dp(28), 0);
        page.addView(playButton, playLp);
        playHint = ui.body("", 13, LauncherUi.MUTED);
        playHint.setGravity(Gravity.CENTER_HORIZONTAL);
        playHint.setPadding(ui.dp(24), ui.dp(8), ui.dp(24), ui.dp(4));
        page.addView(playHint, LauncherUi.matchWrap());
        // A crash last time, or a newer release: shown here, above everything else.
        banners = new LinearLayout(this);
        banners.setOrientation(LinearLayout.VERTICAL);
        page.addView(banners, LauncherUi.matchWrap());

        addLanguageSection(page);

        // Game files
        LinearLayout files = ui.card(page, getString(R.string.files_title), getString(R.string.files_body));
        status = ui.statusBox();
        files.addView(status, ui.matchWrap(12, 12));
        chooseButton = ui.primary(getString(R.string.files_choose));
        chooseButton.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(pick, REQUEST_TREE);
        });
        files.addView(chooseButton, ui.matchWrap(2, 4));
        preferNewClassic = ui.check(getString(R.string.files_prefer_nc),
                DataSources.preferNewClassic(this));
        preferNewClassic.setOnCheckedChangeListener(
                (b, checked) -> DataSources.setPreferNewClassic(this, checked));
        files.addView(preferNewClassic, LauncherUi.matchWrap());

        // Stage select: skip the title and start at any stage chosen here, as practice. The
        // game plays on from there, and nothing from it (high scores, clear progress, spell
        // cards) is saved.
        LinearLayout select = ui.card(page, getString(R.string.select_title), getString(R.string.select_body));
        quick = ui.check(getString(R.string.select_check), GameOptions.quickStart(this));
        select.addView(quick, ui.matchWrap(6, 0));
        LinearLayout quickOptions = new LinearLayout(this);
        quickOptions.setOrientation(LinearLayout.VERTICAL);
        quickOptions.addView(ui.caption(getString(R.string.select_shot)), LauncherUi.matchWrap());
        quickOptions.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.shot_types)), GameOptions.shotType(this),
                v -> GameOptions.setShotType(this, v)), LauncherUi.matchWrap());
        quickOptions.addView(ui.caption(getString(R.string.select_difficulty)), LauncherUi.matchWrap());
        quickOptions.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.difficulties)), GameOptions.difficulty(this),
                v -> GameOptions.setDifficulty(this, v)), LauncherUi.matchWrap());
        quickOptions.addView(ui.caption(getString(R.string.select_start)), LauncherUi.matchWrap());
        quickOptions.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.stages)), GameOptions.stage(this) - 1, v -> {
            GameOptions.setStage(this, v + 1);
            updatePlayHint();
        }), LauncherUi.matchWrap());
        quickOptions.setVisibility(quick.isChecked() ? View.VISIBLE : View.GONE);
        quick.setOnCheckedChangeListener((b, checked) -> {
            GameOptions.setQuickStart(this, checked);
            quickOptions.setVisibility(checked ? View.VISIBLE : View.GONE);
            updatePlayHint();
        });
        select.addView(quickOptions, LauncherUi.matchWrap());

        addControlsSection(page);
        addDisplaySection(page);
        addPracticeSection(page);

        // Mischief
        LinearLayout mischiefCard = ui.card(page, getString(R.string.mischief_title), getString(R.string.mischief_body));
        int mischief = GameOptions.mischief(this);
        String[] mischiefNames = getResources().getStringArray(R.array.mischief);
        for (int i = 0; i < mischiefNames.length; ++i) {
            final int bit = i;
            CheckBox box = ui.check(mischiefNames[i], (mischief & (1 << i)) != 0);
            box.setOnCheckedChangeListener((b, checked) -> GameOptions.setMischief(this, bit, checked));
            mischiefCard.addView(box, LauncherUi.matchWrap());
        }

        addMusicSection(page);
        addBackupSection(page);
        addHelpSection(page);

        TextView footer = ui.body(getString(R.string.footer), 12, LauncherUi.MUTED);
        footer.setGravity(Gravity.CENTER_HORIZONTAL);
        footer.setPadding(ui.dp(28), ui.dp(10), ui.dp(28), ui.dp(8));
        page.addView(footer, LauncherUi.matchWrap());

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(LauncherUi.PAPER);
        scroll.setClipToPadding(false);
        scroll.addView(page);
        setContentView(scroll);
        // The header runs under the status bar; the page keeps clear of the system bars.
        final int headerTop = ui.dp(30), headerBottom = ui.dp(54), side = ui.dp(20);
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            int top, bottom, left, right;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                top = i.top;
                bottom = i.bottom;
                left = i.left;
                right = i.right;
            } else {
                top = insets.getSystemWindowInsetTop();
                bottom = insets.getSystemWindowInsetBottom();
                left = insets.getSystemWindowInsetLeft();
                right = insets.getSystemWindowInsetRight();
            }
            header.setPadding(side + left, headerTop + top, side + right, headerBottom);
            page.setPadding(left, 0, right, bottom + ui.dp(20));
            return insets;
        });

        refresh();
        if (UpdateChecker.due(this)) checkForUpdates(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        showBanners();
    }

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

    /** The red header: the picture in a white ring, the title in brush script. */
    private LinearLayout buildHeader() {
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setGravity(Gravity.CENTER_HORIZONTAL);
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[] {LauncherUi.RED_DEEP, LauncherUi.RED_DARK, LauncherUi.RED});
        float r = ui.dp(36);
        bg.setCornerRadii(new float[] {0, 0, 0, 0, r, r, r, r});
        header.setBackground(bg);

        FrameLayout ring = new FrameLayout(this);
        GradientDrawable ringBg = new GradientDrawable();
        ringBg.setShape(GradientDrawable.OVAL);
        ringBg.setColor(Color.WHITE);
        ringBg.setStroke(ui.dp(3), LauncherUi.PINK_SOFT);
        ring.setBackground(ringBg);
        ring.setPadding(ui.dp(5), ui.dp(5), ui.dp(5), ui.dp(5));
        ring.setElevation(ui.dp(10));
        LauncherUi.clipOval(ring);
        ImageView art = new ImageView(this);
        art.setImageResource(R.drawable.launcher_art);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        LauncherUi.clipOval(art);
        ring.addView(art, new FrameLayout.LayoutParams(ui.dp(108), ui.dp(108)));
        LinearLayout.LayoutParams ringLp = new LinearLayout.LayoutParams(ui.dp(118), ui.dp(118));
        ringLp.bottomMargin = ui.dp(14);
        header.addView(ring, ringLp);

        TextView title = ui.title("東方紅魔郷", 44, Color.WHITE);
        title.setShadowLayer(ui.dp(6), 0, ui.dp(2), 0x80000000);
        header.addView(title);
        TextView subtitle = ui.title("the Embodiment of Scarlet Devil", 18, LauncherUi.PINK_SOFT);
        subtitle.setPadding(0, ui.dp(8), 0, 0);
        header.addView(subtitle);
        TextView tag = ui.body(getString(R.string.header_tag), 13, 0xFFFFC2D2);
        tag.setPadding(0, ui.dp(8), 0, 0);
        header.addView(tag);
        return header;
    }

    /** Touch controls: drag-to-move or the D-pad, and how the buttons look and behave. */
    private void addControlsSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.controls_title), getString(R.string.controls_body));
        card.addView(ui.caption(getString(R.string.controls_movement)), LauncherUi.matchWrap());
        LinearLayout dragOnly = new LinearLayout(this);
        dragOnly.setOrientation(LinearLayout.VERTICAL);
        card.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.move_modes)), GameOptions.controlChoice(this, "mode", 0), v -> {
            GameOptions.setControlChoice(this, "mode", v);
            dragOnly.setVisibility(v == 0 ? View.VISIBLE : View.GONE);
        }), LauncherUi.matchWrap());
        dragOnly.addView(ui.caption(getString(R.string.controls_drag_speed)), LauncherUi.matchWrap());
        dragOnly.addView(ui.spinner(ui.adapter(GameOptions.DRAG_SPEEDS), GameOptions.controlChoice(this, "speed", 2),
                v -> GameOptions.setControlChoice(this, "speed", v)), LauncherUi.matchWrap());
        dragOnly.setVisibility(GameOptions.controlChoice(this, "mode", 0) == 0 ? View.VISIBLE : View.GONE);
        card.addView(dragOnly, LauncherUi.matchWrap());
        card.addView(ui.caption(getString(R.string.controls_button_size)), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.button_sizes)), GameOptions.controlChoice(this, "size", 1),
                v -> GameOptions.setControlChoice(this, "size", v)), LauncherUi.matchWrap());
        card.addView(ui.caption(getString(R.string.controls_opacity)), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.opacities)), GameOptions.controlChoice(this, "opacity", 3),
                v -> GameOptions.setControlChoice(this, "opacity", v)), LauncherUi.matchWrap());
        String[][] flags = {
            {"autoFire", getString(R.string.controls_autofire)},
            {"focusToggle", getString(R.string.controls_focus_toggle)},
            {"left", getString(R.string.controls_left)},
            {"vibrate", getString(R.string.controls_vibrate)},
        };
        for (String[] f : flags) {
            final String key = f[0];
            CheckBox box = ui.check(f[1], GameOptions.controlFlag(this, key, key.equals("vibrate")));
            box.setOnCheckedChangeListener((b, checked) -> GameOptions.setControlFlag(this, key, checked));
            card.addView(box, LauncherUi.matchWrap());
        }
    }

    /** How the game is shown: sideways or upright, smooth or sharp pixels, scanlines. */
    private void addDisplaySection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.display_title), getString(R.string.display_body));
        card.addView(ui.caption(getString(R.string.display_screen)), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.layouts)), GameOptions.controlChoice(this, "layout", 0),
                v -> GameOptions.setControlChoice(this, "layout", v)), LauncherUi.matchWrap());
        card.addView(ui.caption(getString(R.string.display_scaling)), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.scalings)), GameOptions.controlChoice(this, "scaling", 0),
                v -> GameOptions.setControlChoice(this, "scaling", v)), LauncherUi.matchWrap());
        CheckBox scan = ui.check(getString(R.string.display_scanlines), GameOptions.controlFlag(this, "scanlines", false));
        scan.setOnCheckedChangeListener((b, checked) -> GameOptions.setControlFlag(this, "scanlines", checked));
        card.addView(scan, LauncherUi.matchWrap());
    }

    /** Practice tools (not in the original): slow motion, a hitbox marker, save states. */
    private void addPracticeSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.practice_title), getString(R.string.practice_body));
        card.addView(ui.caption(getString(R.string.practice_speed)), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(getResources().getStringArray(R.array.speeds)), GameOptions.controlChoice(this, "speed%", 0),
                v -> GameOptions.setControlChoice(this, "speed%", v)), LauncherUi.matchWrap());
        String[][] flags = {
            {"hitbox", getString(R.string.practice_hitbox)},
            {"saveStates", getString(R.string.practice_states)},
        };
        for (String[] f : flags) {
            final String key = f[0];
            CheckBox box = ui.check(f[1], GameOptions.controlFlag(this, key, false));
            box.setOnCheckedChangeListener((b, checked) -> GameOptions.setControlFlag(this, key, checked));
            card.addView(box, LauncherUi.matchWrap());
        }
    }

    // ---- language ---------------------------------------------------------------------

    /** The language picker: the launcher's text now, the game's text when it starts. */
    private void addLanguageSection(LinearLayout page) {
        String heading = getString(R.string.lang_title);
        if (!heading.equals("Language")) heading += "  ·  Language";  // findable in any language
        LinearLayout card = ui.card(page, heading, getString(R.string.lang_body));
        final int current = Lang.chosenIndex(this);
        card.addView(ui.spinner(ui.adapter(Lang.labels(this)), current, v -> {
            if (v == Lang.chosenIndex(this)) return;
            Lang.choose(this, v);
            recreate();  // every text on the page in the new language
        }), ui.matchWrap(8, 0));
    }

    // ---- banners ----------------------------------------------------------------------

    private void showBanners() {
        if (banners == null) return;
        banners.removeAllViews();
        if (CrashReports.hasUnseenCrash(this)) {
            LinearLayout card = ui.card(banners, getString(R.string.crash_title), getString(R.string.crash_body));
            Button save = ui.primary(getString(R.string.crash_save));
            save.setOnClickListener(v -> saveLog());
            card.addView(save, ui.matchWrap(12, 4));
            Button dismiss = ui.secondary(getString(R.string.crash_dismiss));
            dismiss.setOnClickListener(v -> {
                CrashReports.markSeen(this);
                showBanners();
            });
            card.addView(dismiss, ui.matchWrap(0, 0));
        }
        UpdateChecker.Result update = UpdateChecker.cached(this);
        if (update != null && update.newer) {
            LinearLayout card = ui.card(banners, getString(R.string.update_title, update.latest),
                    getString(R.string.update_body, CrashReports.versionName(this)));
            Button open = ui.primary(getString(R.string.update_open));
            open.setOnClickListener(v -> openPage(update.page));
            card.addView(open, ui.matchWrap(12, 0));
        }
    }

    private void openPage(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (android.content.ActivityNotFoundException e) {
            if (updateStatus != null) updateStatus.setText(getString(R.string.no_browser, url));
        }
    }

    // ---- backup -----------------------------------------------------------------------

    /** Backup and restore of scores, options, replays, launcher settings and replaced music. */
    private void addBackupSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.backup_title), getString(R.string.backup_body));
        backupMusic = ui.check(getString(R.string.backup_music), true);
        card.addView(backupMusic, ui.matchWrap(4, 0));
        backupButton = ui.primary(getString(R.string.backup_save));
        backupButton.setOnClickListener(v -> {
            Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            create.addCategory(Intent.CATEGORY_OPENABLE);
            create.setType("application/zip");
            create.putExtra(Intent.EXTRA_TITLE, Backup.suggestedName());
            startActivityForResult(create, REQUEST_BACKUP);
        });
        card.addView(backupButton, ui.matchWrap(10, 4));
        restoreButton = ui.secondary(getString(R.string.backup_restore));
        restoreButton.setOnClickListener(v -> {
            Intent open = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            open.addCategory(Intent.CATEGORY_OPENABLE);
            open.setType("*/*");
            open.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            startActivityForResult(open, REQUEST_RESTORE);
        });
        card.addView(restoreButton, ui.matchWrap(0, 0));
        backupStatus = ui.body("", 14, LauncherUi.INK);
        backupStatus.setPadding(ui.dp(4), ui.dp(10), ui.dp(4), 0);
        card.addView(backupStatus, LauncherUi.matchWrap());
    }

    private void setBusy(boolean on) {
        busy = on;
        backupButton.setEnabled(!on);
        restoreButton.setEnabled(!on);
        playButton.setEnabled(!on && !scanning);
    }

    private void backUp(Uri target) {
        final boolean music = backupMusic.isChecked();
        setBusy(true);
        backupStatus.setText(getString(R.string.backup_saving));
        new Thread(() -> {
            String message;
            try (OutputStream out = getContentResolver().openOutputStream(target, "wt")) {
                if (out == null) throw new IOException(getString(R.string.err_cannot_write));
                message = getString(R.string.backup_saved, Backup.create(this, out, music,
                        m -> runOnUiThread(() -> backupStatus.setText(m))));
            } catch (IOException | RuntimeException e) {
                message = getString(R.string.backup_save_failed, e.getMessage());
            }
            final String result = message;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                setBusy(false);
                backupStatus.setText(result);
            });
        }, "th06-backup").start();
    }

    private void confirmRestore(Uri source) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.backup_confirm_title)
                .setMessage(R.string.backup_confirm_body)
                .setPositiveButton(R.string.backup_confirm_ok, (d, w) -> restore(source))
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    private void restore(Uri source) {
        setBusy(true);
        backupStatus.setText(getString(R.string.backup_restoring));
        new Thread(() -> {
            String message;
            boolean ok = false;
            try (InputStream in = getContentResolver().openInputStream(source)) {
                if (in == null) throw new IOException(getString(R.string.err_cannot_read));
                message = getString(R.string.backup_restored, Backup.restore(this, in, m -> runOnUiThread(() -> backupStatus.setText(m))));
                ok = true;
            } catch (IOException | RuntimeException e) {
                message = getString(R.string.backup_restore_failed, e.getMessage());
            }
            final String result = message;
            final boolean restored = ok;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                setBusy(false);
                if (restored) {
                    // The launcher's settings changed under its controls: rebuild the page.
                    getIntent().putExtra("restored", result);
                    recreate();
                } else {
                    backupStatus.setText(result);
                }
            });
        }, "th06-restore").start();
    }

    // ---- help & updates -------------------------------------------------------------------

    private void addHelpSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.help_title),
                getString(R.string.help_version, CrashReports.versionName(this), CrashReports.versionCode(this)));
        CheckBox auto = ui.check(getString(R.string.help_auto), UpdateChecker.autoCheck(this));
        auto.setOnCheckedChangeListener((b, checked) -> UpdateChecker.setAutoCheck(this, checked));
        card.addView(auto, ui.matchWrap(4, 0));
        updateButton = ui.secondary(getString(R.string.help_check));
        updateButton.setOnClickListener(v -> checkForUpdates(true));
        card.addView(updateButton, ui.matchWrap(6, 4));
        updateStatus = ui.body("", 14, LauncherUi.INK);
        updateStatus.setPadding(ui.dp(4), ui.dp(4), ui.dp(4), ui.dp(6));
        card.addView(updateStatus, LauncherUi.matchWrap());
        card.addView(ui.caption(getString(R.string.help_bug)), LauncherUi.matchWrap());
        Button log = ui.secondary(getString(R.string.help_save_log));
        log.setOnClickListener(v -> saveLog());
        card.addView(log, ui.matchWrap(4, 4));
        Button issues = ui.secondary(getString(R.string.help_report));
        issues.setOnClickListener(v -> openPage(UpdateChecker.ISSUES_PAGE));
        card.addView(issues, ui.matchWrap(0, 0));
        String restored = getIntent().getStringExtra("restored");
        if (restored != null) {
            backupStatus.setText(restored);
            getIntent().removeExtra("restored");
        }
    }

    private void checkForUpdates(boolean manual) {
        updateButton.setEnabled(false);
        if (manual) updateStatus.setText(getString(R.string.help_checking));
        new Thread(() -> {
            String message;
            try {
                UpdateChecker.Result r = UpdateChecker.check(this);
                if (r.latest == null) message = getString(R.string.help_none_published);
                else if (r.newer) message = getString(R.string.help_newer, r.latest);
                else message = getString(R.string.help_latest, r.latest);
            } catch (IOException | RuntimeException e) {
                message = getString(R.string.help_check_failed, e.getMessage());
            }
            final String result = message;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                updateButton.setEnabled(true);
                if (manual || UpdateChecker.cached(this) != null) updateStatus.setText(result);
                showBanners();
            });
        }, "th06-update").start();
    }

    private void saveLog() {
        Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        create.addCategory(Intent.CATEGORY_OPENABLE);
        create.setType("text/plain");
        create.putExtra(Intent.EXTRA_TITLE, "th06-log-" + new java.text.SimpleDateFormat("yyyy-MM-dd-HHmm", Locale.ROOT)
                .format(new java.util.Date()) + ".txt");
        startActivityForResult(create, REQUEST_LOG);
    }

    private void writeLog(Uri target) {
        try (OutputStream out = getContentResolver().openOutputStream(target, "wt")) {
            if (out == null) throw new IOException(getString(R.string.err_cannot_write));
            CrashReports.writeReport(this, out);
            CrashReports.markSeen(this);
            showBanners();
            if (updateStatus != null) updateStatus.setText(getString(R.string.help_log_saved));
        } catch (IOException | RuntimeException e) {
            if (updateStatus != null) updateStatus.setText(getString(R.string.help_log_failed, e.getMessage()));
        }
    }

    private void updatePlayHint() {
        if (playHint == null) return;
        if (DataSources.load(this).isEmpty()) {
            playHint.setText(R.string.hint_choose_folder);
        } else if (quick != null && quick.isChecked()) {
            int st = GameOptions.stage(this);
            playHint.setText(st == 7 ? getString(R.string.hint_extra) : getString(R.string.hint_stage, st));
        } else {
            playHint.setText(R.string.hint_title);
        }
    }

    /**
     * Replace music: pick one of the game's songs and an audio file of your own (MP3, AAC,
     * OGG, FLAC, WAV ...); it is converted to the game's format and played (and looped) in
     * that song's place. The game folder is not touched; "Restore" puts the original back.
     */
    private void addMusicSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.music_title), getString(R.string.music_body));
        card.addView(ui.caption(getString(R.string.music_song)), LauncherUi.matchWrap());
        musicTracks = ui.adapter(trackLabels());
        musicTrack = ui.spinner(musicTracks, 0, position -> showMusicStatus(null));
        card.addView(musicTrack, LauncherUi.matchWrap());
        card.addView(ui.caption(getString(R.string.music_loop)), LauncherUi.matchWrap());
        loopFrom = ui.field("0", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        card.addView(loopFrom, LauncherUi.matchWrap());
        musicPick = ui.primary(getString(R.string.music_choose));
        musicPick.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("audio/*");
            pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"audio/*", "application/ogg"});
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(pick, REQUEST_AUDIO);
        });
        card.addView(musicPick, ui.matchWrap(16, 8));
        musicRestore = ui.secondary(getString(R.string.music_restore));
        musicRestore.setOnClickListener(v -> {
            MusicReplacer.restore(this, musicTrack.getSelectedItemPosition());
            refreshTracks();
            showMusicStatus(getString(R.string.music_restored));
        });
        card.addView(musicRestore, ui.matchWrap(0, 4));
        musicStatus = ui.body("", 14, LauncherUi.INK);
        musicStatus.setPadding(ui.dp(4), ui.dp(10), ui.dp(4), 0);
        card.addView(musicStatus, LauncherUi.matchWrap());
        showMusicStatus(null);
    }

    private String[] trackLabels() {
        String[] labels = new String[MusicReplacer.TRACK_COUNT];
        for (int t = 0; t < labels.length; ++t) {
            labels[t] = MusicReplacer.trackLabel(this, t)
                    + (MusicReplacer.isReplaced(this, t) ? "   " + getString(R.string.music_replaced_mark) : "");
        }
        return labels;
    }

    private void refreshTracks() {
        int selected = musicTrack.getSelectedItemPosition();
        musicTracks.clear();
        musicTracks.addAll(trackLabels());
        musicTracks.notifyDataSetChanged();
        musicTrack.setSelection(selected);
    }

    private void showMusicStatus(String message) {
        int track = musicTrack.getSelectedItemPosition();
        String now = MusicReplacer.describe(this, track);
        String state = now != null ? getString(R.string.music_now_yours, now) : getString(R.string.music_now_game);
        musicStatus.setText(message != null ? message + "\n" + state : state);
        musicRestore.setEnabled(!converting && now != null);
        musicPick.setEnabled(!converting);
        musicTrack.setEnabled(!converting);
    }

    private void convert(Uri audio) {
        final int track = musicTrack.getSelectedItemPosition();
        double loop = 0;
        try {
            String text = loopFrom.getText().toString().trim();
            if (!text.isEmpty()) loop = Double.parseDouble(text);
        } catch (NumberFormatException e) {
            showMusicStatus(getString(R.string.music_loop_nan));
            return;
        }
        final double loopSeconds = loop;
        converting = true;
        showMusicStatus(getString(R.string.music_reading));
        new Thread(() -> {
            String result;
            try {
                String summary = MusicReplacer.replace(this, audio, track, loopSeconds,
                        message -> runOnUiThread(() -> musicStatus.setText(message)));
                result = getString(R.string.music_done, MusicReplacer.trackName(this, track), summary);
            } catch (IOException | RuntimeException e) {
                result = getString(R.string.music_failed, e.getMessage());
            }
            final String message = result;
            runOnUiThread(() -> {
                converting = false;
                if (isFinishing() || isDestroyed()) return;
                refreshTracks();
                showMusicStatus(message);
                refresh();
            });
        }, "th06-music").start();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        final Uri picked = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (requestCode == REQUEST_AUDIO) {
            if (picked != null) convert(picked);
            return;
        }
        if (requestCode == REQUEST_BACKUP) {
            if (picked != null && !busy) backUp(picked);
            return;
        }
        if (requestCode == REQUEST_RESTORE) {
            if (picked != null && !busy) confirmRestore(picked);
            return;
        }
        if (requestCode == REQUEST_LOG) {
            if (picked != null) writeLog(picked);
            return;
        }
        if (requestCode != REQUEST_TREE || resultCode != RESULT_OK || data == null
                || data.getData() == null) {
            return;
        }
        Uri tree = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            status.setText(getString(R.string.files_denied, e.getMessage()));
            return;
        }
        scan(tree);
    }

    private void scan(Uri tree) {
        scanning = true;
        chooseButton.setEnabled(false);
        playButton.setEnabled(false);
        status.setText(R.string.files_scanning);
        new Thread(() -> {
            List<DataSources.Entry> found = DataSources.scanTree(this, tree,
                    path -> runOnUiThread(() -> status.setText(getString(R.string.files_scanning_path, path))));
            if (!found.isEmpty()) {
                DataSources.save(this, tree, found);
                releaseOtherGrants(tree);
            }
            runOnUiThread(() -> {
                scanning = false;
                chooseButton.setEnabled(true);
                if (found.isEmpty()) {
                    status.setText(getString(R.string.files_none_found) + "\n\n");
                    playButton.setEnabled(true);
                    appendCurrent();
                } else {
                    refresh();
                }
            });
        }, "th06-scan").start();
    }

    /** Keep only the grant for the folder in use (Android caps persisted grants). */
    private void releaseOtherGrants(Uri keep) {
        for (UriPermission p : getContentResolver().getPersistedUriPermissions()) {
            if (!p.getUri().equals(keep)) {
                try {
                    getContentResolver().releasePersistableUriPermission(p.getUri(),
                            Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (SecurityException ignored) {
                    // Already gone.
                }
            }
        }
    }

    private void refresh() {
        if (scanning) return;
        status.setText("");
        appendCurrent();
        playButton.setEnabled(!busy);
    }

    private void appendCurrent() {
        List<DataSources.Entry> entries = DataSources.load(this);
        StringBuilder sb = new StringBuilder(status.getText());
        if (entries.isEmpty()) {
            sb.append(getString(R.string.files_none_selected));
        } else {
            boolean classic = false;
            int tracks = 0;
            long musicBytes = 0;
            boolean hasScores = false;
            sb.append(getResources().getQuantityString(R.plurals.files_in_use, entries.size(), entries.size())).append('\n');
            for (DataSources.Entry e : entries) {
                if (DataSources.FORMAT_WAV.equals(e.format)) {
                    ++tracks;
                    musicBytes += e.size;
                    continue;
                }
                if (DataSources.FORMAT_SCORE.equals(e.format)) {
                    hasScores = true;
                    continue;
                }
                boolean isClassic = DataSources.FORMAT_PBG3.equals(e.format);
                classic |= isClassic;
                String kind = getString(isClassic ? R.string.files_kind_classic
                        : DataSources.FORMAT_EXE.equals(e.format) ? R.string.files_kind_program : R.string.files_kind_nc);
                sb.append(String.format(Locale.ROOT, "  %-18s %-11s %6.1f MB%n",
                        e.name, kind, e.size / 1048576.0));
            }
            sb.append("  ").append(tracks > 0 ? getString(R.string.files_music, tracks, musicBytes / 1048576.0)
                    : getString(R.string.files_music_none)).append('\n');
            int replaced = MusicReplacer.replacedCount(this);
            if (replaced > 0) {
                sb.append("  ").append(getString(R.string.files_replaced, replaced)).append('\n');
            }
            if (hasScores) {
                sb.append("  ").append(getString(R.string.files_scores)).append('\n');
            }
            if (!classic) {
                sb.append('\n').append(getString(R.string.files_nc_only)).append('\n');
            }
        }
        status.setText(sb.toString().trim());
        updatePlayHint();
    }

}
