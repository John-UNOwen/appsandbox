/*
 * appsandbox-input.exe — Console-session input injector for AppSandbox.
 *
 * Runs in the interactive console session (Session 1+), spawned by the agent
 * service via CreateProcessAsUser. Receives InputPacket messages from the host
 * over the AppSandbox transport (asb_transport, ASB_CH_INPUT: AF_HYPERV on a
 * Windows host, ivshmem shared memory on a macOS host) and calls SendInput to
 * inject mouse/keyboard events into the active desktop.
 *
 * Logs to C:\Windows\AppSandbox\input.log (beside agent.log).
 */

#include "../transport/asb_transport.h"
#include "../../src/core/protocol.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")

typedef struct {
    BYTE scan[2][256];
    BYTE vk[2][256];
} PhysicalKeyState;

/* ---- Logging ---- */

static void input_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\input.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* Per-monitor DPI awareness keeps GetSystemMetrics in the same physical pixel
 * space as the host framebuffer coordinates normalized for SendInput. */
static BOOL initialize_dpi_awareness(void)
{
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return TRUE;
    /* A manifest or compatibility setting may already have set the process
     * default. Input is received/injected on this thread, so override it here. */
    if (SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return TRUE;
    input_log("Cannot enable per-monitor DPI awareness: %lu", GetLastError());
    return FALSE;
}

/* ---- Desktop switching ---- */

static void switch_to_input_desktop(void)
{
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (desk) {
        SetThreadDesktop(desk);
        CloseDesktop(desk);
    }
}

/* ---- Target display ---- */

/* Host frame coordinates are relative to the App Sandbox virtual display,
 * which is not always the primary monitor (the Hyper-V synthetic display can
 * still be active beside it). Return its desktop rect, else the primary's. */
static RECT target_display_rect(void)
{
    static RECT cached;
    static ULONGLONG checked;
    ULONGLONG now = GetTickCount64();
    DISPLAY_DEVICEW dd;
    DEVMODEW dm;
    DWORD i;

    if (checked && now - checked < 1000)
        return cached;
    checked = now;

    SetRect(&cached, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (IsRectEmpty(&cached))
        SetRect(&cached, 0, 0, 1920, 1080);

    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++, dd.cb = sizeof(dd)) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            !wcsstr(dd.DeviceString, L"App Sandbox Virtual Display"))
            continue;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0) &&
            dm.dmPelsWidth && dm.dmPelsHeight) {
            SetRect(&cached, dm.dmPosition.x, dm.dmPosition.y,
                    dm.dmPosition.x + (LONG)dm.dmPelsWidth,
                    dm.dmPosition.y + (LONG)dm.dmPelsHeight);
            break;
        }
    }
    return cached;
}

/* Normalized MOUSEEVENTF_VIRTUALDESK coordinate for desktop pixel p, aimed a
 * quarter pixel in so truncating or rounding denormalization both land on it. */
static LONG to_virtual_desk(LONG p, int origin, int size)
{
    if (size <= 0) return 0;
    return (LONG)(((LONGLONG)(p - origin) * 4 + 1) * 65536 / ((LONGLONG)size * 4));
}

/* ---- Input injection ---- */

static void inject_input(const InputPacket *pkt)
{
    INPUT inp;
    UINT result;
    ZeroMemory(&inp, sizeof(inp));
    switch_to_input_desktop();

    switch (pkt->type) {
    case INPUT_MOUSE_MOVE: {
        RECT r = target_display_rect();
        UINT32 max_x = (UINT32)(r.right - r.left - 1);
        UINT32 max_y = (UINT32)(r.bottom - r.top - 1);
        LONG x = r.left + (LONG)(pkt->param1 < max_x ? pkt->param1 : max_x);
        LONG y = r.top + (LONG)(pkt->param2 < max_y ? pkt->param2 : max_y);
        inp.type = INPUT_MOUSE;
        inp.mi.dx = to_virtual_desk(x, GetSystemMetrics(SM_XVIRTUALSCREEN),
                                    GetSystemMetrics(SM_CXVIRTUALSCREEN));
        inp.mi.dy = to_virtual_desk(y, GetSystemMetrics(SM_YVIRTUALSCREEN),
                                    GetSystemMetrics(SM_CYVIRTUALSCREEN));
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(MOUSE_MOVE) failed: %lu", GetLastError());
        break;
    }
    case INPUT_MOUSE_BUTTON: {
        inp.type = INPUT_MOUSE;
        switch (pkt->param1) {
        case INPUT_BTN_LEFT:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case INPUT_BTN_RIGHT:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case INPUT_BTN_MIDDLE:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        default:
            return;
        }
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(MOUSE_BUTTON btn=%u down=%u) failed: %lu",
                       pkt->param1, pkt->param2, GetLastError());
        break;
    }
    case INPUT_MOUSE_WHEEL: {
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inp.mi.mouseData = (DWORD)(INT32)pkt->param1;
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(MOUSE_WHEEL delta=%d) failed: %lu",
                       (INT32)pkt->param1, GetLastError());
        break;
    }
    case INPUT_KEY: {
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = (WORD)pkt->param1;
        inp.ki.wScan = (WORD)pkt->param2;
        inp.ki.dwFlags = 0;
        if (pkt->param3 & 1) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (pkt->param3 & 2) inp.ki.dwFlags |= KEYEVENTF_KEYUP;
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(KEY vk=0x%X scan=0x%X flags=0x%X) failed: %lu",
                       pkt->param1, pkt->param2, pkt->param3, GetLastError());
        break;
    }
    }
}

static BOOL is_function_key(UINT32 vk)
{
    return vk == VK_CANCEL || vk == VK_PAUSE || vk == VK_SNAPSHOT ||
           vk == VK_SLEEP || (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2);
}

static void inject_relative_mouse(const InputPacket *pkt)
{
    INPUT inp = {0};
    inp.type = INPUT_MOUSE;
    inp.mi.dx = (LONG)(INT32)pkt->param1;
    inp.mi.dy = (LONG)(INT32)pkt->param2;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    switch_to_input_desktop();
    SendInput(1, &inp, sizeof(inp));
}

static void inject_physical_key(const InputPacket *pkt, PhysicalKeyState *keys)
{
    INPUT inp;
    BYTE *held;
    UINT extended = (pkt->param3 & INPUT_KEY_EXTENDED) != 0;

    if (pkt->param2 > 0xFF ||
        (pkt->param3 & ~(INPUT_KEY_EXTENDED | INPUT_KEY_UP)))
        return;

    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_KEYBOARD;
    if (pkt->param2) {
        inp.ki.wScan = (WORD)pkt->param2;
        inp.ki.dwFlags = KEYEVENTF_SCANCODE;
        held = &keys->scan[extended][pkt->param2];
    } else {
        if (!is_function_key(pkt->param1))
            return;
        inp.ki.wVk = (WORD)pkt->param1;
        held = &keys->vk[extended][pkt->param1];
    }
    if (extended) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (pkt->param3 & INPUT_KEY_UP) inp.ki.dwFlags |= KEYEVENTF_KEYUP;

    switch_to_input_desktop();
    if (SendInput(1, &inp, sizeof(inp)) == 1)
        *held = (pkt->param3 & INPUT_KEY_UP) == 0;
    else
        input_log("SendInput(PHYSICAL_KEY scan=0x%X flags=0x%X) failed: %lu",
                  pkt->param2, pkt->param3, GetLastError());
}

static void release_physical_keys(PhysicalKeyState *keys)
{
    UINT kind, extended, code;

    switch_to_input_desktop();
    for (kind = 0; kind < 2; kind++) {
        for (extended = 0; extended < 2; extended++) {
            for (code = 1; code < 256; code++) {
                BYTE *held = kind ? &keys->vk[extended][code] :
                                    &keys->scan[extended][code];
                INPUT inp;
                if (!*held) continue;
                ZeroMemory(&inp, sizeof(inp));
                inp.type = INPUT_KEYBOARD;
                inp.ki.dwFlags = KEYEVENTF_KEYUP;
                if (kind)
                    inp.ki.wVk = (WORD)code;
                else {
                    inp.ki.wScan = (WORD)code;
                    inp.ki.dwFlags |= KEYEVENTF_SCANCODE;
                }
                if (extended) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                if (SendInput(1, &inp, sizeof(inp)) == 0)
                    input_log("SendInput(key release) failed: %lu", GetLastError());
                *held = 0;
            }
        }
    }
}

/* ---- Receive exactly len bytes (transport may deliver partial reads) ---- */

static int recv_full(AsbConn *c, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = asb_recv(c, (char *)buf + got, len - got);
        if (n <= 0)
            return n;   /* 0 = peer closed, <0 = error */
        got += n;
    }
    return got;
}

static int send_full(AsbConn *c, const void *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = asb_send(c, (const char *)buf + sent, len - sent);
        if (n <= 0)
            return n;
        sent += n;
    }
    return sent;
}

/* ---- Handle one host connection ---- */

static void handle_conn(AsbConn *c)
{
    InputPacket pkt;
    PhysicalKeyState keys = {0};
    UINT keyboard_version = 1;
    UINT mouse_version = 0;
    UINT pkt_count = 0;
    UINT32 ready = INPUT_READY_MAGIC;

    /* Tell the host we're ready to receive input. */
    if (asb_send(c, &ready, sizeof(ready)) != (int)sizeof(ready)) {
        input_log("Failed to send ready signal.");
        return;
    }
    input_log("Sent ready signal to host. Entering recv loop.");

    for (;;) {
        int n = recv_full(c, &pkt, (int)sizeof(pkt));
        if (n <= 0) {
            input_log("%s after %u packets.",
                       n == 0 ? "Host disconnected" : "recv error", pkt_count);
            break;
        }
        if (pkt.magic != INPUT_MAGIC) {
            input_log("Bad magic 0x%08X, skipping.", pkt.magic);
            continue;
        }
        pkt_count++;
        if (pkt_count == 1)
            input_log("First packet: type=%u p1=%u p2=%u p3=%u",
                       pkt.type, pkt.param1, pkt.param2, pkt.param3);
        if (pkt.type == INPUT_MOUSE_QUERY) {
            InputPacket reply = {INPUT_MAGIC, INPUT_MOUSE_REPLY, 0, pkt.param2, 0};
            if (pkt.param3 != 0) continue;
            if (pkt.param1 >= INPUT_MOUSE_VERSION)
                reply.param1 = INPUT_MOUSE_VERSION;
            if (send_full(c, &reply, (int)sizeof(reply)) != (int)sizeof(reply))
                break;
            mouse_version = reply.param1;
        } else if (pkt.type == INPUT_MOUSE_RELATIVE) {
            if (mouse_version == INPUT_MOUSE_VERSION && pkt.param3 == 0)
                inject_relative_mouse(&pkt);
        } else if (pkt.type == INPUT_MOUSE_POSITION_QUERY) {
            POINT point;
            InputPacket reply = {INPUT_MAGIC, INPUT_MOUSE_POSITION_REPLY,
                                 (UINT32)INT32_MIN, (UINT32)INT32_MIN, pkt.param1};
            if (mouse_version != INPUT_MOUSE_VERSION || pkt.param2 || pkt.param3) continue;
            switch_to_input_desktop();
            if (GetPhysicalCursorPos(&point)) {
                /* Frame-relative, like MOUSE_MOVE. Off the target display the
                   values fall outside the frame and the host ignores them. */
                RECT r = target_display_rect();
                reply.param1 = (UINT32)(point.x - r.left);
                reply.param2 = (UINT32)(point.y - r.top);
            }
            if (send_full(c, &reply, (int)sizeof(reply)) != (int)sizeof(reply))
                break;
        } else if (pkt.type == INPUT_KEYBOARD_QUERY) {
            InputPacket reply = {INPUT_MAGIC, INPUT_KEYBOARD_REPLY, 1, pkt.param2, 0};
            if (pkt.param3 != 0) continue;
            if (pkt.param1 >= INPUT_KEYBOARD_VERSION)
                reply.param1 = INPUT_KEYBOARD_VERSION;
            if (reply.param1 != keyboard_version)
                release_physical_keys(&keys);
            if (send_full(c, &reply, (int)sizeof(reply)) != (int)sizeof(reply)) {
                input_log("Failed to send keyboard reply.");
                break;
            }
            keyboard_version = reply.param1;
        } else if (pkt.type == INPUT_KEY_PHYSICAL) {
            if (keyboard_version >= INPUT_KEYBOARD_VERSION)
                inject_physical_key(&pkt, &keys);
        } else {
            inject_input(&pkt);
        }
    }
    release_physical_keys(&keys);
}

/* ---- Main: listen on the input channel, accept connections ---- */

int main(void)
{
    AsbListener *l;

    if (!initialize_dpi_awareness()) return 1;

    input_log("Starting (PID=%lu, session=%lu).",
              GetCurrentProcessId(),
              WTSGetActiveConsoleSessionId());

    if (asb_transport_init() != 0) {
        input_log("asb_transport_init failed.");
        return 1;
    }

    l = asb_listen(ASB_CH_INPUT);
    if (!l) {
        input_log("asb_listen(ASB_CH_INPUT) failed.");
        return 1;
    }
    input_log("Listening on input channel (transport=%s).",
              asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    /* Accept loop — one host connection at a time. */
    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        if (!c) {
            Sleep(100);
            continue;
        }
        input_log("Host connected.");
        handle_conn(c);
        asb_close(c);
    }
}
