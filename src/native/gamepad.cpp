// XInput 1.3 for the native host on SDL game controllers (any pad SDL knows: Xbox, PlayStation, Android
// Bluetooth pads). Pads are given XInput slots 0..3 in the order they are first seen. On Android the on-screen
// controls can also act as a pad (TouchControls.nativePad): it answers as slot 0 while no real pad holds it.
#include <SDL2/SDL.h>
#ifdef __ANDROID__
#include <SDL2/SDL_system.h>
#include <jni.h>
#endif

#include <atomic>
#include <cstring>
#include <mutex>

#include "guest.h"

namespace native {

namespace {

constexpr uint32_t kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED

std::mutex g_mutex;
SDL_GameController* g_pads[4] = {};
uint32_t g_packet = 0;

// The on-screen pad: XINPUT_GAMEPAD fields, written by the UI thread.
struct TouchPad {
    std::atomic<bool> active{false};
    std::atomic<uint16_t> buttons{0};
    std::atomic<uint8_t> lt{0}, rt{0};
    std::atomic<int16_t> lx{0}, ly{0}, rx{0}, ry{0};
} g_touch;

#ifdef __ANDROID__
// The game's rumble on the on-screen pad becomes a phone vibration (TouchControls.rumble, 0..255).
jclass g_controls = nullptr;  // global ref, taken on the UI thread (FindClass sees app classes only there)
int g_rumble = 0;

void PhoneRumble(uint16_t left, uint16_t right) {
    int amp = (left > right ? left : right) >> 8;
    if (amp == g_rumble || !g_controls) return;
    g_rumble = amp;
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    static jmethodID rumble = env ? env->GetStaticMethodID(g_controls, "rumble", "(I)V") : nullptr;
    if (env && rumble) env->CallStaticVoidMethod(g_controls, rumble, jint(amp));
}
#endif

// Fills free slots with controllers that are not assigned yet and drops detached ones.
SDL_GameController* Pad(uint32_t index) {
    if (index >= 4) return nullptr;
    for (auto*& p : g_pads) {
        if (p && !SDL_GameControllerGetAttached(p)) {
            SDL_GameControllerClose(p);
            p = nullptr;
        }
    }
    if (!g_pads[index]) {
        for (int j = 0; j < SDL_NumJoysticks(); ++j) {
            if (!SDL_IsGameController(j)) continue;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
            bool used = false;
            for (auto* p : g_pads) {
                if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) used = true;
            }
            if (used) continue;
            g_pads[index] = SDL_GameControllerOpen(j);
            if (g_pads[index]) Log("xinput: pad %u = %s", index, SDL_GameControllerName(g_pads[index]));
            break;
        }
    }
    return g_pads[index];
}

int16_t Axis(SDL_GameController* p, SDL_GameControllerAxis a, bool invert) {
    int v = SDL_GameControllerGetAxis(p, a);
    if (invert) v = -v - 1;  // SDL: down is positive; XInput: up is positive
    return int16_t(v);
}

void x_XInputGetState(Ctx* c) {
    // XINPUT_STATE: dwPacketNumber, XINPUT_GAMEPAD {wButtons, bLeftTrigger, bRightTrigger, sThumbLX, sThumbLY,
    // sThumbRX, sThumbRY}
    std::lock_guard<std::mutex> lock(g_mutex);
    SDL_GameController* p = Pad(Arg(c, 0));
    if (!p && Arg(c, 0) == 0 && g_touch.active.load()) {
        uint32_t s = Arg(c, 1);
        W32(s, ++g_packet);
        W16(s + 4, g_touch.buttons.load());
        W8(s + 6, g_touch.lt.load());
        W8(s + 7, g_touch.rt.load());
        W16(s + 8, uint16_t(g_touch.lx.load()));
        W16(s + 10, uint16_t(g_touch.ly.load()));
        W16(s + 12, uint16_t(g_touch.rx.load()));
        W16(s + 14, uint16_t(g_touch.ry.load()));
        Return(c, 0, 2);
        return;
    }
    if (!p) {
        Return(c, kNotConnected, 2);
        return;
    }
    static const struct {
        SDL_GameControllerButton b;
        uint16_t x;
    } kButtons[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0001}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0004}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_CONTROLLER_BUTTON_START, 0x0010}, {SDL_CONTROLLER_BUTTON_BACK, 0x0020},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0040}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200},
        {SDL_CONTROLLER_BUTTON_A, 0x1000}, {SDL_CONTROLLER_BUTTON_B, 0x2000},
        {SDL_CONTROLLER_BUTTON_X, 0x4000}, {SDL_CONTROLLER_BUTTON_Y, 0x8000},
    };
    uint16_t buttons = 0;
    for (const auto& b : kButtons) {
        if (SDL_GameControllerGetButton(p, b.b)) buttons |= b.x;
    }
    uint32_t s = Arg(c, 1);
    W32(s, ++g_packet);
    W16(s + 4, buttons);
    W8(s + 6, uint8_t(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7));
    W8(s + 7, uint8_t(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7));
    W16(s + 8, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_LEFTX, false)));
    W16(s + 10, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_LEFTY, true)));
    W16(s + 12, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_RIGHTX, false)));
    W16(s + 14, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_RIGHTY, true)));
    Return(c, 0, 2);
}

void x_XInputSetState(Ctx* c) {
    // XINPUT_VIBRATION: wLeftMotorSpeed, wRightMotorSpeed
    std::lock_guard<std::mutex> lock(g_mutex);
    SDL_GameController* p = Pad(Arg(c, 0));
    if (!p) {
        const bool touch = Arg(c, 0) == 0 && g_touch.active.load();
#ifdef __ANDROID__
        if (touch) PhoneRumble(R16(Arg(c, 1)), R16(Arg(c, 1) + 2));
#endif
        Return(c, touch ? 0 : kNotConnected, 2);
        return;
    }
    SDL_GameControllerRumble(p, R16(Arg(c, 1)), R16(Arg(c, 1) + 2), 1000);
    Return(c, 0, 2);
}

void x_XInputGetCapabilities(Ctx* c) {
    // XINPUT_CAPABILITIES: Type, SubType, Flags, XINPUT_GAMEPAD Gamepad, XINPUT_VIBRATION Vibration
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Pad(Arg(c, 0)) && !(Arg(c, 0) == 0 && g_touch.active.load())) {
        Return(c, kNotConnected, 3);
        return;
    }
    uint32_t caps = Arg(c, 2);
    memset(Ptr<void>(caps), 0, 20);
    W8(caps, 1);      // XINPUT_DEVTYPE_GAMEPAD
    W8(caps + 1, 1);  // XINPUT_DEVSUBTYPE_GAMEPAD
    W16(caps + 4, 0xf3ff);
    W8(caps + 6, 0xff);
    W8(caps + 7, 0xff);
    for (uint32_t i = 0; i < 4; ++i) W16(caps + 8 + 2 * i, 0xffc0);
    W16(caps + 16, 0xffff);
    W16(caps + 18, 0xffff);
    Return(c, 0, 3);
}

// XInput 1.3 is imported by ordinal: #2 XInputGetState, #3 XInputSetState, #4 XInputGetCapabilities.
const ImportDef kXInput[] = {
    {"xinput1_3.dll", "#2", x_XInputGetState},         {"xinput1_3.dll", "#3", x_XInputSetState},
    {"xinput1_3.dll", "#4", x_XInputGetCapabilities},  {"xinput1_3.dll", "XInputGetState", x_XInputGetState},
    {"xinput1_3.dll", "XInputSetState", x_XInputSetState},
    {"xinput1_3.dll", "XInputGetCapabilities", x_XInputGetCapabilities},
};
ImportRegistrar g_xinput(kXInput);

}  // namespace

#ifdef __ANDROID__
// TouchControls: the on-screen pad's state (XInput layout: buttons mask, triggers 0..255, sticks with up positive);
// active false hands slot 0 back to "not connected".
extern "C" JNIEXPORT void JNICALL Java_com_protore_game_TouchControls_nativePad(JNIEnv* env, jclass cls,
                                                                               jboolean active, jint buttons,
                                                                               jint lt, jint rt, jint lx, jint ly,
                                                                               jint rx, jint ry) {
    g_touch.buttons = uint16_t(buttons);
    g_touch.lt = uint8_t(lt);
    g_touch.rt = uint8_t(rt);
    g_touch.lx = int16_t(lx);
    g_touch.ly = int16_t(ly);
    g_touch.rx = int16_t(rx);
    g_touch.ry = int16_t(ry);
    g_touch.active = active != 0;
    if (!g_controls) g_controls = static_cast<jclass>(env->NewGlobalRef(cls));
}
#endif

}  // namespace native
