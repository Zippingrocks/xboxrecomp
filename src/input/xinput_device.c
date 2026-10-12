/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

static void apply_keyboard_menu_controls(XBOX_INPUT_STATE *pState,
                                        int keyboard_enabled)
{
    static ULONGLONG auto_start_epoch;
    DWORD fg_pid = 0;

    /* GetAsyncKeyState is system-wide: only honour the keyboard while this
     * process owns the foreground window, so typing elsewhere (or a test run
     * in the background) never presses buttons in the game. */
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    if (!keyboard_enabled || fg_pid != GetCurrentProcessId())
        goto auto_start;

    if (GetAsyncKeyState(VK_RETURN) & 0x8000)
        pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
    if (GetAsyncKeyState(VK_UP) & 0x8000)
        pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_UP;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)
        pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)
        pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000)
        pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
    if ((GetAsyncKeyState(VK_SPACE) & 0x8000) ||
        (GetAsyncKeyState('Z') & 0x8000))
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    if (GetAsyncKeyState('X') & 0x8000)
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;

auto_start:
    /* Deterministic headless test aid.  This is disabled in normal runs and
     * periodically taps Start only when explicitly requested by the launcher. */
    /* GODZILLA_AUTO_START[=delay_seconds]: after the delay (default 0),
     * tap Start for 750 ms every 5 s. */
    if (getenv("GODZILLA_AUTO_START")) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG delay = (ULONGLONG)atoi(getenv("GODZILLA_AUTO_START")) * 1000ull;
        if (!auto_start_epoch)
            auto_start_epoch = now;
        /* GODZILLA_AUTO_START_TAPS=n limits the number of taps */
        ULONGLONG taps = getenv("GODZILLA_AUTO_START_TAPS")
            ? (ULONGLONG)atoi(getenv("GODZILLA_AUTO_START_TAPS")) : ~0ull;
        if (now - auto_start_epoch >= delay &&
            (now - auto_start_epoch - delay) / 5000 < taps &&
            ((now - auto_start_epoch - delay) % 5000) < 750)
            pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
    }
    /* GODZILLA_INPUT_SCRIPT="secs:button,secs:button,...": tap a button for
     * 300 ms at each time (seconds since first poll, fractional allowed).
     * Buttons: start back a b x y up down left right. */
    if (getenv("GODZILLA_INPUT_SCRIPT")) {
        static ULONGLONG script_epoch;
        ULONGLONG now = GetTickCount64();
        const char *p = getenv("GODZILLA_INPUT_SCRIPT");
        if (!script_epoch) script_epoch = now;
        while (*p) {
            char name[16];
            int k = 0;
            double t = strtod(p, (char **)&p);
            ULONGLONG at;
            if (*p == ':') p++;
            while (*p && *p != ',' && k < 15) name[k++] = *p++;
            name[k] = 0;
            if (*p == ',') p++;
            at = script_epoch + (ULONGLONG)(t * 1000.0);
            if (now >= at && now < at + 300) {
                if (!strcmp(name, "start")) pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
                else if (!strcmp(name, "back")) pState->Gamepad.wButtons |= XBOX_GAMEPAD_BACK;
                else if (!strcmp(name, "up")) pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_UP;
                else if (!strcmp(name, "down")) pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
                else if (!strcmp(name, "left")) pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
                else if (!strcmp(name, "right")) pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
                else if (!strcmp(name, "a")) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
                else if (!strcmp(name, "b")) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
                else if (!strcmp(name, "x")) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
                else if (!strcmp(name, "y")) pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
            }
            if (!k) break;
        }
    }
}

static void trace_menu_input(const XBOX_INPUT_STATE *pState)
{
    static WORD last_buttons;
    static BYTE last_analog[8];
    const XBOX_GAMEPAD *pad = &pState->Gamepad;

    if (!getenv("GODZILLA_INPUT_TRACE"))
        return;
    if (pad->wButtons == last_buttons &&
        memcmp(pad->bAnalogButtons, last_analog, sizeof(last_analog)) == 0)
        return;

    fprintf(stderr,
        "[INPUT] packet=%lu digital=%04X analog=A:%u B:%u X:%u Y:%u\n",
        (unsigned long)pState->dwPacketNumber, pad->wButtons,
        pad->bAnalogButtons[XBOX_BUTTON_A],
        pad->bAnalogButtons[XBOX_BUTTON_B],
        pad->bAnalogButtons[XBOX_BUTTON_X],
        pad->bAnalogButtons[XBOX_BUTTON_Y]);
    last_buttons = pad->wButtons;
    memcpy(last_analog, pad->bAnalogButtons, sizeof(last_analog));
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

static DWORD input_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState,
                             int keyboard_enabled)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* Test-only deterministic replay mode.  It deliberately ignores the host
     * controller so a scripted callback test cannot be contaminated by stick
     * drift or buttons pressed in the visible game window. */
    if (dwPort == 0 && getenv("GODZILLA_INPUT_SCRIPT_ONLY")) {
        memset(pState, 0, sizeof(XBOX_INPUT_STATE));
        pState->dwPacketNumber = ++g_last_packet[dwPort];
        apply_keyboard_menu_controls(pState, keyboard_enabled);
        trace_menu_input(pState);
        g_controller_connected[dwPort] = TRUE;
        return ERROR_SUCCESS;
    }

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        /* Keep port 0 usable on keyboard-only hosts.  Save the Earth's shell
         * waits at controller selection when no pad is present, so expose a
         * neutral virtual pad and map the minimum menu controls. */
        if (dwPort != 0)
            return result;

        memset(pState, 0, sizeof(XBOX_INPUT_STATE));
        pState->dwPacketNumber = ++g_last_packet[dwPort];
        apply_keyboard_menu_controls(pState, keyboard_enabled);
        trace_menu_input(pState);
        g_controller_connected[dwPort] = TRUE;
        return ERROR_SUCCESS;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;
    apply_keyboard_menu_controls(pState, keyboard_enabled);
    trace_menu_input(pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return input_get_state(dwPort, pState, 1);
}

DWORD xbox_InputGetStateRaw(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return input_get_state(dwPort, pState, 0);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    if (dwPort == 0) return TRUE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) {
        if (dwPort != 0) return result;
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        return ERROR_SUCCESS;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

/* The SDL backend currently has no keyboard overlay. */
DWORD xbox_InputGetStateRaw(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return xbox_InputGetState(dwPort, pState);
}

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
