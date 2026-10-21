package com.protore.game;

import android.os.Bundle;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

/** SDL's activity running the native port (libmain.so: the recompiled engine and its platform layer). */
public class ProtoActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "FAudio", "dxvk_d3d9", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // On-screen controls above SDL's surface; they feed SDL keyboard and mouse input.
        addContentView(new TouchControls(this), new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
