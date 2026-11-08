package io.github.cng605568art.th07;

import android.app.Application;

/** Installs the Java crash recorder for both the launcher and the game's process. */
public class Th07App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        Logs.install(this);
    }
}
