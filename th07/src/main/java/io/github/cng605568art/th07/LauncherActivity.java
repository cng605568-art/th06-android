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
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.text.DateFormat;
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
 * copied into the app's storage), sees which files are there, chooses the language, the start
 * options, the touch controls, practice tools and mischief, replaces songs, backs up, and
 * plays. No game file is bundled with the app.
 */
public class LauncherActivity extends Activity {
    private static final int REQUEST_FOLDER = 1;
    private static final int REQUEST_FILES = 2;
    private static final int REQUEST_LOG = 3;
    private static final int REQUEST_BACKUP = 4;
    private static final int REQUEST_RESTORE = 5;
    private static final int REQUEST_AUDIO = 6;
    private static final String EXTRA_RESTORED = "restored";

    // Imports, downloads, conversions and backups run on their own threads and report to
    // whichever launcher page is showing (the page is rebuilt after a language change or a
    // restore).
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static WeakReference<LauncherActivity> current = new WeakReference<>(null);
    private static volatile String lastImport;    // the latest import progress or result
    private static volatile String lastBackup;    // the latest backup/restore progress or result
    private static volatile String lastGameText;  // the latest game-text download message
    private static volatile String lastMusic;     // the latest music conversion message
    private static volatile String lastUpdate;    // the latest update check's result
    private static volatile boolean checkingUpdates;
    private static int musicTrack;                // the song chosen under Replace music

    private Ui ui;
    private TextView status;
    private TextView importStatus;
    private TextView playHint;
    private LinearLayout banners;
    private Button playButton;
    private Button folderButton;
    private Button filesButton;
    private Button removeButton;
    // Language and game text
    private Spinner gameTextChoice;
    private TextView gameTextStatus;
    private Button gameTextDownload;
    private Button gameTextRemove;
    // Start
    private LinearLayout stageRow;
    private LinearLayout livesRow;
    private CheckBox practice;
    private LinearLayout directRows;
    // Replace music
    private Spinner musicSpinner;
    private ArrayAdapter<String> musicTracks;
    private EditText loopFrom;
    private Button musicPick;
    private Button musicRestore;
    private TextView musicStatus;
    // Backup, help
    private CheckBox backupMusic;
    private TextView backupStatus;
    private Button backupButton;
    private Button restoreButton;
    private Button updateButton;
    private TextView updateStatus;
    private TextView logStatus;
    private boolean ready;     // th07.dat and th07.exe are imported and right
    private boolean imported;  // any of the files is imported

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(GameStorage.wrap(Lang.wrap(base)));  // (its own folder inside Touhou Launcher)  // the launcher's text in the chosen language
    }

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
        playButton = ui.hero(getString(R.string.th07_play));
        playButton.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        LinearLayout.LayoutParams playLp = Ui.matchWrap();
        playLp.setMargins(ui.dp(28), -ui.dp(32), ui.dp(28), 0);
        page.addView(playButton, playLp);
        playHint = ui.body("", 13, Ui.MUTED);
        playHint.setGravity(Gravity.CENTER_HORIZONTAL);
        playHint.setPadding(ui.dp(24), ui.dp(8), ui.dp(24), ui.dp(4));
        page.addView(playHint, Ui.matchWrap());
        // A crash last time, or a newer release: shown here, above everything else.
        banners = new LinearLayout(this);
        banners.setOrientation(LinearLayout.VERTICAL);
        page.addView(banners, Ui.matchWrap());

        addLanguageSection(page);
        addFilesSection(page);
        addStartSection(page);
        addControlsSection(page);
        addDisplaySection(page);
        addPracticeSection(page);
        addMischiefSection(page);
        addMusicSection(page);
        addBackupSection(page);
        addHelpSection(page);

        TextView footer = ui.body(getString(R.string.th07_footer), 12, Ui.MUTED);
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
        if (UpdateChecker.due(this)) checkForUpdates(false);
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

        TextView title = ui.title(getString(R.string.th07_header_title), 40, Color.WHITE);
        title.setShadowLayer(ui.dp(6), 0, ui.dp(2), 0x80000000);
        header.addView(title);
        TextView subtitle = ui.title(getString(R.string.th07_header_subtitle), 18, Ui.SAKURA_SOFT);
        subtitle.setPadding(0, ui.dp(8), 0, 0);
        header.addView(subtitle);
        TextView tag = ui.body(getString(R.string.th07_header_tag), 13, 0xFFE9D6F0);
        tag.setGravity(Gravity.CENTER_HORIZONTAL);
        tag.setPadding(0, ui.dp(8), 0, 0);
        header.addView(tag);
        return header;
    }

    /** Everything that follows files and background work, redrawn. */
    private void refresh() {
        status.setText(describeFiles());
        if (lastImport != null) importStatus.setText(lastImport);
        if (lastBackup != null) backupStatus.setText(lastBackup);
        if (lastUpdate != null) updateStatus.setText(lastUpdate);
        updateButton.setEnabled(!checkingUpdates);
        updateBusy();
        refreshGameText();
        showMusicStatus();
        updatePlayHint();
        showBanners();
    }

    private void updateBusy() {
        boolean files = GameFiles.importing();  // an import, or a game-text download or removal
        boolean busy = files || Backup.busy();
        folderButton.setEnabled(!busy);
        filesButton.setEnabled(!busy);
        removeButton.setEnabled(!busy && imported);
        playButton.setEnabled(!busy);
        backupButton.setEnabled(!busy && !MusicReplacer.busy());
        restoreButton.setEnabled(!busy && !MusicReplacer.busy());
    }

    private static void onUi(Consumer<LauncherActivity> action) {
        MAIN.post(() -> {
            LauncherActivity a = current.get();
            if (a != null && !a.isFinishing() && !a.isDestroyed()) action.accept(a);
        });
    }

    /** For a background task's messages: the application, in the launcher's language. */
    private Context appContext() {
        return Lang.wrap(getApplicationContext());
    }

    private void launch(Intent intent, int request, TextView errors) {
        try {
            startActivityForResult(intent, request);
        } catch (ActivityNotFoundException e) {
            errors.setText(R.string.th07_files_no_picker);
        }
    }

    private void openPage(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            updateStatus.setText(getString(R.string.th07_no_browser, url));
        }
    }

    // ---- banners ----------------------------------------------------------------------------

    private void showBanners() {
        banners.removeAllViews();
        if (Logs.hasUnseenCrash(this)) {
            LinearLayout card = ui.card(banners, getString(R.string.th07_crash_title), getString(R.string.th07_crash_body));
            Button save = ui.primary(getString(R.string.th07_crash_save));
            save.setOnClickListener(v -> saveLog());
            card.addView(save, ui.matchWrap(12, 4));
            Button dismiss = ui.secondary(getString(R.string.th07_crash_dismiss));
            dismiss.setOnClickListener(v -> {
                Logs.markSeen(this);
                showBanners();
            });
            card.addView(dismiss, ui.matchWrap(0, 0));
        }
        UpdateChecker.Result update = UpdateChecker.cached(this);
        if (update != null && update.newer) {
            LinearLayout card = ui.card(banners, getString(R.string.th07_update_title, update.latest),
                    getString(R.string.th07_update_body, Logs.versionName(this)));
            Button open = ui.primary(getString(R.string.th07_update_open));
            open.setOnClickListener(v -> openPage(update.page));
            card.addView(open, ui.matchWrap(12, 0));
        }
    }

    // ---- language and game text ---------------------------------------------------------------

    /** The launcher's language, and the game's own text: the original or a translation. */
    private void addLanguageSection(LinearLayout page) {
        String heading = getString(R.string.th07_lang_title);
        if (!heading.equals("Language")) heading += "  ·  Language";  // findable in any language
        LinearLayout card = ui.card(page, heading, getString(R.string.th07_lang_body));
        card.addView(ui.spinner(Lang.labels(this), Lang.chosenIndex(this), v -> {
            if (v == Lang.chosenIndex(this)) return;
            Lang.choose(this, v);
            lastGameText = null;
            recreate();  // every text on the page in the new language
        }), ui.matchWrap(8, 0));

        card.addView(ui.caption(getString(R.string.th07_gametext_caption)), Ui.matchWrap());
        String[] labels = {getString(R.string.th07_gametext_same), "日本語 (Original)", "English", "简体中文", "繁體中文", "Bahasa Melayu"};
        gameTextChoice = ui.spinner(labels, GameText.chosenIndex(this), v -> {
            if (v == GameText.chosenIndex(this)) return;
            GameText.choose(this, v);
            lastGameText = null;
            refreshGameText();
            updatePlayHint();
        });
        card.addView(gameTextChoice, Ui.matchWrap());
        gameTextStatus = ui.body("", 14, Ui.INK);
        gameTextStatus.setPadding(ui.dp(4), ui.dp(10), ui.dp(4), ui.dp(4));
        card.addView(gameTextStatus, Ui.matchWrap());
        gameTextDownload = ui.primary(getString(R.string.th07_gametext_download));
        gameTextDownload.setOnClickListener(v -> downloadGameText());
        card.addView(gameTextDownload, ui.matchWrap(6, 4));
        gameTextRemove = ui.secondary(getString(R.string.th07_gametext_remove));
        gameTextRemove.setOnClickListener(v -> removeGameText());
        card.addView(gameTextRemove, ui.matchWrap(0, 4));
        card.addView(ui.caption(getString(R.string.th07_gametext_credit)), Ui.matchWrap());
    }

    private void refreshGameText() {
        String code = GameText.effective(this);
        boolean translatable = GameText.patchFor(code) != null;
        String message = lastGameText;
        boolean busy = GameFiles.importing() || Backup.busy();
        if (GameText.downloading() && message != null) {
            gameTextStatus.setText(message);  // the download's progress
        } else {
            GameText.Status st = GameText.status(this, code);
            String state;
            if (!translatable) {
                state = getString(R.string.th07_gametext_original);
            } else if (st.downloaded) {
                state = getString(R.string.th07_gametext_installed, st.files, st.dialogue,
                        DateFormat.getDateInstance(DateFormat.MEDIUM).format(new Date(st.updated)));
            } else if (st.installed) {
                state = getString(R.string.th07_gametext_imported, st.files, st.dialogue);
            } else {
                state = getString(code.equals("en") ? R.string.th07_gametext_not_installed_en : R.string.th07_gametext_not_installed);
            }
            gameTextStatus.setText(message != null ? message + "\n" + state : state);
            gameTextDownload.setText(st.installed ? R.string.th07_gametext_update : R.string.th07_gametext_download);
            gameTextRemove.setVisibility(st.downloaded ? View.VISIBLE : View.GONE);
        }
        gameTextDownload.setVisibility(translatable ? View.VISIBLE : View.GONE);
        gameTextDownload.setEnabled(!busy);
        gameTextRemove.setEnabled(!busy);
        gameTextChoice.setEnabled(!GameText.downloading());
    }

    /** A game-text download's progress line, from any thread. */
    private static void reportGameText(String message) {
        lastGameText = message;
        onUi(a -> a.gameTextStatus.setText(message));
    }

    private void downloadGameText() {
        if (Backup.busy() || !GameText.beginDownload()) {
            gameTextStatus.setText(R.string.th07_gametext_busy);
            return;
        }
        final Context app = appContext();
        final String code = GameText.effective(this);
        reportGameText(getString(R.string.th07_gametext_connecting));
        updateBusy();
        refreshGameText();
        updatePlayHint();
        new Thread(() -> {
            String message;
            try {
                int n = GameText.download(app, code, LauncherActivity::reportGameText);
                message = app.getString(R.string.th07_gametext_done, n);
            } catch (GameText.NotAvailable e) {
                message = app.getString(R.string.th07_gametext_unavailable);
            } catch (IOException | RuntimeException e) {
                message = app.getString(R.string.th07_gametext_failed, String.valueOf(e.getMessage()));
            } finally {
                GameText.endDownload();
            }
            lastGameText = message;
            onUi(LauncherActivity::refresh);
        }, "th07-gametext").start();
    }

    private void removeGameText() {
        if (Backup.busy() || !GameFiles.beginImport()) {
            gameTextStatus.setText(R.string.th07_gametext_busy);
            return;
        }
        try {
            GameText.remove(this, GameText.effective(this));
            lastGameText = getString(R.string.th07_gametext_removed);
        } finally {
            GameFiles.endImport();
        }
        refresh();
    }

    // ---- game files -----------------------------------------------------------------------

    private void addFilesSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_files_title), getString(R.string.th07_files_body));
        status = ui.statusBox();
        card.addView(status, ui.matchWrap(12, 8));
        importStatus = ui.body("", 14, Ui.INK);
        importStatus.setPadding(ui.dp(4), ui.dp(2), ui.dp(4), ui.dp(6));
        card.addView(importStatus, Ui.matchWrap());
        folderButton = ui.primary(getString(R.string.th07_files_choose_folder));
        folderButton.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            launch(pick, REQUEST_FOLDER, importStatus);
        });
        card.addView(folderButton, ui.matchWrap(4, 4));
        filesButton = ui.secondary(getString(R.string.th07_files_choose_files));
        filesButton.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            launch(pick, REQUEST_FILES, importStatus);
        });
        card.addView(filesButton, ui.matchWrap(0, 4));
        removeButton = ui.secondary(getString(R.string.th07_files_remove));
        removeButton.setOnClickListener(v -> confirmRemove());
        card.addView(removeButton, ui.matchWrap(0, 0));
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
                what = getString(music ? R.string.th07_status_missing_optional : R.string.th07_status_missing_needed);
            } else if (!s.valid) {
                mark = "✗";
                what = getString(GameFiles.DAT.equals(name) ? R.string.th07_status_bad_dat
                        : GameFiles.EXE.equals(name) ? R.string.th07_status_bad_exe : R.string.th07_status_bad_bgm);
            } else {
                mark = "✓";
                what = GameFiles.EXE.equals(name) ? getString(R.string.th07_status_ok_exe)
                        : getString(R.string.th07_status_ok_size, s.size / 1048576.0);
            }
            imported |= s.present;
            valid[k] = s.valid;
            sb.append(String.format(Locale.ROOT, "%-10s %s  %s%n", name, mark, what));
        }
        boolean patch = GameFiles.englishPresent(this);
        imported |= patch;
        sb.append(String.format(Locale.ROOT, "%-10s %s  %s%n", GameFiles.THCRAP, patch ? "✓" : "–",
                patch ? getString(R.string.th07_status_english_ok, GameFiles.englishFiles(this)) : getString(R.string.th07_status_english_missing)));
        int replaced = MusicReplacer.replacedCount(this);
        if (replaced > 0) {
            sb.append(String.format(Locale.ROOT, "%-10s %s  %s%n", "music/", "✓", getString(R.string.th07_status_music_replaced, replaced)));
        }
        ready = valid[0] && valid[1];
        sb.append('\n').append(getString(!ready ? R.string.th07_status_not_ready
                : valid[2] ? R.string.th07_status_ready : R.string.th07_status_ready_no_music));
        return sb.toString();
    }

    private void confirmRemove() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.th07_files_remove_title)
                .setMessage(R.string.th07_files_remove_body)
                .setPositiveButton(R.string.th07_files_remove_ok, (d, which) -> {
                    if (Backup.busy() || !GameFiles.beginImport()) return;
                    try {
                        GameFiles.removeAll(this);
                    } finally {
                        GameFiles.endImport();
                    }
                    lastImport = getString(R.string.th07_files_removed);
                    refresh();
                })
                .setNegativeButton(R.string.th07_cancel, null)
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
        } else if (requestCode == REQUEST_AUDIO) {
            if (data.getData() != null) convert(data.getData());
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
            importStatus.setText(GameText.downloading() ? R.string.th07_gametext_busy : R.string.th07_import_busy);
            return;
        }
        final Context app = appContext();
        // Hold on to read access for the import's whole run, even if this page is recreated;
        // given back when it ends (the copies need no access to the originals).
        final List<Uri> held = holdAccess(tree != null ? Collections.singletonList(tree) : docs);
        report(getString(R.string.th07_import_starting));
        updateBusy();
        refreshGameText();
        updatePlayHint();
        new Thread(() -> {
            String result;
            try {
                GameFiles.Progress progress = LauncherActivity::report;
                List<GameFiles.Source> sources = tree != null ? GameFiles.scanFolder(app, tree, progress)
                        : GameFiles.describe(app, docs);
                result = GameFiles.importFrom(app, sources, tree == null, progress);
            } catch (IOException | RuntimeException e) {
                result = app.getString(R.string.th07_import_failed, String.valueOf(e.getMessage()));
            } finally {
                releaseAccess(app, held);
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

    /** Takes lasting read access to picked documents where it is offered (given back by releaseAccess). */
    private List<Uri> holdAccess(List<Uri> uris) {
        List<Uri> held = new ArrayList<>();
        for (Uri u : uris) {
            try {
                getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                held.add(u);
            } catch (SecurityException e) {
                // Not offered as lasting: the picker's grant still covers this run.
            }
        }
        return held;
    }

    private static void releaseAccess(Context app, List<Uri> held) {
        for (Uri u : held) {
            try {
                app.getContentResolver().releasePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
                // Already gone.
            }
        }
    }

    /** An import's progress line, from any thread. */
    private static void report(String message) {
        lastImport = message;
        onUi(a -> a.importStatus.setText(message));
    }

    // ---- start options ----------------------------------------------------------------------

    private void addStartSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_start_title), getString(R.string.th07_start_body));
        CheckBox skipTitle = ui.check(getString(R.string.th07_start_skip_title), Options.skipTitle(this));
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
        directRows.addView(ui.caption(getString(R.string.th07_start_shot)), Ui.matchWrap());
        directRows.addView(ui.spinner(getResources().getStringArray(R.array.th07_shot_types), Options.shotType(this), v -> {
            Options.setShotType(this, v);
            updatePlayHint();
        }), Ui.matchWrap());
        directRows.addView(ui.caption(getString(R.string.th07_start_difficulty)), Ui.matchWrap());
        directRows.addView(ui.spinner(getResources().getStringArray(R.array.th07_difficulties), Options.difficulty(this), v -> {
            Options.setDifficulty(this, v);
            updateStartRows();
            updatePlayHint();
        }), Ui.matchWrap());

        stageRow = new LinearLayout(this);
        stageRow.setOrientation(LinearLayout.VERTICAL);
        stageRow.addView(ui.caption(getString(R.string.th07_start_stage)), Ui.matchWrap());
        stageRow.addView(ui.spinner(getResources().getStringArray(R.array.th07_stages), Options.stage(this) - 1, v -> {
            Options.setStage(this, v + 1);
            updatePlayHint();
        }), Ui.matchWrap());
        directRows.addView(stageRow, Ui.matchWrap());

        livesRow = new LinearLayout(this);
        livesRow.setOrientation(LinearLayout.VERTICAL);
        livesRow.addView(ui.caption(getString(R.string.th07_start_lives)), Ui.matchWrap());
        livesRow.addView(ui.spinner(getResources().getStringArray(R.array.th07_lives), Options.lives(this),
                v -> Options.setLives(this, v)), Ui.matchWrap());
        directRows.addView(livesRow, Ui.matchWrap());

        practice = ui.check(getString(R.string.th07_start_practice), Options.practice(this));
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
            playHint.setText(GameText.downloading() ? R.string.th07_hint_gametext_downloading : R.string.th07_hint_importing);
            return;
        }
        if (Backup.busy()) {
            playHint.setText(R.string.th07_hint_backup);
            return;
        }
        if (!ready) {
            playHint.setText(R.string.th07_hint_import);
            return;
        }
        String hint;
        if (!Options.skipTitle(this)) {
            hint = getString(R.string.th07_hint_title);
        } else {
            String shot = getResources().getStringArray(R.array.th07_shot_types)[Options.shotType(this)];
            int difficulty = Options.difficulty(this);
            if (difficulty == Options.EXTRA) {
                hint = getString(R.string.th07_hint_extra, shot);
            } else if (difficulty == Options.PHANTASM) {
                hint = getString(R.string.th07_hint_phantasm, shot);
            } else {
                String name = getResources().getStringArray(R.array.th07_difficulties)[difficulty];
                hint = getString(R.string.th07_hint_stage, Options.stage(this), shot, name);
            }
        }
        if (GameText.stack(this).length > 0) hint += getString(R.string.th07_hint_gametext, GameText.label(GameText.effective(this)));
        playHint.setText(hint);
    }

    // ---- controls, display, practice tools, mischief ----------------------------------------

    /** Touch controls: drag-to-move or the D-pad, and how the buttons look and behave. */
    private void addControlsSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_controls_title), getString(R.string.th07_controls_body));
        card.addView(ui.caption(getString(R.string.th07_controls_movement)), Ui.matchWrap());
        LinearLayout dragOnly = new LinearLayout(this);
        dragOnly.setOrientation(LinearLayout.VERTICAL);
        card.addView(ui.spinner(getResources().getStringArray(R.array.th07_move_modes), Options.controlChoice(this, "mode", 0), v -> {
            Options.setControlChoice(this, "mode", v);
            dragOnly.setVisibility(v == 0 ? View.VISIBLE : View.GONE);
        }), Ui.matchWrap());
        dragOnly.addView(ui.caption(getString(R.string.th07_controls_drag_speed)), Ui.matchWrap());
        dragOnly.addView(ui.spinner(Options.DRAG_SPEEDS, Options.controlChoice(this, "speed", 2),
                v -> Options.setControlChoice(this, "speed", v)), Ui.matchWrap());
        dragOnly.setVisibility(Options.controlChoice(this, "mode", 0) == 0 ? View.VISIBLE : View.GONE);
        card.addView(dragOnly, Ui.matchWrap());
        card.addView(ui.caption(getString(R.string.th07_controls_button_size)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.th07_button_sizes), Options.controlChoice(this, "size", 1),
                v -> Options.setControlChoice(this, "size", v)), Ui.matchWrap());
        card.addView(ui.caption(getString(R.string.th07_controls_opacity)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.th07_opacities), Options.controlChoice(this, "opacity", 3),
                v -> Options.setControlChoice(this, "opacity", v)), Ui.matchWrap());
        String[][] flags = {
            {"autoFire", getString(R.string.th07_controls_autofire)},
            {"focusToggle", getString(R.string.th07_controls_focus_toggle)},
            {"left", getString(R.string.th07_controls_left)},
            {"vibrate", getString(R.string.th07_controls_vibrate)},
        };
        for (String[] f : flags) {
            final String key = f[0];
            CheckBox box = ui.check(f[1], Options.controlFlag(this, key, key.equals("vibrate")));
            box.setOnCheckedChangeListener((b, checked) -> Options.setControlFlag(this, key, checked));
            card.addView(box, Ui.matchWrap());
        }
    }

    /** How the game is shown: sideways or upright, smooth or sharp pixels, scanlines. */
    private void addDisplaySection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_display_title), getString(R.string.th07_display_body));
        card.addView(ui.caption(getString(R.string.th07_display_screen)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.th07_layouts), Options.controlChoice(this, "layout", 0),
                v -> Options.setControlChoice(this, "layout", v)), Ui.matchWrap());
        card.addView(ui.caption(getString(R.string.th07_display_scaling)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.th07_scalings), Options.controlChoice(this, "scaling", 0),
                v -> Options.setControlChoice(this, "scaling", v)), Ui.matchWrap());
        CheckBox scan = ui.check(getString(R.string.th07_display_scanlines), Options.controlFlag(this, "scanlines", false));
        scan.setOnCheckedChangeListener((b, checked) -> Options.setControlFlag(this, "scanlines", checked));
        card.addView(scan, Ui.matchWrap());
    }

    /** Practice tools (not in the original): slow motion, a hitbox marker, save states. */
    private void addPracticeSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_practice_title), getString(R.string.th07_practice_body));
        card.addView(ui.caption(getString(R.string.th07_practice_speed)), Ui.matchWrap());
        card.addView(ui.spinner(getResources().getStringArray(R.array.th07_speeds), Options.controlChoice(this, "speed%", 0),
                v -> Options.setControlChoice(this, "speed%", v)), Ui.matchWrap());
        String[][] flags = {
            {"hitbox", getString(R.string.th07_practice_hitbox)},
            {"saveStates", getString(R.string.th07_practice_states)},
        };
        for (String[] f : flags) {
            final String key = f[0];
            CheckBox box = ui.check(f[1], Options.controlFlag(this, key, false));
            box.setOnCheckedChangeListener((b, checked) -> Options.setControlFlag(this, key, checked));
            card.addView(box, Ui.matchWrap());
        }
    }

    /** Mischief (not in the original): while any is on, nothing from the game is saved. */
    private void addMischiefSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_mischief_title), getString(R.string.th07_mischief_body));
        int mischief = Options.mischief(this);
        String[] names = getResources().getStringArray(R.array.th07_mischief);
        for (int i = 0; i < names.length && i < Options.MISCHIEF_COUNT; ++i) {
            final int bit = i;
            CheckBox box = ui.check(names[i], (mischief & (1 << i)) != 0);
            box.setOnCheckedChangeListener((b, checked) -> Options.setMischief(this, bit, checked));
            card.addView(box, Ui.matchWrap());
        }
    }

    // ---- replace music ----------------------------------------------------------------------

    /**
     * Replace music: pick one of the game's songs and an audio file of your own (MP3, AAC,
     * OGG, FLAC, WAV ...); it is converted to WAV and played (and looped) in that song's
     * place. thbgm.dat is not touched; "Restore" puts the original back.
     */
    private void addMusicSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_music_title), getString(R.string.th07_music_body));
        card.addView(ui.caption(getString(R.string.th07_music_song)), Ui.matchWrap());
        musicTracks = ui.adapter(trackLabels());
        musicSpinner = ui.spinner(musicTracks, musicTrack, position -> {
            if (position != musicTrack && !MusicReplacer.busy()) lastMusic = null;
            musicTrack = position;
            showMusicStatus();
        });
        card.addView(musicSpinner, Ui.matchWrap());
        card.addView(ui.caption(getString(R.string.th07_music_loop)), Ui.matchWrap());
        loopFrom = ui.field("0", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        card.addView(loopFrom, Ui.matchWrap());
        musicPick = ui.primary(getString(R.string.th07_music_choose));
        musicPick.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("audio/*");
            pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"audio/*", "application/ogg"});
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            launch(pick, REQUEST_AUDIO, musicStatus);
        });
        card.addView(musicPick, ui.matchWrap(16, 8));
        musicRestore = ui.secondary(getString(R.string.th07_music_restore));
        musicRestore.setOnClickListener(v -> {
            if (MusicReplacer.busy() || Backup.busy()) return;
            MusicReplacer.restore(this, musicTrack);
            lastMusic = getString(R.string.th07_music_restored);
            refreshTracks();
            refresh();
        });
        card.addView(musicRestore, ui.matchWrap(0, 4));
        musicStatus = ui.body("", 14, Ui.INK);
        musicStatus.setPadding(ui.dp(4), ui.dp(10), ui.dp(4), 0);
        card.addView(musicStatus, Ui.matchWrap());
    }

    private String[] trackLabels() {
        String[] labels = new String[MusicReplacer.TRACK_COUNT];
        for (int t = 0; t < labels.length; ++t) {
            labels[t] = MusicReplacer.trackLabel(this, t)
                    + (MusicReplacer.isReplaced(this, t) ? "   " + getString(R.string.th07_music_replaced_mark) : "");
        }
        return labels;
    }

    private void refreshTracks() {
        musicTracks.clear();
        musicTracks.addAll(trackLabels());
        musicTracks.notifyDataSetChanged();
        musicSpinner.setSelection(musicTrack);
    }

    private void showMusicStatus() {
        boolean converting = MusicReplacer.busy();
        String message = lastMusic;
        String now = MusicReplacer.describe(this, musicTrack);
        if (converting && message != null) {
            musicStatus.setText(message);  // the conversion's progress
        } else {
            String state = now != null ? getString(R.string.th07_music_now_yours, now) : getString(R.string.th07_music_now_game);
            musicStatus.setText(message != null ? message + "\n" + state : state);
        }
        boolean busy = converting || Backup.busy();
        musicRestore.setEnabled(!busy && now != null);
        musicPick.setEnabled(!busy);
        musicSpinner.setEnabled(!busy);
    }

    /** A conversion's progress line, from any thread. */
    private static void reportMusic(String message) {
        lastMusic = message;
        onUi(a -> a.musicStatus.setText(message));
    }

    private void convert(Uri audio) {
        final int track = musicTrack;
        double loop = 0;
        try {
            String text = loopFrom.getText().toString().trim().replace(',', '.');
            if (!text.isEmpty()) loop = Double.parseDouble(text);
            if (Double.isNaN(loop) || Double.isInfinite(loop)) throw new NumberFormatException(text);
        } catch (NumberFormatException e) {
            lastMusic = getString(R.string.th07_music_loop_nan);
            showMusicStatus();
            return;
        }
        if (Backup.busy() || !MusicReplacer.begin()) {
            musicStatus.setText(R.string.th07_music_busy);
            return;
        }
        final double loopSeconds = loop;
        final Context app = appContext();
        final List<Uri> held = holdAccess(Collections.singletonList(audio));
        lastMusic = getString(R.string.th07_music_reading);
        updateBusy();
        showMusicStatus();
        new Thread(() -> {
            String result;
            try {
                String summary = MusicReplacer.replace(app, audio, track, loopSeconds, LauncherActivity::reportMusic);
                result = app.getString(R.string.th07_music_done, MusicReplacer.trackName(app, track), summary);
            } catch (IOException | RuntimeException e) {
                result = app.getString(R.string.th07_music_failed, String.valueOf(e.getMessage()));
            } finally {
                releaseAccess(app, held);
                MusicReplacer.end();
            }
            lastMusic = result;
            onUi(a -> {
                a.refreshTracks();
                a.refresh();
            });
        }, "th07-music").start();
    }

    // ---- backup -----------------------------------------------------------------------------

    /** Backup and restore of high scores, game options, replays, launcher settings and replaced music. */
    private void addBackupSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_backup_title), getString(R.string.th07_backup_body));
        backupMusic = ui.check(getString(R.string.th07_backup_music), true);
        card.addView(backupMusic, ui.matchWrap(4, 0));
        backupButton = ui.primary(getString(R.string.th07_backup_save));
        backupButton.setOnClickListener(v -> {
            Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            create.addCategory(Intent.CATEGORY_OPENABLE);
            create.setType("application/zip");
            create.putExtra(Intent.EXTRA_TITLE, Backup.suggestedName());
            launch(create, REQUEST_BACKUP, backupStatus);
        });
        card.addView(backupButton, ui.matchWrap(10, 4));
        restoreButton = ui.secondary(getString(R.string.th07_backup_restore));
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

    /** Takes the backup lock (imports, backups, restores and music conversions never overlap). */
    private boolean beginBackup() {
        if (GameFiles.importing() || MusicReplacer.busy() || !Backup.begin()) {
            backupStatus.setText(R.string.th07_backup_busy);
            return false;
        }
        updateBusy();
        updatePlayHint();
        showMusicStatus();
        return true;
    }

    private void backUp(Uri target) {
        if (!beginBackup()) return;
        final Context app = appContext();
        final boolean music = backupMusic.isChecked();
        reportBackup(getString(R.string.th07_backup_saving));
        new Thread(() -> {
            String message;
            try (OutputStream out = app.getContentResolver().openOutputStream(target, "wt")) {
                if (out == null) throw new IOException(app.getString(R.string.th07_err_cannot_write));
                message = app.getString(R.string.th07_backup_saved, Backup.create(app, out, music, LauncherActivity::reportBackup));
            } catch (IOException | RuntimeException e) {
                message = app.getString(R.string.th07_backup_save_failed, String.valueOf(e.getMessage()));
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
                .setTitle(R.string.th07_backup_confirm_title)
                .setMessage(R.string.th07_backup_confirm_body)
                .setPositiveButton(R.string.th07_backup_confirm_ok, (d, w) -> restore(source))
                .setNegativeButton(R.string.th07_cancel, null)
                .show();
    }

    private void restore(Uri source) {
        if (!beginBackup()) return;
        final Context app = appContext();
        reportBackup(getString(R.string.th07_backup_restoring));
        new Thread(() -> {
            String message;
            boolean ok = false;
            try (InputStream in = app.getContentResolver().openInputStream(source)) {
                if (in == null) throw new IOException(app.getString(R.string.th07_err_cannot_read));
                message = app.getString(R.string.th07_backup_restored, Backup.restore(app, in, LauncherActivity::reportBackup));
                ok = true;
            } catch (IOException | RuntimeException e) {
                message = app.getString(R.string.th07_backup_restore_failed, String.valueOf(e.getMessage()));
            } finally {
                Backup.end();
            }
            final String result = message;
            final boolean restored = ok;
            lastBackup = result;
            onUi(a -> {
                if (restored) {
                    // The launcher's settings (its language too) changed under its controls:
                    // rebuild the page.
                    a.getIntent().putExtra(EXTRA_RESTORED, result);
                    a.recreate();
                } else {
                    a.backupStatus.setText(result);
                    a.refresh();
                }
            });
        }, "th07-restore").start();
    }

    // ---- help & updates ---------------------------------------------------------------------

    private void addHelpSection(LinearLayout page) {
        LinearLayout card = ui.card(page, getString(R.string.th07_help_title),
                getString(R.string.th07_help_version, Logs.versionName(this), Logs.versionCode(this)));
        CheckBox auto = ui.check(getString(R.string.th07_help_auto), UpdateChecker.autoCheck(this));
        auto.setOnCheckedChangeListener((b, checked) -> UpdateChecker.setAutoCheck(this, checked));
        card.addView(auto, ui.matchWrap(4, 0));
        updateButton = ui.secondary(getString(R.string.th07_help_check));
        updateButton.setOnClickListener(v -> checkForUpdates(true));
        card.addView(updateButton, ui.matchWrap(6, 4));
        updateStatus = ui.body("", 14, Ui.INK);
        updateStatus.setPadding(ui.dp(4), ui.dp(4), ui.dp(4), ui.dp(6));
        card.addView(updateStatus, Ui.matchWrap());

        card.addView(ui.caption(getString(R.string.th07_help_bug)), Ui.matchWrap());
        Button log = ui.secondary(getString(R.string.th07_help_save_log));
        log.setOnClickListener(v -> saveLog());
        card.addView(log, ui.matchWrap(4, 4));
        Button issues = ui.secondary(getString(R.string.th07_help_report));
        issues.setOnClickListener(v -> openPage(UpdateChecker.ISSUES_PAGE));
        card.addView(issues, ui.matchWrap(0, 4));
        logStatus = ui.body("", 14, Ui.INK);
        logStatus.setPadding(ui.dp(4), ui.dp(4), ui.dp(4), 0);
        card.addView(logStatus, Ui.matchWrap());
    }

    /** Looks for a newer release (automatic checks only show their result as the banner). */
    private void checkForUpdates(boolean manual) {
        if (checkingUpdates) return;
        checkingUpdates = true;
        updateButton.setEnabled(false);
        if (manual) {
            lastUpdate = getString(R.string.th07_help_checking);
            updateStatus.setText(lastUpdate);
        }
        final Context app = appContext();
        new Thread(() -> {
            String message;
            try {
                UpdateChecker.Result r = UpdateChecker.check(app);
                if (r.latest == null) message = app.getString(R.string.th07_help_none_published);
                else if (r.newer) message = app.getString(R.string.th07_help_newer, r.latest);
                else message = app.getString(R.string.th07_help_latest, r.latest);
            } catch (IOException | RuntimeException e) {
                message = app.getString(R.string.th07_help_check_failed, String.valueOf(e.getMessage()));
            }
            if (manual) lastUpdate = message;
            checkingUpdates = false;
            onUi(LauncherActivity::refresh);
        }, "th07-update").start();
    }

    private void saveLog() {
        Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        create.addCategory(Intent.CATEGORY_OPENABLE);
        create.setType("text/plain");
        create.putExtra(Intent.EXTRA_TITLE, "th07-log-"
                + new SimpleDateFormat("yyyy-MM-dd-HHmm", Locale.ROOT).format(new Date()) + ".txt");
        launch(create, REQUEST_LOG, logStatus);
    }

    private void writeLog(Uri target) {
        try (OutputStream out = getContentResolver().openOutputStream(target, "wt")) {
            if (out == null) throw new IOException(getString(R.string.th07_err_cannot_write));
            Logs.writeReport(this, out);
            Logs.markSeen(this);
            showBanners();
            logStatus.setText(R.string.th07_help_log_saved);
        } catch (IOException | RuntimeException e) {
            logStatus.setText(getString(R.string.th07_help_log_failed, e.getMessage()));
        }
    }
}
