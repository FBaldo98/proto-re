package com.protore.game;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.util.HashMap;
import java.util.Map;

/**
 * On-screen controls drawn over the game. Everything goes into SDL as real keyboard and mouse input, so the engine
 * sees it both as window messages and through DirectInput.
 *
 * Two modes, switched with the button in the top-left corner:
 *  - menu: a touch is a left mouse click at that point (menus are mouse-driven), plus Enter and Esc buttons;
 *  - game: a stick on the left (WASD), action buttons on the right, and dragging anywhere else turns the camera
 *    (relative mouse motion, no click).
 */
public class TouchControls extends View {
    private static final int MOUSE_LEFT = 1, MOUSE_RIGHT = 2;

    /** A round button: either a key (Android keycode) or a mouse button. Position in fractions of the view. */
    private static final class Button {
        final String label;
        final int keycode, mouseButton;
        final float fx, fy, radiusDp;
        final boolean gameOnly;
        float cx, cy, r;
        int pointer = -1;

        Button(String label, int keycode, int mouseButton, float fx, float fy, float radiusDp, boolean gameOnly) {
            this.label = label;
            this.keycode = keycode;
            this.mouseButton = mouseButton;
            this.fx = fx;
            this.fy = fy;
            this.radiusDp = radiusDp;
            this.gameOnly = gameOnly;
        }

        boolean hit(float x, float y) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r * 1.2f;
        }
    }

    private final Button[] buttons = {
        // always visible
        new Button("Enter", KeyEvent.KEYCODE_ENTER, 0, 0.17f, 0.09f, 26, false),
        new Button("Esc", KeyEvent.KEYCODE_ESCAPE, 0, 0.95f, 0.09f, 26, false),
        // game mode: right-hand cluster
        new Button("Удар", 0, MOUSE_LEFT, 0.90f, 0.74f, 46, true),
        new Button("Спец", 0, MOUSE_RIGHT, 0.79f, 0.86f, 36, true),
        new Button("Прыжок", KeyEvent.KEYCODE_SPACE, 0, 0.92f, 0.47f, 36, true),
        new Button("Захват", KeyEvent.KEYCODE_E, 0, 0.79f, 0.62f, 32, true),
        new Button("Бег", KeyEvent.KEYCODE_SHIFT_LEFT, 0, 0.68f, 0.86f, 30, true),
        new Button("Цель", KeyEvent.KEYCODE_Q, 0, 0.82f, 0.38f, 28, true),
        new Button("Силы", KeyEvent.KEYCODE_TAB, 0, 0.70f, 0.09f, 26, true),
    };

    private final float density;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private boolean gameMode = false;
    private final RectF modeRect = new RectF();
    private int modePointer = -1;

    // stick
    private float stickX, stickY, stickR;
    private int stickPointer = -1;
    private float knobX, knobY;
    private final boolean[] dirDown = new boolean[4];  // W A S D
    private static final int[] DIR_KEYS = {KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_D};

    // camera / menu pointer
    private int lookPointer = -1;
    private float lookX, lookY;
    private int mouseState = 0;
    private float lastMouseX, lastMouseY;
    private final Map<Integer, Button> pointerButton = new HashMap<>();

    public TouchControls(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(2 * density);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(0xddffffff);
        setHapticFeedbackEnabled(true);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        for (Button b : buttons) {
            b.cx = b.fx * w;
            b.cy = b.fy * h;
            b.r = b.radiusDp * density;
        }
        stickR = 64 * density;
        stickX = 0.13f * w;
        stickY = 0.70f * h;
        knobX = stickX;
        knobY = stickY;
        modeRect.set(12 * density, 12 * density, 104 * density, 52 * density);
        lastMouseX = w / 2f;
        lastMouseY = h / 2f;
    }

    // ---- input to SDL ----
    private void key(int keycode, boolean down) {
        if (down) SDLActivity.onNativeKeyDown(keycode);
        else SDLActivity.onNativeKeyUp(keycode);
    }

    private void mouseButton(int bit, boolean down, float x, float y) {
        lastMouseX = x;
        lastMouseY = y;
        if (down) {
            mouseState |= bit;
            SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_DOWN, x, y, false);
        } else {
            mouseState &= ~bit;
            SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_UP, x, y, false);
        }
    }

    private void mouseMoveTo(float x, float y) {
        lastMouseX = x;
        lastMouseY = y;
        SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_MOVE, x, y, false);
    }

    private void mouseMoveBy(float dx, float dy) {
        SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_MOVE, dx, dy, true);
    }

    private void press(Button b, int pointer) {
        b.pointer = pointer;
        pointerButton.put(pointer, b);
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        if (b.mouseButton != 0) mouseButton(b.mouseButton, true, lastMouseX, lastMouseY);
        else key(b.keycode, true);
    }

    private void release(Button b) {
        pointerButton.remove(b.pointer);
        b.pointer = -1;
        if (b.mouseButton != 0) mouseButton(b.mouseButton, false, lastMouseX, lastMouseY);
        else key(b.keycode, false);
    }

    private void updateStick(float x, float y) {
        float dx = x - stickX, dy = y - stickY;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > stickR) {
            dx *= stickR / len;
            dy *= stickR / len;
        }
        knobX = stickX + dx;
        knobY = stickY + dy;
        float dead = stickR * 0.3f;
        boolean[] want = {dy < -dead, dx < -dead, dy > dead, dx > dead};
        for (int i = 0; i < 4; i++) {
            if (want[i] != dirDown[i]) {
                dirDown[i] = want[i];
                key(DIR_KEYS[i], want[i]);
            }
        }
    }

    private void releaseStick() {
        stickPointer = -1;
        updateStick(stickX, stickY);
    }

    private boolean visible(Button b) {
        return gameMode || !b.gameOnly;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        float x = e.getX(index), y = e.getY(index);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                if (modeRect.contains(x, y)) {
                    modePointer = id;
                    break;
                }
                Button hit = null;
                for (Button b : buttons) {
                    if (visible(b) && b.pointer < 0 && b.hit(x, y)) {
                        hit = b;
                        break;
                    }
                }
                if (hit != null) {
                    press(hit, id);
                } else if (gameMode && stickPointer < 0 && x < getWidth() * 0.4f && y > getHeight() * 0.35f) {
                    stickPointer = id;
                    stickX = x;  // the stick centres where the thumb lands
                    stickY = y;
                    updateStick(x, y);
                } else if (lookPointer < 0) {
                    lookPointer = id;
                    lookX = x;
                    lookY = y;
                    if (!gameMode) {
                        mouseMoveTo(x, y);
                        mouseButton(MOUSE_LEFT, true, x, y);
                    }
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int pid = e.getPointerId(i);
                    float px = e.getX(i), py = e.getY(i);
                    if (pid == stickPointer) {
                        updateStick(px, py);
                    } else if (pid == lookPointer) {
                        if (gameMode) mouseMoveBy((px - lookX) * 1.5f, (py - lookY) * 1.5f);
                        else mouseMoveTo(px, py);
                        lookX = px;
                        lookY = py;
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (action == MotionEvent.ACTION_CANCEL) {
                    for (Button b : buttons) if (b.pointer >= 0) release(b);
                    if (stickPointer >= 0) releaseStick();
                    if (lookPointer >= 0 && !gameMode) mouseButton(MOUSE_LEFT, false, lookX, lookY);
                    lookPointer = -1;
                    modePointer = -1;
                    break;
                }
                if (id == modePointer) {
                    modePointer = -1;
                    if (modeRect.contains(x, y)) {
                        gameMode = !gameMode;
                        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                        if (!gameMode && stickPointer >= 0) releaseStick();
                    }
                } else if (pointerButton.containsKey(id)) {
                    release(pointerButton.get(id));
                } else if (id == stickPointer) {
                    releaseStick();
                    stickX = 0.13f * getWidth();
                    stickY = 0.70f * getHeight();
                    knobX = stickX;
                    knobY = stickY;
                } else if (id == lookPointer) {
                    lookPointer = -1;
                    if (!gameMode) mouseButton(MOUSE_LEFT, false, x, y);
                }
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    // ---- drawing ----
    @Override
    protected void onDraw(Canvas canvas) {
        // mode switch
        fill.setColor(gameMode ? 0x66aa2222 : 0x66333333);
        canvas.drawRoundRect(modeRect, 10 * density, 10 * density, fill);
        stroke.setColor(0x88ffffff);
        canvas.drawRoundRect(modeRect, 10 * density, 10 * density, stroke);
        text.setTextSize(14 * density);
        canvas.drawText(gameMode ? "Игра" : "Меню", modeRect.centerX(), modeRect.centerY() + 5 * density, text);

        if (gameMode) {
            fill.setColor(0x33ffffff);
            canvas.drawCircle(stickX, stickY, stickR, fill);
            canvas.drawCircle(stickX, stickY, stickR, stroke);
            fill.setColor(stickPointer >= 0 ? 0x88ffffff : 0x55ffffff);
            canvas.drawCircle(knobX, knobY, stickR * 0.45f, fill);
        }
        for (Button b : buttons) {
            if (!visible(b)) continue;
            fill.setColor(b.pointer >= 0 ? 0x88ff5533 : 0x44000000);
            canvas.drawCircle(b.cx, b.cy, b.r, fill);
            canvas.drawCircle(b.cx, b.cy, b.r, stroke);
            text.setTextSize(Math.min(15 * density, b.r * 0.55f));
            canvas.drawText(b.label, b.cx, b.cy + text.getTextSize() * 0.35f, text);
        }
    }
}
