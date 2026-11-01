package io.github.cng605568art.th06;

import android.app.Activity;
import android.content.Intent;
import android.content.UriPermission;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.text.InputType;

import java.io.IOException;
import java.util.List;
import java.util.Locale;
import java.util.function.IntConsumer;

/**
 * Launcher screen. The player picks the folder that holds their own copy of the game
 * (the 2002 original, or the New Classic Steam install's data folder); we only remember
 * read access to it. No game file is bundled with or copied into the app.
 */
public class ImportActivity extends Activity {
    private static final int REQUEST_TREE = 6;
    private static final int REQUEST_AUDIO = 7;

    private TextView status;
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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = dp(20);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(pad, pad, pad, pad);
        column.setGravity(Gravity.CENTER_HORIZONTAL);

        TextView title = new TextView(this);
        title.setText("TH06 Android Port");
        title.setTextSize(24);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title, wrap());

        TextView help = new TextView(this);
        help.setText("Copy your own game folder to the phone (the 2002 original with its "
                + "紅魔郷**.DAT / KOUMAKYO_**.dat files, or the New Classic data folder with "
                + "th06**.dat), then choose it below. The files are read in place.");
        help.setPadding(0, dp(12), 0, dp(12));
        column.addView(help, wrap());

        chooseButton = new Button(this);
        chooseButton.setText("Choose game folder");
        chooseButton.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(pick, REQUEST_TREE);
        });
        column.addView(chooseButton, wrap());

        preferNewClassic = new CheckBox(this);
        preferNewClassic.setText("If both versions are found, prefer New Classic assets");
        preferNewClassic.setChecked(DataSources.preferNewClassic(this));
        preferNewClassic.setOnCheckedChangeListener(
                (b, checked) -> DataSources.setPreferNewClassic(this, checked));
        column.addView(preferNewClassic, wrap());

        status = new TextView(this);
        status.setPadding(0, dp(12), 0, dp(12));
        status.setTypeface(Typeface.MONOSPACE);
        column.addView(status, wrap());

        // The game opens at its own title screen (Start, Extra, Practice, Option). The quick
        // start skips it and goes straight into a stage chosen here.
        CheckBox quick = new CheckBox(this);
        quick.setText("Quick start: skip the title screen");
        quick.setChecked(GameOptions.quickStart(this));
        column.addView(quick, wrap());
        LinearLayout quickOptions = new LinearLayout(this);
        quickOptions.setOrientation(LinearLayout.VERTICAL);
        quickOptions.addView(label("Shot type"), wrap());
        quickOptions.addView(choice(GameOptions.SHOT_TYPES, GameOptions.shotType(this),
                v -> GameOptions.setShotType(this, v)), wrap());
        quickOptions.addView(label("Difficulty"), wrap());
        quickOptions.addView(choice(GameOptions.DIFFICULTIES, GameOptions.difficulty(this),
                v -> GameOptions.setDifficulty(this, v)), wrap());
        quickOptions.addView(label("Start at (Extra uses its own difficulty)"), wrap());
        quickOptions.addView(choice(GameOptions.STAGES, GameOptions.stage(this) - 1,
                v -> GameOptions.setStage(this, v + 1)), wrap());
        quickOptions.setVisibility(quick.isChecked() ? View.VISIBLE : View.GONE);
        quick.setOnCheckedChangeListener((b, checked) -> {
            GameOptions.setQuickStart(this, checked);
            quickOptions.setVisibility(checked ? View.VISIBLE : View.GONE);
        });
        column.addView(quickOptions, wrap());

        column.addView(label("Mischief (high scores are not saved while any is on)"), wrap());
        int mischief = GameOptions.mischief(this);
        for (int i = 0; i < GameOptions.MISCHIEF.length; ++i) {
            final int bit = i;
            CheckBox box = new CheckBox(this);
            box.setText(GameOptions.MISCHIEF[i]);
            box.setChecked((mischief & (1 << i)) != 0);
            box.setOnCheckedChangeListener((b, checked) -> GameOptions.setMischief(this, bit, checked));
            column.addView(box, wrap());
        }

        playButton = new Button(this);
        playButton.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        column.addView(playButton, wrap());

        addMusicSection(column);

        ScrollView scroll = new ScrollView(this);
        scroll.setFitsSystemWindows(true);
        scroll.addView(column);
        setContentView(scroll);

        refresh();
    }

    /**
     * Replace music: pick one of the game's songs and an audio file of your own (MP3, AAC,
     * OGG, FLAC, WAV ...); it is converted to the game's format and played (and looped) in
     * that song's place. The game folder is not touched; "Restore" puts the original back.
     */
    private void addMusicSection(LinearLayout column) {
        column.addView(label("Replace music (play your own audio file instead of one of the game's songs)"), wrap());
        musicTracks = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, trackLabels());
        musicTracks.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        musicTrack = new Spinner(this);
        musicTrack.setAdapter(musicTracks);
        musicTrack.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                showMusicStatus(null);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        column.addView(musicTrack, wrap());
        loopFrom = new EditText(this);
        loopFrom.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        loopFrom.setHint("Loop from (seconds into your song; empty or 0 = loop the whole song)");
        column.addView(loopFrom, wrap());
        musicPick = new Button(this);
        musicPick.setText("Choose audio file (MP3 ...)");
        musicPick.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("audio/*");
            pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"audio/*", "application/ogg"});
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(pick, REQUEST_AUDIO);
        });
        column.addView(musicPick, wrap());
        musicRestore = new Button(this);
        musicRestore.setText("Restore the game's song");
        musicRestore.setOnClickListener(v -> {
            MusicReplacer.restore(this, musicTrack.getSelectedItemPosition());
            refreshTracks();
            showMusicStatus("Restored: the game's own song plays again.");
        });
        column.addView(musicRestore, wrap());
        musicStatus = new TextView(this);
        musicStatus.setPadding(0, dp(6), 0, dp(12));
        column.addView(musicStatus, wrap());
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
        if (requestCode == REQUEST_AUDIO) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) convert(data.getData());
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
        playButton.setEnabled(true);
    }

    private void appendCurrent() {
        List<DataSources.Entry> entries = DataSources.load(this);
        StringBuilder sb = new StringBuilder(status.getText());
        if (entries.isEmpty()) {
            sb.append("No game data selected yet. Choose the folder of the 2002 game"
                    + " (\u7d05\u9b54\u90f7CM.DAT, \u7d05\u9b54\u90f7ST.DAT ... or KOUMAKYO_*.dat).");
            playButton.setText("Play (no game data selected)");
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
        status.setText(sb.toString());
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private TextView label(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setPadding(0, dp(10), 0, 0);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private Spinner choice(String[] items, int selected, IntConsumer onSelect) {
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, items);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        spinner.setSelection(selected);
        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                onSelect.accept(position);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        return spinner;
    }

    private static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }
}
