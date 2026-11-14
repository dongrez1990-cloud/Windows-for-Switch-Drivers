/* SPDX-License-Identifier: MIT
 * Read-only native ARM64 dashboard. No clock, voltage, fan or display writes.
 */
#define UNICODE 1
#define _UNICODE 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include "../../drivers/SwitchFan/src/power_ioctl.h"

#define CLASS_NAME L"SwitchWOA.Telemetry"
#define UPDATE_MESSAGE (WM_APP + 1)
#define FAN_KEY L"SYSTEM\\CurrentControlSet\\Services\\switchfan\\Parameters"
#define GPU_KEY L"SYSTEM\\CurrentControlSet\\Services\\tegragpurender\\Parameters"

typedef struct {
    DWORD cpu, gpu, ram, fan, cpu_mv, gpu_uv, temp, gpu_age;
    BOOL cpu_ok, gpu_ok, ram_ok, fan_ok, temp_ok, live_bridge, gpu_live, fan_fresh;
    DWORD error;
    SYSTEMTIME time;
} SAMPLE;

static SAMPLE sample;
static CRITICAL_SECTION sample_lock;
static HANDLE stop_event, worker;
static HFONT title_font, value_font, label_font, detail_font;

static BOOL ReadDword(HKEY key, LPCWSTR name, DWORD *value)
{
    DWORD size = sizeof(*value), type = 0;
    return key && RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(*value);
}

static SAMPLE ReadSample(void)
{
    SAMPLE s = {0};
    HKEY key = NULL;
    HANDLE device;
    DWORD bytes = 0, buffer[256] = {0};
    SWFAN_POWER_STATUS *power = (SWFAN_POWER_STATUS *)buffer;
    device = CreateFileW(L"\\\\.\\SwitchBacklight", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device != INVALID_HANDLE_VALUE) {
        BOOL ok = DeviceIoControl(device, IOCTL_SWFAN_GET_POWER_STATUS, NULL, 0,
                                  buffer, sizeof(buffer), &bytes, NULL);
        s.error = ok ? 0 : GetLastError();
        if (ok && bytes >= 19u * sizeof(DWORD) && (power->Version == 1u || power->Version == 2u)) {
            s.live_bridge = TRUE;
            s.cpu = power->CpuActualKhz; s.cpu_ok = s.cpu != 0;
            s.gpu = power->GpuReportedKhz; s.gpu_ok = s.gpu != 0;
            s.gpu_age = power->Version == 2u && bytes >= sizeof(*power) ? power->GpuSampleAgeMs : ~0u;
            s.gpu_live = s.gpu_ok && s.gpu_age <= 2000u && (power->Flags & SWFAN_POWER_FLAG_LIVE_GPU_COUNTER);
            s.ram = power->RamActualKhz; s.ram_ok = s.ram != 0;
            s.fan = power->FanDuty; s.fan_ok = s.fan <= 236u;
            s.cpu_mv = power->CpuMv; s.gpu_uv = power->GpuReportedUv;
            s.temp = power->TempC; s.temp_ok = power->TempValid != 0;
            s.fan_fresh = TRUE;
        }
        CloseHandle(device);
    } else s.error = GetLastError();

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FAN_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        FILETIME write_time, now;
        ULARGE_INTEGER a, b;
        if (!s.live_bridge) {
            s.cpu_ok = ReadDword(key, L"CpuActualKhz", &s.cpu) && s.cpu != 0;
            s.ram_ok = ReadDword(key, L"RamActualKhz", &s.ram) && s.ram != 0;
            s.fan_ok = ReadDword(key, L"Duty", &s.fan) && s.fan <= 236u;
            (void)ReadDword(key, L"CpuMv", &s.cpu_mv);
            s.temp_ok = ReadDword(key, L"TempSoC", &s.temp);
            if (RegQueryInfoKeyW(key, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &write_time) == ERROR_SUCCESS) {
                GetSystemTimeAsFileTime(&now);
                a.LowPart = now.dwLowDateTime; a.HighPart = now.dwHighDateTime;
                b.LowPart = write_time.dwLowDateTime; b.HighPart = write_time.dwHighDateTime;
                s.fan_fresh = a.QuadPart >= b.QuadPart && a.QuadPart - b.QuadPart <= 100000000ULL;
            }
        }
        RegCloseKey(key);
    }
    if (!s.gpu_ok && !s.live_bridge && RegOpenKeyExW(HKEY_LOCAL_MACHINE, GPU_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        s.gpu_ok = ReadDword(key, L"BootGpuMeasuredKhz", &s.gpu) && s.gpu != 0;
        (void)ReadDword(key, L"BootGpuRailUv", &s.gpu_uv);
        RegCloseKey(key);
    }
    GetLocalTime(&s.time);
    return s;
}

static DWORD WINAPI PollThread(void *context)
{
    HWND window = context;
    do {
        SAMPLE next = ReadSample();
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) break;
        EnterCriticalSection(&sample_lock); sample = next; LeaveCriticalSection(&sample_lock);
        PostMessageW(window, UPDATE_MESSAGE, 0, 0);
    } while (WaitForSingleObject(stop_event, 1000) == WAIT_TIMEOUT);
    return 0;
}

static void Text(HDC dc, HFONT font, COLORREF color, RECT rect, LPCWSTR value, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void Frequency(WCHAR *out, size_t count, BOOL valid, DWORD khz)
{
    if (valid) _snwprintf(out, count, L"%lu.%lu MHz", khz / 1000, (khz % 1000) / 100);
    else _snwprintf(out, count, L"未提供");
    out[count - 1] = 0;
}

static void Card(HDC dc, RECT r, LPCWSTR name, LPCWSTR value, LPCWSTR detail, COLORREF accent)
{
    HBRUSH brush = CreateSolidBrush(RGB(28, 39, 57));
    FillRect(dc, &r, brush); DeleteObject(brush);
    RECT stripe = {r.left, r.top, r.left + 4, r.bottom};
    brush = CreateSolidBrush(accent); FillRect(dc, &stripe, brush); DeleteObject(brush);
    RECT a = {r.left + 18, r.top + 14, r.right - 14, r.top + 42};
    Text(dc, label_font, accent, a, name, DT_LEFT | DT_SINGLELINE);
    a.top = r.top + 52; a.bottom = r.top + 98;
    Text(dc, value_font, RGB(244, 247, 253), a, value, DT_LEFT | DT_SINGLELINE);
    a.top = r.top + 108; a.bottom = r.bottom - 10;
    Text(dc, detail_font, RGB(176, 190, 209), a, detail, DT_LEFT | DT_WORDBREAK);
}

static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps), memory = CreateCompatibleDC(dc);
    RECT client; GetClientRect(hwnd, &client);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    HBRUSH background = CreateSolidBrush(RGB(15, 23, 38));
    FillRect(memory, &client, background); DeleteObject(background);
    SetBkMode(memory, TRANSPARENT);
    SAMPLE s; EnterCriticalSection(&sample_lock); s = sample; LeaveCriticalSection(&sample_lock);
    RECT r = {24, 20, client.right - 24, 58};
    Text(memory, title_font, RGB(244, 247, 253), r, L"Switch 硬件监控", DT_LEFT | DT_SINGLELINE);
    r.top = 61; r.bottom = 84;
    Text(memory, detail_font, RGB(176, 190, 209), r, L"每秒刷新 · 驱动读回 · 仅查看", DT_LEFT | DT_SINGLELINE);
    int width = (client.right - 62) / 2, height = (client.bottom - 148) / 2;
    WCHAR value[96], detail[220];
    RECT cards[4] = {{24,98,24+width,98+height}, {38+width,98,client.right-24,98+height},
                    {24,112+height,24+width,112+2*height}, {38+width,112+height,client.right-24,112+2*height}};
    Frequency(value, 96, s.cpu_ok, s.cpu);
    _snwprintf(detail, 220, L"%s\n%lu mV%s", s.live_bridge ? L"CPU 寄存器 / 驱动读回" :
        (s.fan_fresh ? L"驱动定期更新" : L"驱动缓存未更新"), s.cpu_mv, s.cpu_mv ? L"" : L"（未提供电压）");
    Card(memory, cards[0], L"CPU", value, detail, RGB(89, 173, 255));
    Frequency(value, 96, s.gpu_ok, s.gpu);
    _snwprintf(detail, 220, L"%s\n%lu mV", s.gpu_live ? L"GPCCLK 硬件计数器 · 2秒内采样" :
        (s.gpu_ok ? (s.live_bridge ? L"KMD 上次采样 · 已过期" : L"启动时硬件计数器 · 非实时") :
         L"等待 GPU 驱动读数"), s.gpu_uv / 1000u);
    Card(memory, cards[1], L"GPU", value, detail, RGB(77, 214, 170));
    Frequency(value, 96, s.ram_ok, s.ram);
    _snwprintf(detail, 220, L"EMC 运行频率\n%s", s.ram_ok ? L"固件交接读回；当前 RAM 不动态调频" : L"固件未提供有效实际频率");
    Card(memory, cards[2], L"RAM", value, detail, RGB(177, 148, 255));
    if (s.fan_ok) _snwprintf(value, 96, L"PWM %lu%%", (s.fan * 100u + 118u) / 236u);
    else _snwprintf(value, 96, L"未提供");
    if (s.temp_ok) _snwprintf(detail, 220, L"SoC %lu °C\n转速 RPM：驱动未提供反馈", s.temp);
    else _snwprintf(detail, 220, L"温度未提供\n转速 RPM：驱动未提供反馈");
    Card(memory, cards[3], L"FAN", value, detail, RGB(255, 185, 96));
    _snwprintf(detail, 220, L"更新 %02u:%02u:%02u  |  %s", s.time.wHour, s.time.wMinute, s.time.wSecond,
        s.live_bridge ? L"实时查询接口" : L"兼容当前驱动 · GPU 仅启动读数");
    r = (RECT){24,client.bottom-29,client.right-24,client.bottom-6};
    Text(memory, detail_font, RGB(151, 167, 190), r, detail, DT_LEFT | DT_SINGLELINE);
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        worker = CreateThread(NULL, 0, PollThread, hwnd, 0, NULL);
        return worker ? 0 : -1;
    case UPDATE_MESSAGE: case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lparam)->ptMinTrackSize = (POINT){620,530}; return 0;
    case WM_DESTROY: SetEvent(stop_event); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE old_instance, PWSTR command, int show)
{
    (void)old_instance;
    if (command && wcsstr(command, L"--snapshot")) {
        SAMPLE s = ReadSample();
        printf("{\"cpu_khz\":%lu,\"gpu_khz\":%lu,\"ram_khz\":%lu,\"fan_pwm\":%lu,"
               "\"fan_pwm_max\":236,\"cpu_mv\":%lu,\"gpu_uv\":%lu,\"temp_c\":%lu,"
               "\"cpu_valid\":%d,\"gpu_valid\":%d,\"ram_valid\":%d,\"fan_valid\":%d,"
               "\"live_bridge\":%d,\"gpu_live\":%d,\"driver_cache_fresh\":%d,\"win32_error\":%lu}\n",
               s.cpu,s.gpu,s.ram,s.fan,s.cpu_mv,s.gpu_uv,s.temp,
               s.cpu_ok,s.gpu_ok,s.ram_ok,s.fan_ok,s.live_bridge,s.gpu_live,s.fan_fresh,s.error);
        return s.cpu_ok || s.gpu_ok || s.ram_ok || s.fan_ok ? 0 : 3;
    }
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\SwitchWOA.Telemetry");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(CLASS_NAME, NULL);
        if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        CloseHandle(mutex); return 0;
    }
    SetProcessDPIAware();
    InitializeCriticalSection(&sample_lock);
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop_event) return 1;
    title_font = CreateFontW(-26,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    value_font = CreateFontW(-32,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    label_font = CreateFontW(-18,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    detail_font = CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WindowProc; wc.hInstance = instance; wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) return 1;
    HWND window = CreateWindowExW(0, CLASS_NAME, L"Switch 硬件监控", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 600, NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (worker && WaitForSingleObject(worker, 3000) != WAIT_OBJECT_0) ExitProcess(0);
    if (worker) CloseHandle(worker);
    CloseHandle(stop_event); if (mutex) CloseHandle(mutex);
    DeleteObject(title_font); DeleteObject(value_font); DeleteObject(label_font); DeleteObject(detail_font);
    DeleteCriticalSection(&sample_lock);
    return 0;
}
