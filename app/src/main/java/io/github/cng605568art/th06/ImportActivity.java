package io.github.cng605568art.th06;

import android.app.Activity;
import android.content.Intent;
import android.content.UriPermission;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;
import java.util.Locale;

/**
 * Launcher screen. The player picks the folder that holds their own copy of the game
 * (the 2002 original, or the New Classic Steam install's data folder); we only remember
 * read access to it. No game file is bundled with or copied into the app.
 */
public class ImportActivity extends Activity {
    private static final int REQUEST_TREE = 6;

    private TextView status;
    private Button chooseButton;
    private Button playButton;
    private CheckBox preferNewClassic;
    private volatile boolean scanning;

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

        playButton = new Button(this);
        playButton.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        column.addView(playButton, wrap());

        ScrollView scroll = new ScrollView(this);
        scroll.setFitsSystemWindows(true);
        scroll.addView(column);
        setContentView(scroll);

        refresh();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
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
            sb.append("No game data selected yet.");
            playButton.setText("Start demo (no game data)");
        } else {
            sb.append(String.format(Locale.ROOT, "%d archive(s) in use:%n", entries.size()));
            for (DataSources.Entry e : entries) {
                String kind = DataSources.FORMAT_PBG3.equals(e.format) ? "Classic" : "New Classic";
                sb.append(String.format(Locale.ROOT, "  %-18s %-11s %6.1f MB%n",
                        e.name, kind, e.size / 1048576.0));
            }
            playButton.setText("Play");
        }
        status.setText(sb.toString());
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }
}
