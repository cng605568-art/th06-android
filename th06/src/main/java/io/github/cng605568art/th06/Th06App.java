package io.github.cng605568art.th06;

import android.app.Application;

/** Installs the Java crash recorder for both the launcher and the game's process. */
public class Th06App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        CrashReports.install(this);
    }
}
