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
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ui = new LauncherUi(this);
        drawEdgeToEdge();

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        LinearLayout header = buildHeader();
        page.addView(header, LauncherUi.matchWrap());

        // Play, floating over the header's edge, with a line saying what it will do.
        playButton = ui.hero("Play");
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

        // Game files
        LinearLayout files = ui.card(page, "Game files",
                "Copy your own game folder to the phone (the 2002 original with its 紅魔郷**.DAT / "
                        + "KOUMAKYO_**.dat files, or the New Classic data folder with th06**.dat), then "
                        + "choose it here. The files are read in place; nothing is copied.");
        status = ui.statusBox();
        files.addView(status, ui.matchWrap(12, 12));
        chooseButton = ui.primary("Choose game folder");
        chooseButton.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(pick, REQUEST_TREE);
        });
        files.addView(chooseButton, ui.matchWrap(2, 4));
        preferNewClassic = ui.check("If both versions are found, prefer New Classic assets",
                DataSources.preferNewClassic(this));
        preferNewClassic.setOnCheckedChangeListener(
                (b, checked) -> DataSources.setPreferNewClassic(this, checked));
        files.addView(preferNewClassic, LauncherUi.matchWrap());

        // Stage select: skip the title and start at any stage chosen here, as practice. The
        // game plays on from there, and nothing from it (high scores, clear progress, spell
        // cards) is saved.
        LinearLayout select = ui.card(page, "Stage select",
                "Skip the title screen and start at any stage. Practice only: nothing from these "
                        + "games is saved.");
        quick = ui.check("Start at any stage (scores not saved)", GameOptions.quickStart(this));
        select.addView(quick, ui.matchWrap(6, 0));
        LinearLayout quickOptions = new LinearLayout(this);
        quickOptions.setOrientation(LinearLayout.VERTICAL);
        quickOptions.addView(ui.caption("Shot type"), LauncherUi.matchWrap());
        quickOptions.addView(ui.spinner(ui.adapter(GameOptions.SHOT_TYPES), GameOptions.shotType(this),
                v -> GameOptions.setShotType(this, v)), LauncherUi.matchWrap());
        quickOptions.addView(ui.caption("Difficulty"), LauncherUi.matchWrap());
        quickOptions.addView(ui.spinner(ui.adapter(GameOptions.DIFFICULTIES), GameOptions.difficulty(this),
                v -> GameOptions.setDifficulty(this, v)), LauncherUi.matchWrap());
        quickOptions.addView(ui.caption("Start at (Extra uses its own difficulty)"), LauncherUi.matchWrap());
        quickOptions.addView(ui.spinner(ui.adapter(GameOptions.STAGES), GameOptions.stage(this) - 1, v -> {
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
        LinearLayout mischiefCard = ui.card(page, "Mischief",
                "Not in the original game. While any of these is on, scores, clears and spell cards "
                        + "are not saved.");
        int mischief = GameOptions.mischief(this);
        for (int i = 0; i < GameOptions.MISCHIEF.length; ++i) {
            final int bit = i;
            CheckBox box = ui.check(GameOptions.MISCHIEF[i], (mischief & (1 << i)) != 0);
            box.setOnCheckedChangeListener((b, checked) -> GameOptions.setMischief(this, bit, checked));
            mischiefCard.addView(box, LauncherUi.matchWrap());
        }

        addMusicSection(page);
        addBackupSection(page);
        addHelpSection(page);

        TextView footer = ui.body("A fan-made port for your own copy of 東方紅魔郷 ～ the Embodiment of "
                + "Scarlet Devil (Team Shanghai Alice). No game files are included.\n"
                + "Fonts: Yuji Syuku and Zen Antique (SIL Open Font License 1.1).", 12, LauncherUi.MUTED);
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
        TextView tag = ui.body("Android port  ·  plays your own 2002 game files", 13, 0xFFFFC2D2);
        tag.setPadding(0, ui.dp(8), 0, 0);
        header.addView(tag);
        return header;
    }

    /** Touch controls: drag-to-move or the D-pad, and how the buttons look and behave. */
    private void addControlsSection(LinearLayout page) {
        LinearLayout card = ui.card(page, "Controls",
                "Touch controls for the game (a Bluetooth or USB gamepad works too).");
        card.addView(ui.caption("Movement"), LauncherUi.matchWrap());
        LinearLayout dragOnly = new LinearLayout(this);
        dragOnly.setOrientation(LinearLayout.VERTICAL);
        card.addView(ui.spinner(ui.adapter(GameOptions.MOVE_MODES), GameOptions.controlChoice(this, "mode", 0), v -> {
            GameOptions.setControlChoice(this, "mode", v);
            dragOnly.setVisibility(v == 0 ? View.VISIBLE : View.GONE);
        }), LauncherUi.matchWrap());
        dragOnly.addView(ui.caption("Drag speed (how far the ship moves for your finger's travel)"), LauncherUi.matchWrap());
        dragOnly.addView(ui.spinner(ui.adapter(GameOptions.DRAG_SPEEDS), GameOptions.controlChoice(this, "speed", 2),
                v -> GameOptions.setControlChoice(this, "speed", v)), LauncherUi.matchWrap());
        dragOnly.setVisibility(GameOptions.controlChoice(this, "mode", 0) == 0 ? View.VISIBLE : View.GONE);
        card.addView(dragOnly, LauncherUi.matchWrap());
        card.addView(ui.caption("Button size"), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(GameOptions.BUTTON_SIZES), GameOptions.controlChoice(this, "size", 1),
                v -> GameOptions.setControlChoice(this, "size", v)), LauncherUi.matchWrap());
        card.addView(ui.caption("Button opacity"), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(GameOptions.OPACITIES), GameOptions.controlChoice(this, "opacity", 3),
                v -> GameOptions.setControlChoice(this, "opacity", v)), LauncherUi.matchWrap());
        String[][] flags = {
            {"autoFire", "Auto-fire (Shot is always held while you play)"},
            {"focusToggle", "Focus button toggles (tap on, tap off)"},
            {"left", "Left-handed (buttons on the left)"},
            {"vibrate", "Vibrate on a miss and on a bomb"},
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
        LinearLayout card = ui.card(page, "Display",
                "Upright play puts the playfield across the whole width with the score below it and "
                        + "the controls underneath; menus and story screens stay whole.");
        card.addView(ui.caption("Screen"), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(GameOptions.LAYOUTS), GameOptions.controlChoice(this, "layout", 0),
                v -> GameOptions.setControlChoice(this, "layout", v)), LauncherUi.matchWrap());
        card.addView(ui.caption("Scaling"), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(GameOptions.SCALINGS), GameOptions.controlChoice(this, "scaling", 0),
                v -> GameOptions.setControlChoice(this, "scaling", v)), LauncherUi.matchWrap());
        CheckBox scan = ui.check("CRT scanlines", GameOptions.controlFlag(this, "scanlines", false));
        scan.setOnCheckedChangeListener((b, checked) -> GameOptions.setControlFlag(this, "scanlines", checked));
        card.addView(scan, LauncherUi.matchWrap());
    }

    /** Practice tools (not in the original): slow motion, a hitbox marker, save states. */
    private void addPracticeSection(LinearLayout page) {
        LinearLayout card = ui.card(page, "Practice tools",
                "Not in the original game. Slow motion, or loading a save state, means that game's "
                        + "scores are not saved; the hitbox marker changes nothing.");
        card.addView(ui.caption("Game speed"), LauncherUi.matchWrap());
        card.addView(ui.spinner(ui.adapter(GameOptions.SPEEDS), GameOptions.controlChoice(this, "speed%", 0),
                v -> GameOptions.setControlChoice(this, "speed%", v)), LauncherUi.matchWrap());
        String[][] flags = {
            {"hitbox", "Show the ship's hitbox"},
            {"saveStates", "Save states (SAVE / LOAD buttons; F5 / F9 on a keyboard, L3 / R3 on a gamepad)"},
        };
        for (String[] f : flags) {
            final String key = f[0];
            CheckBox box = ui.check(f[1], GameOptions.controlFlag(this, key, false));
            box.setOnCheckedChangeListener((b, checked) -> GameOptions.setControlFlag(this, key, checked));
            card.addView(box, LauncherUi.matchWrap());
        }
    }

    // ---- banners ----------------------------------------------------------------------

    private void showBanners() {
        if (banners == null) return;
        banners.removeAllViews();
        if (CrashReports.hasUnseenCrash(this)) {
            LinearLayout card = ui.card(banners, "The game closed unexpectedly",
                    "Sorry about that. A log of what happened was kept on this phone; you can save it and "
                            + "attach it to a bug report on GitHub. Nothing is sent automatically.");
            Button save = ui.primary("Save the log");
            save.setOnClickListener(v -> saveLog());
            card.addView(save, ui.matchWrap(12, 4));
            Button dismiss = ui.secondary("Dismiss");
            dismiss.setOnClickListener(v -> {
                CrashReports.markSeen(this);
                showBanners();
            });
            card.addView(dismiss, ui.matchWrap(0, 0));
        }
        UpdateChecker.Result update = UpdateChecker.cached(this);
        if (update != null && update.newer) {
            LinearLayout card = ui.card(banners, "Version " + update.latest + " is available",
                    "You have " + CrashReports.versionName(this) + ". Download the new APK from its release page "
                            + "and install it over this one (your scores and settings stay).");
            Button open = ui.primary("Open the release page");
            open.setOnClickListener(v -> openPage(update.page));
            card.addView(open, ui.matchWrap(12, 0));
        }
    }

    private void openPage(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (android.content.ActivityNotFoundException e) {
            if (updateStatus != null) updateStatus.setText("No browser found to open " + url);
        }
    }

    // ---- backup -----------------------------------------------------------------------

    /** Backup and restore of scores, options, replays, launcher settings and replaced music. */
    private void addBackupSection(LinearLayout page) {
        LinearLayout card = ui.card(page, "Backup",
                "Keep your high scores, game options, replays and launcher settings in one file (in "
                        + "Downloads, a cloud drive …), and bring them back after reinstalling or on a new phone.");
        backupMusic = ui.check("Include replaced music (bigger file)", true);
        card.addView(backupMusic, ui.matchWrap(4, 0));
        backupButton = ui.primary("Back up to a file");
        backupButton.setOnClickListener(v -> {
            Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            create.addCategory(Intent.CATEGORY_OPENABLE);
            create.setType("application/zip");
            create.putExtra(Intent.EXTRA_TITLE, Backup.suggestedName());
            startActivityForResult(create, REQUEST_BACKUP);
        });
        card.addView(backupButton, ui.matchWrap(10, 4));
        restoreButton = ui.secondary("Restore from a backup");
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
        backupStatus.setText("Saving …");
        new Thread(() -> {
            String message;
            try (OutputStream out = getContentResolver().openOutputStream(target, "wt")) {
                if (out == null) throw new IOException("cannot write there");
                message = "Saved: " + Backup.create(this, out, music,
                        m -> runOnUiThread(() -> backupStatus.setText(m))) + ".";
            } catch (IOException | RuntimeException e) {
                message = "Could not save the backup: " + e.getMessage();
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
                .setTitle("Restore this backup?")
                .setMessage("Your current high scores, game options and launcher settings are replaced by the "
                        + "backup's (and your replaced music too, if the backup has music).")
                .setPositiveButton("Restore", (d, w) -> restore(source))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void restore(Uri source) {
        setBusy(true);
        backupStatus.setText("Restoring …");
        new Thread(() -> {
            String message;
            boolean ok = false;
            try (InputStream in = getContentResolver().openInputStream(source)) {
                if (in == null) throw new IOException("cannot read that file");
                message = "Restored: " + Backup.restore(this, in, m -> runOnUiThread(() -> backupStatus.setText(m))) + ".";
                ok = true;
            } catch (IOException | RuntimeException e) {
                message = "Could not restore: " + e.getMessage();
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
        LinearLayout card = ui.card(page, "Help & updates",
                "Version " + CrashReports.versionName(this) + " (build " + CrashReports.versionCode(this) + ").");
        CheckBox auto = ui.check("Check for a new version once a day", UpdateChecker.autoCheck(this));
        auto.setOnCheckedChangeListener((b, checked) -> UpdateChecker.setAutoCheck(this, checked));
        card.addView(auto, ui.matchWrap(4, 0));
        updateButton = ui.secondary("Check for updates now");
        updateButton.setOnClickListener(v -> checkForUpdates(true));
        card.addView(updateButton, ui.matchWrap(6, 4));
        updateStatus = ui.body("", 14, LauncherUi.INK);
        updateStatus.setPadding(ui.dp(4), ui.dp(4), ui.dp(4), ui.dp(6));
        card.addView(updateStatus, LauncherUi.matchWrap());
        card.addView(ui.caption("Found a bug? Save the log (it stays on your phone until you share it) and attach "
                + "it to an issue on GitHub."), LauncherUi.matchWrap());
        Button log = ui.secondary("Save a log for a bug report");
        log.setOnClickListener(v -> saveLog());
        card.addView(log, ui.matchWrap(4, 4));
        Button issues = ui.secondary("Report a problem on GitHub");
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
        if (manual) updateStatus.setText("Checking …");
        new Thread(() -> {
            String message;
            try {
                UpdateChecker.Result r = UpdateChecker.check(this);
                if (r.latest == null) message = "No release has been published yet.";
                else if (r.newer) message = "Version " + r.latest + " is available (see the top of this page).";
                else message = "You have the newest version (" + r.latest + " is the latest release).";
            } catch (IOException | RuntimeException e) {
                message = "Could not check: " + e.getMessage();
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
            if (out == null) throw new IOException("cannot write there");
            CrashReports.writeReport(this, out);
            CrashReports.markSeen(this);
            showBanners();
            if (updateStatus != null) updateStatus.setText("Log saved. Attach it to your bug report.");
        } catch (IOException | RuntimeException e) {
            if (updateStatus != null) updateStatus.setText("Could not save the log: " + e.getMessage());
        }
    }

    private void updatePlayHint() {
        if (playHint == null) return;
        if (DataSources.load(this).isEmpty()) {
            playHint.setText("Choose your game folder first (Game files, below).");
        } else if (quick != null && quick.isChecked()) {
            int st = GameOptions.stage(this);
            playHint.setText((st == 7 ? "Starts at the Extra stage" : "Starts at stage " + st)
                    + "  ·  practice, nothing is saved");
        } else {
            playHint.setText("Opens the game's title screen");
        }
    }

    /**
     * Replace music: pick one of the game's songs and an audio file of your own (MP3, AAC,
     * OGG, FLAC, WAV ...); it is converted to the game's format and played (and looped) in
     * that song's place. The game folder is not touched; "Restore" puts the original back.
     */
    private void addMusicSection(LinearLayout page) {
        LinearLayout card = ui.card(page, "Replace music",
                "Play your own audio file (MP3, AAC, OGG, FLAC, WAV …) in place of one of the game's "
                        + "songs. It is converted to WAV (44,100 Hz, stereo) and loops like the game's music.");
        card.addView(ui.caption("Song"), LauncherUi.matchWrap());
        musicTracks = ui.adapter(trackLabels());
        musicTrack = ui.spinner(musicTracks, 0, position -> showMusicStatus(null));
        card.addView(musicTrack, LauncherUi.matchWrap());
        card.addView(ui.caption("Loop from (seconds into your song; empty or 0 = the whole song)"), LauncherUi.matchWrap());
        loopFrom = ui.field("0", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        card.addView(loopFrom, LauncherUi.matchWrap());
        musicPick = ui.primary("Choose audio file");
        musicPick.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("audio/*");
            pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"audio/*", "application/ogg"});
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(pick, REQUEST_AUDIO);
        });
        card.addView(musicPick, ui.matchWrap(16, 8));
        musicRestore = ui.secondary("Restore the game's song");
        musicRestore.setOnClickListener(v -> {
            MusicReplacer.restore(this, musicTrack.getSelectedItemPosition());
            refreshTracks();
            showMusicStatus("Restored: the game's own song plays again.");
        });
        card.addView(musicRestore, ui.matchWrap(0, 4));
        musicStatus = ui.body("", 14, LauncherUi.INK);
        musicStatus.setPadding(ui.dp(4), ui.dp(10), ui.dp(4), 0);
        card.addView(musicStatus, LauncherUi.matchWrap());
        showMusicStatus(null);
    }

    private String[] trackLabels() {
        String[] labels = new String[MusicReplacer.TRACKS.length];
        for (int t = 0; t < labels.length; ++t) {
            labels[t] = MusicReplacer.TRACKS[t] + (MusicReplacer.isReplaced(this, t) ? "   (replaced)" : "");
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
        String state = now != null ? "Now: your music (" + now + ")" : "Now: the game's own song";
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
            showMusicStatus("The loop point must be a number of seconds.");
            return;
        }
        final double loopSeconds = loop;
        converting = true;
        showMusicStatus("Reading your file...");
        new Thread(() -> {
            String result;
            try {
                String summary = MusicReplacer.replace(this, audio, track, loopSeconds,
                        message -> runOnUiThread(() -> musicStatus.setText(message)));
                result = "Done: " + MusicReplacer.TRACKS[track].substring(4) + " now plays your music (" + summary + ").";
            } catch (IOException | RuntimeException e) {
                result = "Could not use that file: " + e.getMessage();
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
            status.setText("Android did not grant lasting access to that folder: " + e.getMessage());
            return;
        }
        scan(tree);
    }

    private void scan(Uri tree) {
        scanning = true;
        chooseButton.setEnabled(false);
        playButton.setEnabled(false);
        status.setText("Scanning…");
        new Thread(() -> {
            List<DataSources.Entry> found = DataSources.scanTree(this, tree,
                    path -> runOnUiThread(() -> status.setText("Scanning " + path)));
            if (!found.isEmpty()) {
                DataSources.save(this, tree, found);
                releaseOtherGrants(tree);
            }
            runOnUiThread(() -> {
                scanning = false;
                chooseButton.setEnabled(true);
                if (found.isEmpty()) {
                    status.setText("No game archives (.dat files starting with PBG3 or PKGL) "
                            + "were found in that folder or its subfolders.\n\n");
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
            sb.append("No game data selected yet. Choose the folder of the 2002 game"
                    + " (\u7d05\u9b54\u90f7CM.DAT, \u7d05\u9b54\u90f7ST.DAT ... or KOUMAKYO_*.dat).");
            playButton.setText("Play");
        } else {
            boolean classic = false;
            int tracks = 0;
            long musicBytes = 0;
            boolean hasScores = false;
            sb.append(String.format(Locale.ROOT, "%d file(s) in use:%n", entries.size()));
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
                String kind = isClassic ? "Classic"
                        : DataSources.FORMAT_EXE.equals(e.format) ? "Program" : "New Classic";
                sb.append(String.format(Locale.ROOT, "  %-18s %-11s %6.1f MB%n",
                        e.name, kind, e.size / 1048576.0));
            }
            sb.append(tracks > 0
                    ? String.format(Locale.ROOT, "  Music: %d tracks from bgm/ (%.0f MB)%n", tracks, musicBytes / 1048576.0)
                    : String.format(Locale.ROOT, "  Music: none (copy the game's bgm folder too for music)%n"));
            int replaced = MusicReplacer.replacedCount(this);
            if (replaced > 0) {
                sb.append(String.format(Locale.ROOT, "  Replaced music: %d track(s) (see below)%n", replaced));
            }
            if (hasScores) {
                sb.append(String.format(Locale.ROOT, "  High scores: score.dat from your PC (new scores are saved in the app)%n"));
            }
            if (!classic) {
                sb.append(String.format(Locale.ROOT, "%nThe stage engine runs the 2002 data files;"
                        + " New Classic archives alone cannot start a stage yet.%n"));
            }
            playButton.setText("Play");
        }
        status.setText(sb.toString().trim());
        updatePlayHint();
    }

}
