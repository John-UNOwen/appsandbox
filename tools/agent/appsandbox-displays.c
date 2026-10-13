/*
 * appsandbox-displays.exe — reports the guest monitor layout, and with the "fix"
 * argument makes the AppSandbox virtual display the only active display.
 *
 * Output format (single line, what the host logs):
 *   <count>,<W>x<H>,... monitors=<adapter>;<monitor>@<x>,<y>,<W>x<H>[primary][asb]|... asb=<0|1|-1> fix=<state>
 *
 * Example:
 *   1,1920x1080 monitors=App Sandbox Virtual Display Adapter;AppSandboxVDD@0,0,1920x1080(primary)(asb) asb=1 fix=applied(0)
 *
 * Why the fix argument exists. The HCS configuration always declares a 1024x768
 * VideoMonitor (src/backend_win/hcs_vm.c) and the guest keeps that 1024x768 monitor
 * alongside the 1920x1080 AppSandbox display, with the 1024x768 one primary. Absolute
 * mouse injection is resolved against the primary monitor, so the surface the host streams
 * sits 1024 pixels away from the coordinates the guest applies. This helper activates only
 * the AppSandbox display target and leaves the others off. No device is disabled.
 *
 * Identification uses device ids, not friendly names, because EnumDisplayMonitors reports
 * the monitors as "Generic PnP Monitor" and "Generic Non-PnP Monitor". The AppSandbox
 * adapter is Root\AppSandboxVDD named "App Sandbox Virtual Display Adapter"
 * (tools/vdd/AppSandboxVDD.inf), its monitor node is DISPLAY\ASB0001, and the display
 * config target name it reports is "AppSandboxVDD" from the EDID in tools/vdd/vdd.h.
 *
 * Diagnostics go to C:\Windows\AppSandbox\displays.log. The single stdout line is the only
 * thing the host reads.
 *
 * Must be launched in the interactive session (WinSta0\Default) so that
 * EnumDisplayMonitors / QueryDisplayConfig see the real desktop.
 */

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

#define ASB_MAX_MONITORS 8

typedef struct {
    wchar_t device[32];            /* \\.\DISPLAYn */
    wchar_t adapter_name[128];     /* EnumDisplayDevices index 0 */
    wchar_t adapter_id[200];       /* instance id of that entry */
    wchar_t monitor_name[128];     /* EnumDisplayDevices index 1 */
    wchar_t monitor_id[200];       /* e.g. DISPLAY\ASB0001\{...} */
    RECT rect;
    BOOL primary;
    BOOL asb;
} MonitorEntry;

typedef struct {
    MonitorEntry entry[ASB_MAX_MONITORS];
    int count;
} MonitorList;

/* Lowercase and drop everything that is not a letter or a digit, so that
   "App Sandbox Virtual Display Adapter", "AppSandboxVDD" and
   "\\DISPLAY\ASB0001\{...}" can all be tested with one substring. */
static void normalize(const wchar_t *wide, char *out, int out_size)
{
    int o = 0;

    if (out_size > 0) out[0] = '\0';
    if (!wide) return;

    while (*wide && o < out_size - 1) {
        wchar_t c = *wide++;
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))
            out[o++] = (char)c;
    }
    out[o] = '\0';
}

static BOOL is_appsandbox_text(const char *normalized)
{
    return strstr(normalized, "appsandbox") != NULL || strstr(normalized, "asb0001") != NULL;
}

static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC hdc, LPRECT clip, LPARAM lparam)
{
    MONITORINFOEXW info;
    MonitorList *list = (MonitorList *)lparam;
    MonitorEntry *e;
    DISPLAY_DEVICEW dev;
    char probe[256];

    (void)hdc;
    (void)clip;
    if (list->count >= ASB_MAX_MONITORS) return TRUE;

    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, (LPMONITORINFO)&info)) return TRUE;

    e = &list->entry[list->count];
    e->rect = info.rcMonitor;
    e->primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    e->device[0] = e->adapter_name[0] = e->adapter_id[0] = L'\0';
    e->monitor_name[0] = e->monitor_id[0] = L'\0';
    e->asb = FALSE;
    wcsncpy_s(e->device, 32, info.szDevice, _TRUNCATE);

    /* Index 0 is the display device, index 1 the monitor attached to it. */
    ZeroMemory(&dev, sizeof(dev));
    dev.cb = sizeof(dev);
    if (EnumDisplayDevicesW(info.szDevice, 0, &dev, 0)) {
        wcsncpy_s(e->adapter_name, 128, dev.DeviceString, _TRUNCATE);
        wcsncpy_s(e->adapter_id, 200, dev.DeviceID, _TRUNCATE);
    }
    ZeroMemory(&dev, sizeof(dev));
    dev.cb = sizeof(dev);
    if (EnumDisplayDevicesW(info.szDevice, 1, &dev, 0)) {
        wcsncpy_s(e->monitor_name, 128, dev.DeviceString, _TRUNCATE);
        wcsncpy_s(e->monitor_id, 200, dev.DeviceID, _TRUNCATE);
    }
    if (!e->monitor_name[0]) wcsncpy_s(e->monitor_name, 128, e->adapter_name, _TRUNCATE);

    normalize(e->adapter_name, probe, sizeof(probe));
    if (is_appsandbox_text(probe)) e->asb = TRUE;
    normalize(e->adapter_id, probe, sizeof(probe));
    if (is_appsandbox_text(probe)) e->asb = TRUE;
    normalize(e->monitor_id, probe, sizeof(probe));
    if (is_appsandbox_text(probe)) e->asb = TRUE;
    normalize(e->monitor_name, probe, sizeof(probe));
    if (is_appsandbox_text(probe)) e->asb = TRUE;

    list->count++;
    return TRUE;
}

static void read_layout(MonitorList *list)
{
    ZeroMemory(list, sizeof(*list));
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)list);
}

static void disp_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\displays.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static void utf8_text(const wchar_t *wide, char *out, int out_size)
{
    out[0] = '\0';
    if (!wide || !wide[0]) {
        snprintf(out, (size_t)out_size, "-");
        return;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, out_size, NULL, NULL) <= 0)
        snprintf(out, (size_t)out_size, "-");
}

static void log_layout(const char *tag, const MonitorList *list)
{
    int i;

    disp_log("%s: %d monitor(s)", tag, list->count);
    for (i = 0; i < list->count; i++) {
        const MonitorEntry *e = &list->entry[i];
        char adapter[200], monitor[200], adapter_id[260], monitor_id[260];

        utf8_text(e->adapter_name, adapter, sizeof(adapter));
        utf8_text(e->monitor_name, monitor, sizeof(monitor));
        utf8_text(e->adapter_id, adapter_id, sizeof(adapter_id));
        utf8_text(e->monitor_id, monitor_id, sizeof(monitor_id));
        disp_log("  %s at %d,%d %dx%d primary=%d asb=%d device=%s",
                 monitor, e->rect.left, e->rect.top,
                 e->rect.right - e->rect.left, e->rect.bottom - e->rect.top,
                 e->primary ? 1 : 0, e->asb ? 1 : 0, e->device);
        disp_log("    adapter=%s id=%s monitor_id=%s", adapter, adapter_id, monitor_id);
    }
}

/* Names and flags of every display config target, so a refused change can be read
   from the log instead of guessed at. */
static void log_targets(UINT32 num_paths, const DISPLAYCONFIG_PATH_INFO *paths)
{
    UINT32 i;

    for (i = 0; i < num_paths; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target;
        DISPLAYCONFIG_ADAPTER_NAME adapter;
        char friendly[200], monitor_path[300], adapter_path[300];

        ZeroMemory(&target, sizeof(target));
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;

        ZeroMemory(&adapter, sizeof(adapter));
        adapter.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        adapter.header.size = sizeof(adapter);
        adapter.header.adapterId = paths[i].targetInfo.adapterId;
        adapter.header.id = paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            disp_log("  path %u: target name query failed", i);
            continue;
        }
        if (DisplayConfigGetDeviceInfo(&adapter.header) != ERROR_SUCCESS)
            adapter.adapterDevicePath[0] = L'\0';

        utf8_text(target.monitorFriendlyDeviceName, friendly, sizeof(friendly));
        utf8_text(target.monitorDevicePath, monitor_path, sizeof(monitor_path));
        utf8_text(adapter.adapterDevicePath, adapter_path, sizeof(adapter_path));

        disp_log("  path %u target=%s monitor=%s adapter=%s", i, friendly, monitor_path, adapter_path);
        disp_log("    tech=%u targetFlags=0x%x available=%d pathFlags=0x%x scaling=%d rotation=%d",
                 (unsigned)paths[i].targetInfo.outputTechnology,
                 paths[i].targetInfo.statusFlags,
                 paths[i].targetInfo.targetAvailable ? 1 : 0,
                 paths[i].flags, (int)paths[i].targetInfo.scaling, (int)paths[i].targetInfo.rotation);
    }
}

/* EnumDisplayDevices(NULL, i) lists the display adapters, which is what the input helper
   matches on ("App Sandbox Virtual Display Adapter"). Logged so a guest run proves whether
   that match can work. */
static void log_display_devices(void)
{
    DISPLAY_DEVICEW dd;
    DWORD i;

    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++, dd.cb = sizeof(dd)) {
        char name[200], dev[80], id[300];
        DWORD j;

        utf8_text(dd.DeviceString, name, sizeof(name));
        utf8_text(dd.DeviceName, dev, sizeof(dev));
        utf8_text(dd.DeviceID, id, sizeof(id));
        disp_log("  adapter %u: %s at %s flags=0x%x id=%s", i, name, dev, dd.StateFlags, id);

        for (j = 0; ; j++) {
            DISPLAY_DEVICEW mon;
            char mname[200], mid[300];

            ZeroMemory(&mon, sizeof(mon));
            mon.cb = sizeof(mon);
            if (!EnumDisplayDevicesW(dd.DeviceName, j, &mon, 0)) break;
            utf8_text(mon.DeviceString, mname, sizeof(mname));
            utf8_text(mon.DeviceID, mid, sizeof(mid));
            disp_log("    monitor %u: %s flags=0x%x id=%s", j, mname, mon.StateFlags, mid);
        }
    }
}

static BOOL adapter_id_equal(LUID a, LUID b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

static BOOL path_targets_asb(const DISPLAYCONFIG_PATH_INFO *p, LUID asb_adapter, UINT32 asb_target)
{
    return adapter_id_equal(p->targetInfo.adapterId, asb_adapter) && p->targetInfo.id == asb_target;
}

static BOOL target_name_matches(const DISPLAYCONFIG_PATH_INFO *path, char *detail, int detail_size)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME target;
    DISPLAYCONFIG_ADAPTER_NAME adapter;
    char probe[256];

    ZeroMemory(&target, sizeof(target));
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path->targetInfo.adapterId;
    target.header.id = path->targetInfo.id;

    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) return FALSE;

    normalize(target.monitorDevicePath, probe, sizeof(probe));
    if (is_appsandbox_text(probe)) {
        if (detail) snprintf(detail, (size_t)detail_size, "monitor path");
        return TRUE;
    }
    normalize(target.monitorFriendlyDeviceName, probe, sizeof(probe));
    if (is_appsandbox_text(probe)) {
        if (detail) snprintf(detail, (size_t)detail_size, "target name");
        return TRUE;
    }

    ZeroMemory(&adapter, sizeof(adapter));
    adapter.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    adapter.header.size = sizeof(adapter);
    adapter.header.adapterId = path->targetInfo.adapterId;
    adapter.header.id = path->targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&adapter.header) == ERROR_SUCCESS) {
        normalize(adapter.adapterDevicePath, probe, sizeof(probe));
        if (is_appsandbox_text(probe)) {
            if (detail) snprintf(detail, (size_t)detail_size, "adapter path");
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL find_asb_target(UINT32 num_paths, const DISPLAYCONFIG_PATH_INFO *paths,
                            LUID *adapter_out, UINT32 *target_out, char *detail_out, int detail_size)
{
    UINT32 i;

    for (i = 0; i < num_paths; i++) {
        char detail[64];

        if (!target_name_matches(&paths[i], detail, sizeof(detail))) continue;

        *adapter_out = paths[i].targetInfo.adapterId;
        *target_out = paths[i].targetInfo.id;
        snprintf(detail_out, (size_t)detail_size, "%s", detail);
        return TRUE;
    }
    return FALSE;
}

/* Prepare a path for the supplied configuration: no mode index, so the OS picks the
   target's preferred mode, and no IN_USE status, which is the OS's field to set. */
static void prepare_path(DISPLAYCONFIG_PATH_INFO *p)
{
    p->sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    p->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    p->sourceInfo.statusFlags = 0;
    p->targetInfo.statusFlags = 0;
    p->targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
    p->targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    p->targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED;
    p->targetInfo.refreshRate.Numerator = 0;
    p->targetInfo.refreshRate.Denominator = 0;
    p->flags = DISPLAYCONFIG_PATH_ACTIVE;
}

static BOOL try_config(const char *label, UINT32 num_paths, const DISPLAYCONFIG_PATH_INFO *paths,
                       UINT32 num_modes, const DISPLAYCONFIG_MODE_INFO *modes)
{
    LONG status;
    BOOL applied;

    /* Validation only means something together with the flag that says the supplied
       arrays are the configuration to use. */
    status = SetDisplayConfig(num_paths, (DISPLAYCONFIG_PATH_INFO *)paths, num_modes,
                              (DISPLAYCONFIG_MODE_INFO *)modes,
                              SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    disp_log("fix: %s validate -> %ld", label, status);

    status = SetDisplayConfig(num_paths, (DISPLAYCONFIG_PATH_INFO *)paths, num_modes,
                              (DISPLAYCONFIG_MODE_INFO *)modes,
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                              SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    disp_log("fix: %s apply -> %ld", label, status);
    applied = (status == ERROR_SUCCESS);
    return applied;
}

/* Tries the documented shapes in order: only the AppSandbox path with no mode array,
   then every path with the others cleared, then every path keeping the queried modes. */
static void apply_display_fix(char *state_out, int state_size)
{
    UINT32 num_paths = 0, num_modes = 0, i, n;
    DISPLAYCONFIG_PATH_INFO *queried = NULL;
    DISPLAYCONFIG_PATH_INFO *only = NULL;
    DISPLAYCONFIG_PATH_INFO *all_paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    LUID asb_adapter;
    UINT32 asb_target = 0;
    char detail[80];
    LONG status;
    BOOL done = FALSE;

    snprintf(state_out, (size_t)state_size, "failed");
    asb_adapter.HighPart = 0;
    asb_adapter.LowPart = 0;
    detail[0] = '\0';

    status = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &num_paths, &num_modes);
    if (status != ERROR_SUCCESS || num_paths == 0 || num_modes == 0) {
        disp_log("fix: GetDisplayConfigBufferSizes failed (%ld, paths=%u modes=%u)",
                 status, num_paths, num_modes);
        snprintf(state_out, (size_t)state_size, "query(%ld)", status);
        return;
    }

    queried = (DISPLAYCONFIG_PATH_INFO *)calloc(num_paths, sizeof(DISPLAYCONFIG_PATH_INFO));
    only = (DISPLAYCONFIG_PATH_INFO *)calloc(num_paths, sizeof(DISPLAYCONFIG_PATH_INFO));
    all_paths = (DISPLAYCONFIG_PATH_INFO *)calloc(num_paths, sizeof(DISPLAYCONFIG_PATH_INFO));
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(num_modes, sizeof(DISPLAYCONFIG_MODE_INFO));
    if (!queried || !only || !all_paths || !modes) {
        disp_log("fix: out of memory");
        free(queried); free(only); free(all_paths); free(modes);
        snprintf(state_out, (size_t)state_size, "nomem");
        return;
    }

    status = QueryDisplayConfig(QDC_ALL_PATHS, &num_paths, queried, &num_modes, modes, NULL);
    if (status != ERROR_SUCCESS) {
        disp_log("fix: QueryDisplayConfig failed (%ld)", status);
        free(queried); free(only); free(all_paths); free(modes);
        snprintf(state_out, (size_t)state_size, "query(%ld)", status);
        return;
    }
    disp_log("fix: %u display path(s), %u mode(s)", num_paths, num_modes);
    log_targets(num_paths, queried);

    if (!find_asb_target(num_paths, queried, &asb_adapter, &asb_target, detail, sizeof(detail))) {
        disp_log("fix: no AppSandbox display target among %u paths", num_paths);
        free(queried); free(only); free(all_paths); free(modes);
        snprintf(state_out, (size_t)state_size, "no-target");
        return;
    }
    disp_log("fix: keeping the target matched by %s", detail);

    /* Shape 1: every path, AppSandbox first and active, the others cleared, no mode array.
       Supplying the inactive paths is how a monitor is switched off. */
    {
        UINT32 write = 0;

        for (i = 0; i < num_paths; i++) {
            if (path_targets_asb(&queried[i], asb_adapter, asb_target)) {
                all_paths[write] = queried[i];
                prepare_path(&all_paths[write]);
                write++;
            }
        }
        for (i = 0; i < num_paths; i++) {
            if (path_targets_asb(&queried[i], asb_adapter, asb_target)) continue;
            all_paths[write] = queried[i];
            all_paths[write].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            all_paths[write].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            all_paths[write].sourceInfo.statusFlags = 0;
            all_paths[write].targetInfo.statusFlags = 0;
            all_paths[write].flags = 0;
            write++;
        }
        if (try_config("appsandbox first, others cleared", write, all_paths, 0, NULL)) done = TRUE;
    }

    /* Shape 2: the AppSandbox path alone, no mode array. */
    if (!done) {
        n = 0;
        for (i = 0; i < num_paths; i++) {
            if (!path_targets_asb(&queried[i], asb_adapter, asb_target)) continue;
            only[n] = queried[i];
            prepare_path(&only[n]);
            n++;
            break;
        }
        if (n > 0) {
            if (try_config("appsandbox path only", n, only, 0, NULL)) done = TRUE;
        }
    }

    /* Shape 3: every path keeping the modes the OS reported. */
    if (!done) {
        UINT32 write = 0;

        for (i = 0; i < num_paths; i++) {
            if (path_targets_asb(&queried[i], asb_adapter, asb_target)) {
                all_paths[write] = queried[i];
                all_paths[write].flags = DISPLAYCONFIG_PATH_ACTIVE;
                write++;
            }
        }
        for (i = 0; i < num_paths; i++) {
            if (path_targets_asb(&queried[i], asb_adapter, asb_target)) continue;
            all_paths[write] = queried[i];
            all_paths[write].flags = 0;
            all_paths[write].sourceInfo.statusFlags = 0;
            all_paths[write].targetInfo.statusFlags = 0;
            write++;
        }
        if (try_config("queried modes kept", write, all_paths, num_modes, modes)) done = TRUE;
    }

    if (done) snprintf(state_out, (size_t)state_size, "applied");
    else snprintf(state_out, (size_t)state_size, "rejected");

    free(queried);
    free(only);
    free(all_paths);
    free(modes);
}

/* Monitor class device class, same value the log reports in the monitor instance ids
   {4d36e96e-e325-11ce-bfc1-08002be10318}. */
static const GUID GUID_MONITOR_CLASS =
    { 0x4d36e96e, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

/* Last resort when the display config API refuses: disable the device node of every
   monitor that is not the AppSandbox one. CM_Disable_DevNode is the same call the agent
   already uses for the GPU devices (agent.c cycle_gpu_devices). A monitor device node is
   not the display adapter, so GPU paravirtualization keeps working. */
static int disable_foreign_monitor_devnodes(void)
{
    HDEVINFO set;
    SP_DEVINFO_DATA data;
    DWORD idx;
    int disabled = 0;

    set = SetupDiGetClassDevsW(&GUID_MONITOR_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        disp_log("fallback: SetupDiGetClassDevs failed (%ld)", GetLastError());
        return 0;
    }

    data.cbSize = sizeof(data);
    for (idx = 0; SetupDiEnumDeviceInfo(set, idx, &data); idx++) {
        wchar_t dev_id[512];
        char probe[512];
        CONFIGRET cr;

        dev_id[0] = L'\0';
        if (CM_Get_Device_IDW(data.DevInst, dev_id, sizeof(dev_id), 0) != CR_SUCCESS) continue;

        normalize(dev_id, probe, sizeof(probe));
        if (is_appsandbox_text(probe)) {
            disp_log("fallback: keeping %ls", dev_id);
            continue;
        }

        disp_log("fallback: disabling monitor %ls", dev_id);
        cr = CM_Disable_DevNode(data.DevInst, 0);
        if (cr == CR_SUCCESS) {
            disp_log("fallback: %ls disabled", dev_id);
            disabled++;
        } else {
            disp_log("fallback: CM_Disable_DevNode failed on %ls (%lu)", dev_id, cr);
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return disabled;
}

/* Stage 2. The unwanted target is hosted by a root enumerated display adapter, reported
   by the display config as \\?\ROOT#BasicDisplay#0000. If disabling the monitor is not
   enough, disable that adapter. Matched on the device id alone, so the GPU
   paravirtualization device and Root\AppSandboxVDD are never candidates. */
static int disable_basic_display_adapters(void)
{
    /* GUID_DEVCLASS_DISPLAY = {4D36E968-E325-11CE-BFC1-08002BE10318} */
    static const GUID GUID_DISPLAY_CLASS =
        { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    HDEVINFO set;
    SP_DEVINFO_DATA data;
    DWORD idx;
    int disabled = 0;

    set = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        disp_log("fallback 2: SetupDiGetClassDevs failed (%ld)", GetLastError());
        return 0;
    }

    data.cbSize = sizeof(data);
    for (idx = 0; SetupDiEnumDeviceInfo(set, idx, &data); idx++) {
        wchar_t dev_id[512];
        char probe[512];
        CONFIGRET cr;

        dev_id[0] = L'\0';
        if (CM_Get_Device_IDW(data.DevInst, dev_id, sizeof(dev_id), 0) != CR_SUCCESS) continue;

        normalize(dev_id, probe, sizeof(probe));
        if (!strstr(probe, "basicdisplay")) continue;

        disp_log("fallback 2: disabling adapter %ls", dev_id);
        cr = CM_Disable_DevNode(data.DevInst, 0);
        if (cr == CR_SUCCESS) {
            disp_log("fallback 2: %ls disabled", dev_id);
            disabled++;
        } else {
            disp_log("fallback 2: CM_Disable_DevNode failed on %ls (%lu)", dev_id, cr);
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return disabled;
}

static void append(char *buf, int size, int *off, const char *text)
{
    int n;

    if (*off >= size - 1) return;
    n = snprintf(buf + *off, (size_t)(size - *off), "%s", text);
    if (n < 0) return;
    *off += n;
    if (*off >= size - 1) *off = size - 1;
}

int main(int argc, char **argv)
{
    BOOL do_fix = (argc > 1 && _stricmp(argv[1], "fix") == 0);
    MonitorList list;
    char buf[1024];
    char state[64];
    int off = 0;
    int i;
    int asb_primary = -1;

    state[0] = '\0';

    if (do_fix) {
        read_layout(&list);
        log_layout("before", &list);
        log_display_devices();
        apply_display_fix(state, sizeof(state));
        if (strcmp(state, "applied") != 0) {
            int disabled = 0;
            BOOL any_asb = FALSE;

            /* The device level fallback only ever runs in a guest that already shows an
               AppSandbox monitor, and only against monitors that are not it. */
            for (i = 0; i < list.count; i++) {
                if (list.entry[i].asb) any_asb = TRUE;
            }
            if (any_asb) {
                int adapters = 0;
                int monitors = 0;

                /* Adapter first. Disabling only the monitor devnode leaves the Basic
                   Display Driver target active in the desktop with no monitor behind it,
                   which still occupies desktop space at 0,0. */
                adapters = disable_basic_display_adapters();
                Sleep(800);
                read_layout(&list);
                if (list.count > 1) {
                    monitors = disable_foreign_monitor_devnodes();
                    Sleep(800);
                }
                disabled = adapters + monitors;
            }
            if (disabled > 0) snprintf(state, sizeof(state), "rejected,devnodesdisabled(%d)", disabled);
        }
    }

    read_layout(&list);
    if (do_fix) log_layout("after", &list);

    for (i = 0; i < list.count; i++) {
        const MonitorEntry *e = &list.entry[i];
        char piece[64];
        snprintf(piece, sizeof(piece), "%s%dx%d", i ? "," : "",
                 e->rect.right - e->rect.left, e->rect.bottom - e->rect.top);
        append(buf, sizeof(buf), &off, piece);
    }
    printf("%d,%s", list.count, buf);

    buf[0] = '\0';
    off = 0;
    for (i = 0; i < list.count; i++) {
        const MonitorEntry *e = &list.entry[i];
        char monitor[200];
        char adapter[200];
        char piece[480];

        utf8_text(e->monitor_name, monitor, sizeof(monitor));
        utf8_text(e->adapter_name, adapter, sizeof(adapter));
        snprintf(piece, sizeof(piece), "%s%s / %s@%d,%d,%dx%d%s%s", i ? "|" : "", adapter, monitor,
                 e->rect.left, e->rect.top,
                 e->rect.right - e->rect.left, e->rect.bottom - e->rect.top,
                 e->primary ? "(primary)" : "", e->asb ? "(asb)" : "");
        append(buf, sizeof(buf), &off, piece);
        if (e->asb) asb_primary = e->primary ? 1 : 0;
    }
    printf(" monitors=%s asb=%d", buf, asb_primary);
    if (do_fix) printf(" fix=%s", state[0] ? state : "unknown");
    return 0;
}
