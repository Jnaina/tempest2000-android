package com.jnaina.tempest2000;

import android.content.res.AssetManager;
import android.view.Surface;

/** The native side (t2k_android.c): the Virtual Jaguar core running Tempest 2000. */
final class Native {
    static { System.loadLibrary("t2k"); }

    // libretro joypad bit numbers
    static final int B = 1 << 0, Y = 1 << 1, SELECT = 1 << 2, START = 1 << 3,
                     UP = 1 << 4, DOWN = 1 << 5, LEFT = 1 << 6, RIGHT = 1 << 7, A = 1 << 8;
    // Tempest 2000 / Jaguar pad
    static final int FIRE = B, JUMP = A, ZAP = Y, OPTION = START, PAUSE = SELECT;

    static native boolean init(AssetManager assets, String saveDir);
    static native void setSurface(Surface surface);   // null when the surface goes away
    static native void setInput(int mask);
    static native float aspect();
    static native void start();                        // begin: opens audio
    static native void stop();                         // pause: closes audio, saves high scores
    static native void tick(long frameTimeNanos);      // once per screen refresh: runs a game frame when due
    static native void shutdown();
}
