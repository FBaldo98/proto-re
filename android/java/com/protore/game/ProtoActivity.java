package com.protore.game;

import android.content.Context;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** SDL's activity running the native port (libmain.so: the recompiled engine and its platform layer). */
public class ProtoActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "FAudio", "dxvk_d3d9", "main"};
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new ProtoSurface(context);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The native side loads the bundled Vulkan driver (Turnip, on Adreno) from the extracted libraries.
        try {
            Os.setenv("PROTORE_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
        } catch (ErrnoException ignored) {
            // the system Vulkan driver is used
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);  // no dimming while playing
        // On-screen controls above SDL's surface; they feed SDL keyboard and mouse input.
        addContentView(new TouchControls(this), new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
