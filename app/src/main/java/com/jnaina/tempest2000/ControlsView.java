package com.jnaina.tempest2000;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** Translucent on-screen controls: a d-pad, FIRE / JUMP / ZAP, and PAUSE / OPTION.  Multi-touch, slides between buttons. */
final class ControlsView extends View {
    interface Listener { void onTouchMask(int mask); }

    private static final class Btn {
        final int mask; final String label; final boolean circle;
        final RectF r = new RectF(); boolean pressed;
        Btn(int mask, String label, boolean circle) { this.mask = mask; this.label = label; this.circle = circle; }
        boolean hit(float x, float y) {
            if (circle) { float cx = r.centerX(), cy = r.centerY(), rad = r.width() / 2 * 1.2f; return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad; }
            return x >= r.left - r.width() * .1f && x <= r.right + r.width() * .1f && y >= r.top - r.height() * .1f && y <= r.bottom + r.height() * .1f;
        }
    }

    private final Btn left = new Btn(Native.LEFT, "◀", false), right = new Btn(Native.RIGHT, "▶", false),
                      up = new Btn(Native.UP, "▲", false), down = new Btn(Native.DOWN, "▼", false),
                      fire = new Btn(Native.FIRE, "FIRE", true), jump = new Btn(Native.JUMP, "JUMP", true), zap = new Btn(Native.ZAP, "ZAP", true),
                      pause = new Btn(Native.PAUSE, "PAUSE", false), option = new Btn(Native.OPTION, "OPTION", false);
    private final Btn[] all = { left, right, up, down, fire, jump, zap, pause, option };
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Listener listener;
    private boolean hidden;          // a keyboard / game controller is in use: the overlay fades out until the screen is touched

    ControlsView(Context c, Listener l) {
        super(c); listener = l;
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER); text.setColor(0xffffffff); text.setFakeBoldText(true);
    }

    void setHidden(boolean h) { if (h != hidden) { hidden = h; invalidate(); } }

    private void place(Btn b, float cx, float cy, float w, float h) { b.r.set(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2); }

    @Override protected void onSizeChanged(int W, int H, int ow, int oh) {
        float u = H / 100f;                                   // 1% of the screen height
        float dx = 24 * u, dy = H - 27 * u, a = 16 * u, off = 13.5f * u;
        place(left, dx - off, dy, a, a); place(right, dx + off, dy, a, a); place(up, dx, dy - off, a, a); place(down, dx, dy + off, a, a);
        place(fire, W - 21 * u, H - 27 * u, 26 * u, 26 * u);
        place(jump, W - 46 * u, H - 15 * u, 17 * u, 17 * u);
        place(zap, W - 13 * u, H - 55 * u, 17 * u, 17 * u);
        place(pause, W / 2f - 15 * u, 7 * u, 24 * u, 9 * u); place(option, W / 2f + 15 * u, 7 * u, 24 * u, 9 * u);
        stroke.setStrokeWidth(Math.max(2f, u * .5f)); text.setTextSize(u * 3.6f);
    }

    @Override protected void onDraw(Canvas c) {
        if (hidden) return;
        for (Btn b : all) {
            boolean big = b == fire;
            fill.setColor(b.pressed ? 0x99ffffff : 0x40ffffff); stroke.setColor(b.pressed ? 0xddffffff : 0x88ffffff);
            if (b.circle) { float rad = b.r.width() / 2; c.drawCircle(b.r.centerX(), b.r.centerY(), rad, fill); c.drawCircle(b.r.centerX(), b.r.centerY(), rad, stroke); }
            else { float rr = Math.min(b.r.width(), b.r.height()) * .25f; c.drawRoundRect(b.r, rr, rr, fill); c.drawRoundRect(b.r, rr, rr, stroke); }
            float ts = text.getTextSize(); if (big) text.setTextSize(ts * 1.5f);
            c.drawText(b.label, b.r.centerX(), b.r.centerY() + text.getTextSize() * .35f, text); text.setTextSize(ts);
        }
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int act = e.getActionMasked(), mask = 0;
        for (Btn b : all) b.pressed = false;
        if (act != MotionEvent.ACTION_UP && act != MotionEvent.ACTION_CANCEL) {
            for (int i = 0; i < e.getPointerCount(); i++) {
                if (act == MotionEvent.ACTION_POINTER_UP && i == e.getActionIndex()) continue;
                for (Btn b : all) if (b.hit(e.getX(i), e.getY(i))) { b.pressed = true; mask |= b.mask; }
            }
        }
        hidden = false;
        listener.onTouchMask(mask);
        invalidate();
        return true;
    }
}
