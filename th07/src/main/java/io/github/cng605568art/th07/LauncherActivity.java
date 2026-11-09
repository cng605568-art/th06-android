package io.github.cng605568art.th07;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;

/**
 * Launcher screen. The player imports their own copy of the game (th07.dat, th07.exe and,
 * for music, thbgm.dat: picked as a folder or as files with the Storage Access Framework and
 * copied into the app's storage), sees which files are there, chooses the start options and
 * the touch controls, and plays. No game file is bundled with the app.
 */
public class LauncherActivity extends Activity {
    private static final int REQUEST_FOLDER = 1;
    private static final int REQUEST_FILES = 2;
    private static final int REQUEST_LOG = 3;
    private static final int REQUEST_BACKUP = 4;
    private static final int REQUEST_RESTORE = 5;
    private static final String EXTRA_RESTORED = "restored";

    // An import runs on its own thread and reports to whichever launcher page is showing.
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static WeakReference<LauncherActivity> current = new WeakReference<>(null);
    private static volatile String lastImport;  // the latest import progress or result
    private static volatile String lastBackup;  // the latest backup/restore progress or result

    private Ui ui;
    private TextView status;
    private TextView importStatus;
    private TextView playHint;
    private TextView logStatus;
    private TextView backupStatus;
    private Button backupButton;
    private Button restoreButton;
    private Button playButton;
    private Button folderButton;
    private Button filesButton;
    private Button removeButton;
    private CheckBox english;
    private TextView englishHint;
    private LinearLayout stageRow;
    private LinearLayout livesRow;
    private CheckBox practice;
    private CheckBox skipTitle;
    private LinearLayout directRows;
    private boolean ready;     // th07.dat and th07.exe are imported and right
    private boolean imported;  // any of the files is imported

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        current = new WeakReference<>(this);
        ui = new Ui(this);
        drawEdgeToEdge();

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        LinearLayout header = buildHeader();
        page.addView(header, Ui.matchWrap());

        // Play, floating over the header's edge, with a line saying what it will do.
        playButton = ui.hero(getString(R.string.play));
        playButton.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        LinearLayout.LayoutParams playLp = Ui.matchWrap();
        playLp.setMargins(ui.dp(28), -ui.dp(32), ui.dp(28), 0);
        page.addView(playButton, playLp);
        playHint = ui.body("", 13, Ui.MUTED);
        playHint.setGravity(Gravity.CENTER_HORIZONTAL);
        playHint.setPadding(ui.dp(24), ui.dp(8), ui.dp(24), ui.dp(4));
        page.addView(playHint, Ui.matchWrap());

        addFilesSection(page);
        addStartSection(page);
        addControlsSection(page);
        addDisplaySection(page);
        addBackupSection(page);
        addHelpSection(page);

        TextView footer = ui.body(getString(R.string.footer), 12, Ui.MUTED);
        footer.setGravity(Gravity.CENTER_HORIZONTAL);
        footer.setPadding(ui.dp(28), ui.dp(10), ui.dp(28), ui.dp(8));
        page.addView(footer, Ui.matchWrap());

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Ui.PAPER);
        scroll.setClipToPadding(false);
        scroll.addView(page);
        setContentView(scroll);
        // The header runs under the status bar; the page keeps clear of the system bars.
        final int headerTop = ui.dp(28), headerBottom = ui.dp(52), side = ui.dp(20);
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

        String restored = getIntent().getStringExtra(EXTRA_RESTORED);
        if (restored != null) {
            lastBackup = restored;
            getIntent().removeExtra(EXTRA_RESTORED);
        }
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    @Override
    protected void onDestroy() {
        if (current.get() == this) current = new WeakReference<>(null);
        super.onDestroy();
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

    /** The plum header: the game's name in text (no artwork). */
    private LinearLayout buildHeader() {
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setGravity(Gravity.CENTER_HORIZONTAL);
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[] {Ui.PLUM_DEEP, Ui.PLUM, Ui.PLUM_LIGHT});
        float r = ui.dp(36);
        bg.setCornerRadii(new float[] {0, 0, 0, 0, r, r, r, r});
        header.setBackground(bg);

        TextView title = ui.title(getString(R.string.header_title), 40, Color.WHITE);
        title.setShadowLayer(ui.dp(6), 0, ui.dp(2), 0x80000000);
        header.addView(title);
        TextView subtitle = ui.title(getString(R.string.header_subtitle), 18, Ui.SAKURA_SOFT);
        subtitle.setPadding(0, ui.dp(8), 0, 0);
        header.addView(subtitle);
        TextView tag = ui.body(getString(R.string.header_tag), 13, 0xFFE9D6F0);
        tag.setGravity(Gravity.CENTER_HORIZONTAL);
        tag.setPadding(0, ui.dp(8), 0, 0);
        header.addView(tag);
        return header;
    }

    // ---- game files -----------------------------------------------------------------------

    private void addFilesSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.files_title), getString(R.string.files_body));
        status = ui.statusBox();
        card.addView(status, ui.matchWrap(12, 8));
        importStatus = ui.body("", 14, Ui.INK);
        importStatus.setPadding(ui.dp(4), ui.dp(2), ui.dp(4), ui.dp(6));
        card.addView(importStatus, Ui.matchWrap());
        folderButton = ui.primary(getString(R.string.files_choose_folder));
        folderButton.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            launch(pick, REQUEST_FOLDER, importStatus);
        });
        card.addView(folderButton, ui.matchWrap(4, 4));
        filesButton = ui.secondary(getString(R.string.files_choose_files));
        filesButton.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            launch(pick, REQUEST_FILES, importStatus);
        });
        card.addView(filesButton, ui.matchWrap(0, 4));
        removeButton = ui.secondary(getString(R.string.files_remove));
        removeButton.setOnClickListener(v -> confirmRemove());
        card.addView(removeButton, ui.matchWrap(0, 0));

        // The English patch imported from the game folder's thcrap folder: on by default.
        english = ui.check(getString(R.string.english_text), Options.englishChoice(this));
        english.setOnCheckedChangeListener(englishListener);
        card.addView(english, ui.matchWrap(12, 0));
        englishHint = ui.caption("");
        card.addView(englishHint, Ui.matchWrap());
    }

    private final android.widget.CompoundButton.OnCheckedChangeListener englishListener = (b, checked) -> {
        Options.setEnglishChoice(this, checked);
        updatePlayHint();
    };

    /** The checkbox follows the import: usable once the English patch is there. */
    private void updateEnglish() {
        boolean patch = GameFiles.englishPresent(this);
        english.setOnCheckedChangeListener(null);
        english.setChecked(patch && Options.englishChoice(this));
        english.setOnCheckedChangeListener(englishListener);
        english.setEnabled(patch && !GameFiles.importing());
        englishHint.setText(patch ? getString(R.string.english_hint_on, GameFiles.englishFiles(this)) : getString(R.string.english_hint_missing));
    }

    private void launch(Intent intent, int request, TextView errors) {
        try {
            startActivityForResult(intent, request);
        } catch (ActivityNotFoundException e) {
            errors.setText(R.string.files_no_picker);
        }
    }

    /** The file list: each of the three files, present and right or not. */
    private String describeFiles() {
        StringBuilder sb = new StringBuilder();
        imported = false;
        boolean[] valid = new boolean[3];
        String[] names = {GameFiles.DAT, GameFiles.EXE, GameFiles.BGM};
        for (int k = 0; k < names.length; ++k) {
            String name = names[k];
            GameFiles.State s = GameFiles.state(this, name);
            boolean music = GameFiles.BGM.equals(name);
            String mark;
            String what;
            if (!s.present) {
                mark = music ? "–" : "✗";
                what = getString(music ? R.string.status_missing_optional : R.string.status_missing_needed);
            } else if (!s.valid) {
                mark = "✗";
                what = getString(GameFiles.DAT.equals(name) ? R.string.status_bad_dat
                        : GameFiles.EXE.equals(name) ? R.string.status_bad_exe : R.string.status_bad_bgm);
            } else {
                mark = "✓";
                what = GameFiles.EXE.equals(name) ? getString(R.string.status_ok_exe)
                        : getString(R.string.status_ok_size, s.size / 1048576.0);
            }
            imported |= s.present;
            valid[k] = s.valid;
            sb.append(String.format(Locale.ROOT, "%-10s %s  %s%n", name, mark, what));
        }
        boolean patch = GameFiles.englishPresent(this);
        sb.append(String.format(Locale.ROOT, "%-10s %s  %s%n", GameFiles.THCRAP, patch ? "✓" : "–",
                patch ? getString(R.string.status_english_ok, GameFiles.englishFiles(this)) : getString(R.string.status_english_missing)));
        ready = valid[0] && valid[1];
        sb.append('\n').append(getString(!ready ? R.string.status_not_ready
                : valid[2] ? R.string.status_ready : R.string.status_ready_no_music));
        return sb.toString();
    }

    private void refresh() {
        status.setText(describeFiles());
        if (lastImport != null) importStatus.setText(lastImport);
        if (lastBackup != null) backupStatus.setText(lastBackup);
        updateBusy();
        updateEnglish();
        updatePlayHint();
    }

    private void updateBusy() {
        boolean busy = GameFiles.importing() || Backup.busy();
        folderButton.setEnabled(!busy);
        filesButton.setEnabled(!busy);
        removeButton.setEnabled(!busy && imported);
        playButton.setEnabled(!busy);
        english.setEnabled(!busy && GameFiles.englishPresent(this));
        backupButton.setEnabled(!busy);
        restoreButton.setEnabled(!busy);
    }

    private void confirmRemove() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.files_remove_title)
                .setMessage(R.string.files_remove_body)
                .setPositiveButton(R.string.files_remove_ok, (d, which) -> {
                    if (GameFiles.importing() || Backup.busy()) return;
                    GameFiles.removeAll(this);
                    lastImport = getString(R.string.files_removed);
                    refresh();
                })
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;
        if (requestCode == REQUEST_LOG) {
            if (data.getData() != null) writeLog(data.getData());
        } else if (requestCode == REQUEST_BACKUP) {
            if (data.getData() != null) backUp(data.getData());
        } else if (requestCode == REQUEST_RESTORE) {
            if (data.getData() != null) confirmRestore(data.getData());
        } else if (requestCode == REQUEST_FOLDER) {
            if (data.getData() != null) startImport(data.getData(), null);
        } else if (requestCode == REQUEST_FILES) {
            List<Uri> docs = new ArrayList<>();
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); ++i) {
                    Uri u = clip.getItemAt(i).getUri();
                    if (u != null) docs.add(u);
                }
            } else if (data.getData() != null) {
                docs.add(data.getData());
            }
            if (!docs.isEmpty()) startImport(null, docs);
        }
    }

    /** Imports from a picked folder (`tree`) or from picked files (`docs`) on a worker thread. */
    private void startImport(Uri tree, List<Uri> docs) {
        if (Backup.busy() || !GameFiles.beginImport()) {
            importStatus.setText(R.string.import_busy);
            return;
        }
        final Context app = getApplicationContext();
        // Hold on to read access for the import's whole run, even if this page is recreated;
        // given back when it ends (the copies need no access to the originals).
        final List<Uri> held = new ArrayList<>();
        for (Uri u : tree != null ? Collections.singletonList(tree) : docs) {
            try {
                getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                held.add(u);
            } catch (SecurityException e) {
                // Not offered as lasting: the picker's grant still covers this run.
            }
        }
        report(getString(R.string.import_starting));
        updateBusy();
        updatePlayHint();
        new Thread(() -> {
            String result;
            try {
                GameFiles.Progress progress = LauncherActivity::report;
                List<GameFiles.Source> sources = tree != null ? GameFiles.scanFolder(app, tree, progress)
                        : GameFiles.describe(app, docs);
                result = GameFiles.importFrom(app, sources, tree == null, progress);
            } catch (IOException | RuntimeException e) {
                result = app.getString(R.string.import_failed, String.valueOf(e.getMessage()));
            } finally {
                for (Uri u : held) {
                    try {
                        app.getContentResolver().releasePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    } catch (SecurityException ignored) {
                        // Already gone.
                    }
                }
                GameFiles.endImport();
            }
            final String message = result;
            lastImport = message;
            onUi(a -> {
                a.importStatus.setText(message);
                a.refresh();
            });
        }, "th07-import").start();
    }

    /** An import's progress line, from any thread. */
    private static void report(String message) {
        lastImport = message;
        onUi(a -> a.importStatus.setText(message));
    }

    private static void onUi(Consumer<LauncherActivity> action) {
        MAIN.post(() -> {
            LauncherActivity a = current.get();
            if (a != null && !a.isFinishing() && !a.isDestroyed()) action.accept(a);
        });
    }

    // ---- start options ----------------------------------------------------------------------

    private void addStartSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.start_title), getString(R.string.start_body));
        skipTitle = ui.check(getString(R.string.start_skip_title), Options.skipTitle(this));
        skipTitle.setOnCheckedChangeListener((b, checked) -> {
            Options.setSkipTitle(this, checked);
            updateStartRows();
            updatePlayHint();
        });
        card.addView(skipTitle, ui.matchWrap(6, 0));

        // The direct start's choices, shown while the title screen is skipped.
        directRows = new LinearLayout(this);
        directRows.setOrientation(LinearLayout.VERTICAL);
        card.addView(directRows, Ui.matchWrap());
        directRows.addView(ui.caption(getString(R.string.start_shot)), Ui.matchWrap());
        directRows.addView(ui.spinner(getResources().getStringArray(R.array.shot_types), Options.shotType(this), v -> {
            Options.setShotType(this, v);
            updatePlayHint();
        }), Ui.matchWrap());
        directRows.addView(ui.caption(getString(R.string.start_difficulty)), Ui.matchWrap());
        directRows.addView(ui.spinner(getResources().getStringArray(R.array.difficulties), Options.difficulty(this), v -> {
            Options.setDifficulty(this, v);
            updateStartRows();
            updatePlayHint();
        }), Ui.matchWrap());

        stageRow = new LinearLayout(this);
        stageRow.setOrientation(LinearLayout.VERTICAL);
        stageRow.addView(ui.caption(getString(R.string.start_stage)), Ui.matchWrap());
        stageRow.addView(ui.spinner(getResources().getStringArray(R.array.stages), Options.stage(this) - 1, v -> {
            Options.setStage(this, v + 1);
            updatePlayHint();
        }), Ui.matchWrap());
        directRows.addView(stageRow, Ui.matchWrap());

        livesRow = new LinearLayout(this);
        livesRow.setOrientation(LinearLayout.VERTICAL);
        livesRow.addView(ui.caption(getString(R.string.start_lives)), Ui.matchWrap());
        livesRow.addView(ui.spinner(getResources().getStringArray(R.array.lives), Options.lives(this),
                v -> Options.setLives(this, v)), Ui.matchWrap());
        directRows.addView(livesRow, Ui.matchWrap());

        practice = ui.check(getString(R.string.start_practice), Options.practice(this));
        practice.setOnCheckedChangeListener((b, checked) -> {
            Options.setPractice(this, checked);
            updateStartRows();
        });
        directRows.addView(practice, ui.matchWrap(6, 0));
        updateStartRows();
    }

    /** Extra and Phantasm have their own stage and lives; practice sets its own lives. */
    private void updateStartRows() {
        directRows.setVisibility(Options.skipTitle(this) ? View.VISIBLE : View.GONE);
        boolean special = Options.difficulty(this) >= Options.EXTRA;
        stageRow.setVisibility(special ? View.GONE : View.VISIBLE);
        practice.setVisibility(special ? View.GONE : View.VISIBLE);
        livesRow.setVisibility(special || Options.practice(this) ? View.GONE : View.VISIBLE);
    }

    private void updatePlayHint() {
        if (playHint == null) return;
        if (GameFiles.importing()) {
            playHint.setText(R.string.hint_importing);
            return;
        }
        if (Backup.busy()) {
            playHint.setText(R.string.hint_backup);
            return;
        }
        if (!ready) {
            playHint.setText(R.string.hint_import);
            return;
        }
        String hint;
        if (!Options.skipTitle(this)) {
            hint = getString(R.string.hint_title);
        } else {
            String shot = getResources().getStringArray(R.array.shot_types)[Options.shotType(this)];
            int difficulty = Options.difficulty(this);
            if (difficulty == Options.EXTRA) {
                hint = getString(R.string.hint_extra, shot);
            } else if (difficulty == Options.PHANTASM) {
                hint = getString(R.string.hint_phantasm, shot);
            } else {
                String name = getResources().getStringArray(R.array.difficulties)[difficulty];
                hint = getString(R.string.hint_stage, Options.stage(this), shot, name);
            }
        }
        if (Options.english(this)) hint += getString(R.string.hint_english);
        playHint.setText(hint);
    }

    // ---- controls and display ---------------------------------------------------------------

    /** Touch controls: drag-to-move or the D-pad, and how the buttons look and behave. */
    private void addControlsSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.controls_title), getString(R.string.controls_body));
        card.addView(ui.caption(getString(R.string.controls_movement)), Ui.matchWrap());
        LinearLayout dragOnly = new LinearLayout(this);
        dragOnly.setOrientation(LinearLayout.VERTICAL);
        card.addView(ui.spinner(getResources().getStringArray(R.array.move_modes), Options.controlChoice(this, "mode", 0), v -> {
            Options.setControlChoice(this, "mode", v);
            dragOnly.setVisibility(v == 0 ? View.VISIBLE : View.GONE);
        }), Ui.matchWrap());
        dragOnly.addView(ui.caption(getString(R.string.controls_drag_speed)), Ui.matchWrap());
        dragOnly.addView(ui.spinner(Options.DRAG_SPEEDS, Options.controlChoice(this, "speed", 2),
                v -> Options.setControlChoice(this, "speed", v)), Ui.matchWrap());
        dragOnly.setVisibility(Options.controlChoice(this, "mode", 0) == 0 ? View.VISIBLE : View.GONE);
        card.addView(dragOnly, Ui.matchWrap());
        card.addView(ui.caption(getString(R.string.controls_button_size)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.button_sizes), Options.controlChoice(this, "size", 1),
                v -> Options.setControlChoice(this, "size", v)), Ui.matchWrap());
        card.addView(ui.caption(getString(R.string.controls_opacity)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.opacities), Options.controlChoice(this, "opacity", 3),
                v -> Options.setControlChoice(this, "opacity", v)), Ui.matchWrap());
        String[][] flags = {
            {"autoFire", getString(R.string.controls_autofire)},
            {"focusToggle", getString(R.string.controls_focus_toggle)},
            {"left", getString(R.string.controls_left)},
        };
        for (String[] f : flags) {
            final String key = f[0];
            CheckBox box = ui.check(f[1], Options.controlFlag(this, key, false));
            box.setOnCheckedChangeListener((b, checked) -> Options.setControlFlag(this, key, checked));
            card.addView(box, Ui.matchWrap());
        }
    }

    /** How the game is shown: sideways or upright, smooth or sharp pixels, scanlines. */
    private void addDisplaySection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.display_title), getString(R.string.display_body));
        card.addView(ui.caption(getString(R.string.display_screen)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.layouts), Options.controlChoice(this, "layout", 0),
                v -> Options.setControlChoice(this, "layout", v)), Ui.matchWrap());
        card.addView(ui.caption(getString(R.string.display_scaling)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.scalings), Options.controlChoice(this, "scaling", 0),
                v -> Options.setControlChoice(this, "scaling", v)), Ui.matchWrap());
        CheckBox scan = ui.check(getString(R.string.display_scanlines), Options.controlFlag(this, "scanlines", false));
        scan.setOnCheckedChangeListener((b, checked) -> Options.setControlFlag(this, "scanlines", checked));
        card.addView(scan, Ui.matchWrap());
    }

    // ---- backup -----------------------------------------------------------------------------

    /** Backup and restore of high scores, game options, replays and launcher settings. */
    private void addBackupSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.backup_title), getString(R.string.backup_body));
        backupButton = ui.primary(getString(R.string.backup_save));
        backupButton.setOnClickListener(v -> {
            Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            create.addCategory(Intent.CATEGORY_OPENABLE);
            create.setType("application/zip");
            create.putExtra(Intent.EXTRA_TITLE, Backup.suggestedName());
            launch(create, REQUEST_BACKUP, backupStatus);
        });
        card.addView(backupButton, ui.matchWrap(10, 4));
        restoreButton = ui.secondary(getString(R.string.backup_restore));
        restoreButton.setOnClickListener(v -> {
            Intent open = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            open.addCategory(Intent.CATEGORY_OPENABLE);
            open.setType("*/*");
            open.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            launch(open, REQUEST_RESTORE, backupStatus);
        });
        card.addView(restoreButton, ui.matchWrap(0, 0));
        backupStatus = ui.body("", 14, Ui.INK);
        backupStatus.setPadding(ui.dp(4), ui.dp(10), ui.dp(4), 0);
        card.addView(backupStatus, Ui.matchWrap());
    }

    /** A backup or restore line, from any thread. */
    private static void reportBackup(String message) {
        lastBackup = message;
        onUi(a -> a.backupStatus.setText(message));
    }

    /** Takes the backup lock (imports, backups and restores never overlap). */
    private boolean beginBackup() {
        if (GameFiles.importing() || !Backup.begin()) {
            backupStatus.setText(R.string.backup_busy);
            return false;
        }
        updateBusy();
        updatePlayHint();
        return true;
    }

    private void backUp(Uri target) {
        if (!beginBackup()) return;
        final Context app = getApplicationContext();
        reportBackup(getString(R.string.backup_saving));
        new Thread(() -> {
            String message;
            try (OutputStream out = app.getContentResolver().openOutputStream(target, "wt")) {
                if (out == null) throw new IOException(app.getString(R.string.err_cannot_write));
                message = app.getString(R.string.backup_saved, Backup.create(app, out));
            } catch (IOException | RuntimeException e) {
                message = app.getString(R.string.backup_save_failed, String.valueOf(e.getMessage()));
            } finally {
                Backup.end();
            }
            final String result = message;
            lastBackup = result;
            onUi(a -> {
                a.backupStatus.setText(result);
                a.refresh();
            });
        }, "th07-backup").start();
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
        if (!beginBackup()) return;
        final Context app = getApplicationContext();
        reportBackup(getString(R.string.backup_restoring));
        new Thread(() -> {
            String message;
            boolean ok = false;
            try (InputStream in = app.getContentResolver().openInputStream(source)) {
                if (in == null) throw new IOException(app.getString(R.string.err_cannot_read));
                message = app.getString(R.string.backup_restored, Backup.restore(app, in, LauncherActivity::reportBackup));
                ok = true;
            } catch (IOException | RuntimeException e) {
                message = app.getString(R.string.backup_restore_failed, String.valueOf(e.getMessage()));
            } finally {
                Backup.end();
            }
            final String result = message;
            final boolean restored = ok;
            lastBackup = result;
            onUi(a -> {
                if (restored) {
                    // The launcher's settings changed under its controls: rebuild the page.
                    a.getIntent().putExtra(EXTRA_RESTORED, result);
                    a.recreate();
                } else {
                    a.backupStatus.setText(result);
                    a.refresh();
                }
            });
        }, "th07-restore").start();
    }

    // ---- help -------------------------------------------------------------------------------

    private void addHelpSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.help_title),
                getString(R.string.help_version, Logs.versionName(this), Logs.versionCode(this)));
        card.addView(ui.caption(getString(R.string.help_bug)), Ui.matchWrap());
        logStatus = ui.body("", 14, Ui.INK);
        Button log = ui.secondary(getString(R.string.help_save_log));
        log.setOnClickListener(v -> {
            Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            create.addCategory(Intent.CATEGORY_OPENABLE);
            create.setType("text/plain");
            create.putExtra(Intent.EXTRA_TITLE, "th07-log-"
                    + new SimpleDateFormat("yyyy-MM-dd-HHmm", Locale.ROOT).format(new Date()) + ".txt");
            launch(create, REQUEST_LOG, logStatus);
        });
        card.addView(log, ui.matchWrap(4, 4));
        logStatus.setPadding(ui.dp(4), ui.dp(4), ui.dp(4), 0);
        card.addView(logStatus, Ui.matchWrap());
    }

    private void writeLog(Uri target) {
        try (OutputStream out = getContentResolver().openOutputStream(target, "wt")) {
            if (out == null) throw new IOException(getString(R.string.err_cannot_write));
            Logs.writeReport(this, out);
            logStatus.setText(R.string.help_log_saved);
        } catch (IOException | RuntimeException e) {
            logStatus.setText(getString(R.string.help_log_failed, e.getMessage()));
        }
    }
}
