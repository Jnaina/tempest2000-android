package com.jnaina.tempest2000;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.view.Choreographer;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Tempest 2000.  A SurfaceView shows the game (the native code draws into its buffer), a transparent overlay draws the touch
 * controls, and a dedicated thread runs the game once per screen refresh from a Choreographer callback.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {

    /** Sizes the game view to the game's 4:3 shape, centred, and the controls to the whole screen. */
    private static final class GameLayout extends FrameLayout {
        private final View game, controls; float aspect = 4f / 3f;
        GameLayout(Context c, View game, View controls) { super(c); this.game = game; this.controls = controls; addView(game); addView(controls); setBackgroundColor(Color.BLACK); }
        @Override protected void onMeasure(int ws, int hs) {
            int W = MeasureSpec.getSize(ws), H = MeasureSpec.getSize(hs); setMeasuredDimension(W, H);
            int gw = W, gh = Math.round(W / aspect); if (gh > H) { gh = H; gw = Math.round(H * aspect); }
            game.measure(MeasureSpec.makeMeasureSpec(gw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(gh, MeasureSpec.EXACTLY));
            controls.measure(MeasureSpec.makeMeasureSpec(W, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(H, MeasureSpec.EXACTLY));
        }
        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int W = r - l, H = b - t, gw = game.getMeasuredWidth(), gh = game.getMeasuredHeight();
            game.layout((W - gw) / 2, (H - gh) / 2, (W + gw) / 2, (H + gh) / 2); controls.layout(0, 0, W, H);
        }
    }

    private ControlsView controls;
    private HandlerThread emuThread; private Handler emuHandler; private Choreographer choreographer;
    private volatile boolean running;
    private boolean nativeReady;
    private int keyMask, touchMask, padMask;

    private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
        @Override public void doFrame(long frameTimeNanos) {
            if (!running) return;
            Native.tick(frameTimeNanos);
            choreographer.postFrameCallback(this);
        }
    };

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        nativeReady = Native.init(getAssets(), getFilesDir().getPath());
        if (!nativeReady) {
            TextView t = new TextView(this); t.setText("Tempest 2000 could not start.\nThe game data (t2000.abs) is missing from the app:\nrun android/setup.sh and build again."); t.setTextColor(Color.WHITE); t.setTextSize(20); t.setPadding(48, 48, 48, 48);
            setContentView(t); return;
        }
        SurfaceView sv = new SurfaceView(this); sv.getHolder().addCallback(this);
        controls = new ControlsView(this, mask -> { touchMask = mask; controls.setHidden(false); push(); });
        GameLayout layout = new GameLayout(this, sv, controls); layout.aspect = Native.aspect() > 0.5f ? Native.aspect() : 4f / 3f;
        setContentView(layout);

        emuThread = new HandlerThread("t2k-emu", Process.THREAD_PRIORITY_URGENT_DISPLAY); emuThread.start();
        emuHandler = new Handler(emuThread.getLooper());
    }

    @Override protected void onResume() {
        super.onResume();
        if (!nativeReady) return;
        Native.start(); running = true;
        emuHandler.post(() -> { choreographer = Choreographer.getInstance(); choreographer.postFrameCallback(frameCallback); });
    }

    @Override protected void onPause() {
        if (nativeReady) {
            running = false;
            emuHandler.post(() -> { if (choreographer != null) choreographer.removeFrameCallback(frameCallback); });
            Native.stop();
            keyMask = touchMask = padMask = 0; push();
        }
        super.onPause();
    }

    @Override protected void onDestroy() {
        if (nativeReady) { if (emuThread != null) emuThread.quitSafely(); if (isFinishing()) Native.shutdown(); }
        super.onDestroy();
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    // ---- surface --------------------------------------------------------------------------------------------------
    @Override public void surfaceCreated(SurfaceHolder h) {
        Native.setSurface(h.getSurface());
        if (Build.VERSION.SDK_INT >= 30) h.getSurface().setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);   // steady 60 fps on 90/120 Hz phones
    }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { }
    @Override public void surfaceDestroyed(SurfaceHolder h) { Native.setSurface(null); }

    // ---- input ----------------------------------------------------------------------------------------------------
    private void push() { Native.setInput(keyMask | touchMask | padMask); }

    private static int maskForKey(int k) {
        switch (k) {
            case KeyEvent.KEYCODE_DPAD_LEFT: case KeyEvent.KEYCODE_A: return Native.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: case KeyEvent.KEYCODE_D: return Native.RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP: case KeyEvent.KEYCODE_W: return Native.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: case KeyEvent.KEYCODE_S: return Native.DOWN;
            case KeyEvent.KEYCODE_SPACE: case KeyEvent.KEYCODE_X: case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_DPAD_CENTER: return Native.FIRE;
            case KeyEvent.KEYCODE_Z: case KeyEvent.KEYCODE_BUTTON_B: return Native.JUMP;
            case KeyEvent.KEYCODE_C: case KeyEvent.KEYCODE_V: case KeyEvent.KEYCODE_BUTTON_X: case KeyEvent.KEYCODE_BUTTON_Y:
            case KeyEvent.KEYCODE_BUTTON_L1: case KeyEvent.KEYCODE_BUTTON_R1: case KeyEvent.KEYCODE_BUTTON_L2: case KeyEvent.KEYCODE_BUTTON_R2: return Native.ZAP;
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_O: case KeyEvent.KEYCODE_BUTTON_START: return Native.OPTION;
            case KeyEvent.KEYCODE_P: case KeyEvent.KEYCODE_DEL: case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_BUTTON_MODE: return Native.PAUSE;
            default: return 0;
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int m = maskForKey(e.getKeyCode());
        if (nativeReady && m != 0) {
            if (e.getAction() == KeyEvent.ACTION_DOWN) keyMask |= m; else if (e.getAction() == KeyEvent.ACTION_UP) keyMask &= ~m;
            controls.setHidden(true); push();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if (nativeReady && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK && e.getAction() == MotionEvent.ACTION_MOVE) {
            float x = e.getAxisValue(MotionEvent.AXIS_X), y = e.getAxisValue(MotionEvent.AXIS_Y), hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int m = 0;
            if (x < -.5f || hx < -.5f) m |= Native.LEFT;
            if (x > .5f || hx > .5f) m |= Native.RIGHT;
            if (y < -.5f || hy < -.5f) m |= Native.UP;
            if (y > .5f || hy > .5f) m |= Native.DOWN;
            if (e.getAxisValue(MotionEvent.AXIS_RTRIGGER) > .4f || e.getAxisValue(MotionEvent.AXIS_GAS) > .4f) m |= Native.ZAP;
            padMask = m; controls.setHidden(true); push();
            return true;
        }
        return super.onGenericMotionEvent(e);
    }
}
