// BurnInGuard - 防烧屏守护 (native Win32, single exe)
// Copyright (C) 2026 Liuli-Lumine
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cwchar>

#define WM_TRAY        (WM_APP + 1)
#define TIMER_TICK     1
#define TIMER_ANIM     2
#define TIMER_ESC      3

#define IDM_OPEN       5001
#define IDM_EXIT       5002
#define IDM_ENABLE     5003

#define IDC_LIST       1001
#define IDC_PREVIEW    1002
#define IDC_AUTO       1003
#define IDC_TOGGLE     1004
#define IDC_STATUS     1005
#define IDC_APPLY      1006
#define IDC_FSGUARD    1007
#define IDC_EDIT0      1100
#define IDC_UD0        1110
#define IDC_LBL0       1120
#define IDC_HNT0       1130
#define IDC_TESTHINT   1140
#define IDC_TITLE      1141

enum Phase { PH_HIDDEN = 0, PH_DIMIN, PH_HOLD, PH_FADEBLACK, PH_BLACK, PH_FADEOUT };

struct MonInfo
{
    std::wstring device;
    std::wstring name;
    RECT rect;
    bool primary;
    HMONITOR hmon;

    std::wstring Label() const
    {
        wchar_t buf[512];
        swprintf(buf, 512, L"%ls   %ls   %ldx%ld   (%ld, %ld)%ls",
                 device.c_str(), name.c_str(),
                 rect.right - rect.left, rect.bottom - rect.top,
                 rect.left, rect.top, primary ? L"  主屏" : L"");
        return buf;
    }
};

struct OverlayCtx
{
    bool  preview;
    bool  placed;
    RECT  rect;
};

struct Target
{
    MonInfo mon;
    HWND overlay;
    double opacity;
    Phase phase;
    DWORD phaseStart;
    DWORD lastSeen;
    bool fs;
    DWORD fsNext;
};

// ---------- globals ----------
static HINSTANCE g_inst;
static HWND      g_msgWnd;
static HWND      g_settingsWnd;
static HICON     g_trayIcon;
static UINT      g_taskbarCreated;
static HFONT     g_font;

static int g_idleSec     = 300;
static int g_wakeMs      = 500;
static int g_dimFadeMs   = 500;
static int g_dimOpacity  = 50;
static int g_dimHoldSec  = 10;
static int g_blackFadeMs = 500;

static std::vector<MonInfo>      g_mons;
static std::vector<std::wstring> g_selected;
static std::vector<Target*>      g_targets;
static std::vector<Target*>      g_preview;

static bool  g_previewing = false;
static bool  g_inSettings = false;
static bool  g_animRunning = false;
static bool  g_enabled = true;   // 总开关：不持久化，每次启动都是开启
static bool  g_fsGuard = true;   // 全屏程序不调暗：持久化
static DWORD g_lastEnum = 0;
static DWORD g_previewBlackAt = 0;
static bool  g_previewEnding = false;

static std::wstring g_dir;

// ---------- helpers ----------
static void Log(const wchar_t* fmt, ...)
{
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[1200];
    swprintf(line, 1200, L"%02d:%02d:%02d.%03d  %ls\r\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    char utf8[2400];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n <= 1) return;

    std::wstring path = g_dir + L"\\burninguard.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, utf8, (DWORD)(n - 1), &written, nullptr);
    CloseHandle(h);
}

static double DimLevel() { return g_dimOpacity / 100.0; }

static bool Contains(const RECT& r, POINT p)
{
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L'\\');
    return k == std::wstring::npos ? std::wstring(L".") : s.substr(0, k);
}

// ---------- icon ----------
static HICON MakeTrayIcon()
{
    const int S = 32;
    BITMAPV5HEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);

    DWORD* px = (DWORD*)bits;
    for (int i = 0; i < S * S; i++) px[i] = 0;

    struct Px { int x, y; BYTE r, g, b; };
    auto fill = [&](int x0, int y0, int x1, int y1, BYTE r, BYTE g, BYTE b)
    {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
            {
                if (x < 0 || y < 0 || x >= S || y >= S) continue;
                px[y * S + x] = ((DWORD)255 << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
            }
    };

    fill(2, 4, 29, 22, 238, 240, 243);      // body
    fill(2, 4, 29, 4, 64, 68, 74);          // top border
    fill(2, 22, 29, 22, 64, 68, 74);        // bottom border
    fill(2, 4, 2, 22, 64, 68, 74);          // left border
    fill(29, 4, 29, 22, 64, 68, 74);        // right border
    fill(4, 6, 27, 18, 22, 32, 46);         // screen
    fill(5, 8, 26, 9, 0, 200, 130);         // green bar
    fill(14, 23, 17, 25, 238, 240, 243);    // stand
    fill(7, 26, 24, 28, 238, 240, 243);     // base
    fill(7, 26, 24, 26, 64, 68, 74);
    fill(7, 28, 24, 28, 64, 68, 74);

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

// ---------- monitors ----------
static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, (MONITORINFO*)&mi)) return TRUE;

    MonInfo m;
    m.device = mi.szDevice;
    m.rect = mi.rcMonitor;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.hmon = hMon;

    DISPLAY_DEVICEW dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) m.name = dd.DeviceString;

    g_mons.push_back(m);
    return TRUE;
}

static void EnumMons()
{
    g_mons.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, 0);
}

static bool IsSelected(const std::wstring& dev)
{
    for (size_t i = 0; i < g_selected.size(); i++)
        if (_wcsicmp(g_selected[i].c_str(), dev.c_str()) == 0) return true;
    return false;
}

// ---------- overlay ----------
static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    OverlayCtx* ctx = (OverlayCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (ctx && ctx->preview)
        {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(115, 115, 115));
            HFONT old = (HFONT)SelectObject(dc, g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc, L"预览中 · 按 ESC 立刻退出", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void RegisterOverlayClass()
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BurnInGuardOverlay";
    RegisterClassExW(&wc);
}

static HWND CreateOverlay(bool preview)
{
    HWND h = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"BurnInGuardOverlay", L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, g_inst, nullptr);
    if (!h) return nullptr;

    OverlayCtx* ctx = new OverlayCtx();
    ctx->preview = preview;
    ctx->placed = false;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)ctx);
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    return h;
}

static void FreeOverlay(HWND h)
{
    if (!h) return;
    OverlayCtx* ctx = (OverlayCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
    ShowWindow(h, SW_HIDE);
    DestroyWindow(h);
    delete ctx;
}

static void OverlayPlace(HWND h, const RECT& r)
{
    OverlayCtx* ctx = (OverlayCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (ctx && ctx->placed && memcmp(&ctx->rect, &r, sizeof(RECT)) == 0) return;
    if (ctx) { ctx->rect = r; ctx->placed = true; }
    SetWindowPos(h, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void OverlayAlpha(HWND h, double opacity)
{
    int a = (int)(opacity * 255.0 + 0.5);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    SetLayeredWindowAttributes(h, 0, (BYTE)a, LWA_ALPHA);
}

// ---------- phase engine ----------
static void EnsureAnim();
static void EndPreview(bool abort);

static void StartBlank(Target* t)
{
    t->opacity = 0.0;
    OverlayPlace(t->overlay, t->mon.rect);
    OverlayAlpha(t->overlay, 0.0);
    t->phase = PH_DIMIN;
    t->phaseStart = GetTickCount();
    EnsureAnim();
    Log(L"dim-in %ls", t->mon.device.c_str());
}

static void Wake(Target* t)
{
    if (t->phase == PH_HIDDEN) return;
    t->phase = PH_FADEOUT;
    t->phaseStart = GetTickCount();
    EnsureAnim();
    Log(L"wake %ls from %.2f", t->mon.device.c_str(), t->opacity);
}

static void HideIt(Target* t)
{
    ShowWindow(t->overlay, SW_HIDE);
    OverlayCtx* ctx = (OverlayCtx*)GetWindowLongPtrW(t->overlay, GWLP_USERDATA);
    if (ctx) ctx->placed = false;
    t->opacity = 0.0;
    t->phase = PH_HIDDEN;
    Log(L"hidden %ls", t->mon.device.c_str());
}

static void StepTarget(Target* t, DWORD now)
{
    double k;
    switch (t->phase)
    {
    case PH_DIMIN:
        if (g_dimFadeMs <= 0)
        {
            t->opacity = DimLevel();
            OverlayAlpha(t->overlay, t->opacity);
            t->phase = PH_HOLD;
            t->phaseStart = now;
            Log(L"hold %ls", t->mon.device.c_str());
            break;
        }
        k = (now - t->phaseStart) / (double)g_dimFadeMs;
        if (k >= 1.0)
        {
            t->opacity = DimLevel();
            OverlayAlpha(t->overlay, t->opacity);
            t->phase = PH_HOLD;
            t->phaseStart = now;
            Log(L"hold %ls", t->mon.device.c_str());
        }
        else
        {
            t->opacity = DimLevel() * k;
            OverlayAlpha(t->overlay, t->opacity);
        }
        break;

    case PH_HOLD:
        if ((now - t->phaseStart) >= (DWORD)(g_dimHoldSec * 1000))
        {
            t->phase = PH_FADEBLACK;
            t->phaseStart = now;
            Log(L"fade-to-black %ls", t->mon.device.c_str());
        }
        break;

    case PH_FADEBLACK:
        if (g_blackFadeMs <= 0)
        {
            t->opacity = 1.0;
            OverlayAlpha(t->overlay, 1.0);
            t->phase = PH_BLACK;
            t->phaseStart = now;
            Log(L"black %ls", t->mon.device.c_str());
            break;
        }
        k = (now - t->phaseStart) / (double)g_blackFadeMs;
        if (k >= 1.0)
        {
            t->opacity = 1.0;
            OverlayAlpha(t->overlay, 1.0);
            t->phase = PH_BLACK;
            t->phaseStart = now;
            Log(L"black %ls", t->mon.device.c_str());
        }
        else
        {
            t->opacity = DimLevel() + (1.0 - DimLevel()) * k;
            OverlayAlpha(t->overlay, t->opacity);
        }
        break;

    case PH_FADEOUT:
        if (g_wakeMs <= 0) { HideIt(t); break; }
        k = (now - t->phaseStart) / (double)g_wakeMs;
        if (k >= 1.0) { HideIt(t); break; }
        OverlayAlpha(t->overlay, t->opacity * (1.0 - k));
        break;
    }
}

static bool AnyAnimating()
{
    for (size_t i = 0; i < g_targets.size(); i++)
    {
        Phase p = g_targets[i]->phase;
        if (p == PH_DIMIN || p == PH_HOLD || p == PH_FADEBLACK || p == PH_FADEOUT) return true;
    }
    for (size_t i = 0; i < g_preview.size(); i++)
    {
        Phase p = g_preview[i]->phase;
        if (p == PH_DIMIN || p == PH_HOLD || p == PH_FADEBLACK || p == PH_FADEOUT) return true;
    }
    return false;
}

static void EnsureAnim()
{
    bool need = AnyAnimating();
    if (need && !g_animRunning)
    {
        SetTimer(g_msgWnd, TIMER_ANIM, 15, nullptr);
        g_animRunning = true;
    }
    else if (!need && g_animRunning)
    {
        KillTimer(g_msgWnd, TIMER_ANIM);
        g_animRunning = false;
    }
}

static void ResetIdle()
{
    DWORD now = GetTickCount();
    for (size_t i = 0; i < g_targets.size(); i++) g_targets[i]->lastSeen = now;
}

// ---------- fullscreen detection ----------
// 有程序在这块屏上做全屏 / 独占全屏播放时，永不变暗。
static int QueryUserNotificationState()
{
    typedef HRESULT (WINAPI *PFN)(int*);
    static PFN fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        HMODULE h = GetModuleHandleW(L"shell32.dll");
        if (!h) h = LoadLibraryW(L"shell32.dll");
        if (h) fn = (PFN)GetProcAddress(h, "SHQueryUserNotificationState");
    }
    if (!fn) return -1;
    int st = -1;
    if (SUCCEEDED(fn(&st))) return st;
    return -1;
}

static bool GetFrameBounds(HWND h, RECT* r)
{
    typedef HRESULT (WINAPI *PFN)(HWND, DWORD, PVOID, DWORD);
    static PFN fn = NULL;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        HMODULE d = LoadLibraryW(L"dwmapi.dll");
        if (d) fn = (PFN)GetProcAddress(d, "DwmGetWindowAttribute");
    }
    const DWORD DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    if (fn && SUCCEEDED(fn(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(RECT)))) return true;
    return GetWindowRect(h, r) != 0;
}

static bool IsIgnorableWindow(HWND h)
{
    if (h == g_msgWnd || h == g_settingsWnd) return true;

    wchar_t cls[128];
    if (!GetClassNameW(h, cls, 128)) return true;
    if (!_wcsicmp(cls, L"BurnInGuardOverlay")) return true;
    if (!_wcsicmp(cls, L"Progman") || !_wcsicmp(cls, L"WorkerW")) return true;
    if (!_wcsicmp(cls, L"Shell_TrayWnd") || !_wcsicmp(cls, L"Shell_SecondaryTrayWnd")) return true;
    return false;
}

static bool CoversMonitor(HWND h, const RECT& mon)
{
    RECT r;
    if (!GetFrameBounds(h, &r)) return false;

    RECT in;
    if (!IntersectRect(&in, &r, &mon)) return false;

    double monArea = (double)(mon.right - mon.left) * (double)(mon.bottom - mon.top);
    if (monArea <= 0) return false;
    double cover = (double)(in.right - in.left) * (double)(in.bottom - in.top) / monArea;
    return cover >= 0.95;
}

static bool FullscreenOn(HMONITOR hmon, const RECT& mon)
{
    // 1) 按 Z 序扫描最上层的窗口：覆盖整块屏、且没有标题栏。
    //    不要求它在前台——VRChat 这类无边框全屏经常被别的窗口抢走焦点。
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(h) || IsIconic(h)) continue;
        if (IsIgnorableWindow(h)) continue;

        LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
        if (style & WS_CAPTION) continue;      // 有标题栏 = 普通窗口（含"最大化"），不算全屏

        if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != hmon) continue;
        if (CoversMonitor(h, mon)) return true;
    }

    // 2) 独占全屏（D3D flip）：窗口几何可能不可靠，用系统状态兜底
    HWND fg = GetForegroundWindow();
    if (fg && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == hmon &&
        QueryUserNotificationState() == 3)          // QUNS_RUNNING_D3D_FULL_SCREEN
        return true;

    return false;
}

// ---------- master switch ----------
static void RefreshEnabledUi()
{
    if (!g_settingsWnd) return;
    SetDlgItemTextW(g_settingsWnd, IDC_TOGGLE, g_enabled ? L"关闭防烧屏" : L"开启防烧屏");
    SetDlgItemTextW(g_settingsWnd, IDC_STATUS, g_enabled ? L"当前状态：已开启" : L"当前状态：已关闭");
    HWND st = GetDlgItem(g_settingsWnd, IDC_STATUS);
    if (st) InvalidateRect(st, nullptr, TRUE);
}

static void ToggleEnabled()
{
    g_enabled = !g_enabled;
    Log(L"enabled=%d", g_enabled ? 1 : 0);
    if (!g_enabled)
    {
        // 关掉时立刻把已经变暗的屏幕恢复
        for (size_t i = 0; i < g_targets.size(); i++)
            if (g_targets[i]->phase != PH_HIDDEN && g_targets[i]->phase != PH_FADEOUT)
                Wake(g_targets[i]);
    }
    ResetIdle();
    RefreshEnabledUi();
}

// ---------- targets ----------
static void RefreshTargets(bool force)
{
    DWORD now = GetTickCount();
    if (!force && (now - g_lastEnum) < 5000) return;
    g_lastEnum = now;

    EnumMons();

    // 显示设备编号会因为驱动重建拓扑而变化（开关 DSR、驱动重启、换线等），
    // 配置里存的名字可能一个都对不上。这时回退到"所有非主显示器"，避免静默失效。
    if (!g_selected.empty() && !g_mons.empty())
    {
        bool anyMatch = false;
        for (size_t i = 0; i < g_selected.size() && !anyMatch; i++)
            for (size_t m = 0; m < g_mons.size(); m++)
                if (_wcsicmp(g_selected[i].c_str(), g_mons[m].device.c_str()) == 0) { anyMatch = true; break; }

        if (!anyMatch)
        {
            g_selected.clear();
            for (size_t m = 0; m < g_mons.size(); m++)
                if (!g_mons[m].primary) g_selected.push_back(g_mons[m].device);
            if (g_selected.empty()) g_selected.push_back(g_mons[0].device);

            std::wstring names;
            for (size_t i = 0; i < g_selected.size(); i++)
            {
                if (i) names += L", ";
                names += g_selected[i];
            }
            Log(L"selection lost (device names changed), fallback to: %ls", names.c_str());
        }
    }

    for (int i = (int)g_targets.size() - 1; i >= 0; i--)
    {
        Target* t = g_targets[i];
        MonInfo* found = nullptr;
        for (size_t m = 0; m < g_mons.size(); m++)
            if (_wcsicmp(g_mons[m].device.c_str(), t->mon.device.c_str()) == 0) { found = &g_mons[m]; break; }

        if (!found || !IsSelected(t->mon.device))
        {
            FreeOverlay(t->overlay);
            delete t;
            g_targets.erase(g_targets.begin() + i);
            continue;
        }
        if (memcmp(&found->rect, &t->mon.rect, sizeof(RECT)) != 0)
        {
            t->mon = *found;
            OverlayCtx* ctx = (OverlayCtx*)GetWindowLongPtrW(t->overlay, GWLP_USERDATA);
            if (ctx) ctx->placed = false;
            if (t->phase != PH_HIDDEN) OverlayPlace(t->overlay, t->mon.rect);
        }
    }

    for (size_t m = 0; m < g_mons.size(); m++)
    {
        if (!IsSelected(g_mons[m].device)) continue;
        bool exists = false;
        for (size_t k = 0; k < g_targets.size(); k++)
            if (_wcsicmp(g_targets[k]->mon.device.c_str(), g_mons[m].device.c_str()) == 0) { exists = true; break; }
        if (exists) continue;

        Target* t = new Target();
        t->mon = g_mons[m];
        t->overlay = CreateOverlay(false);
        t->opacity = 0;
        t->phase = PH_HIDDEN;
        t->phaseStart = 0;
        t->lastSeen = now;
        t->fs = false;
        t->fsNext = 0;
        g_targets.push_back(t);
        Log(L"added %ls", g_mons[m].Label().c_str());
    }
}

static void DefaultSelection()
{
    if (!g_selected.empty()) return;
    for (size_t i = 0; i < g_mons.size(); i++)
        if (!g_mons[i].primary) g_selected.push_back(g_mons[i].device);
    if (g_selected.empty() && !g_mons.empty()) g_selected.push_back(g_mons[0].device);
}

// ---------- preview ----------
static void RunPreview(const std::vector<MonInfo>& list)
{
    EndPreview(true);
    if (list.empty()) return;
    g_previewing = true;
    g_previewBlackAt = 0;
    g_previewEnding = false;
    for (size_t i = 0; i < list.size(); i++)
    {
        Target* t = new Target();
        t->mon = list[i];
        t->overlay = CreateOverlay(true);
        t->opacity = 0;
        t->phase = PH_HIDDEN;
        t->phaseStart = 0;
        t->lastSeen = GetTickCount();
        g_preview.push_back(t);
        StartBlank(t);
    }
    Log(L"preview start on %d monitor(s)", (int)list.size());
    SetTimer(g_msgWnd, TIMER_ESC, 30, nullptr);
}

static void EndPreview(bool abort)
{
    KillTimer(g_msgWnd, TIMER_ESC);
    for (size_t i = 0; i < g_preview.size(); i++)
    {
        FreeOverlay(g_preview[i]->overlay);
        delete g_preview[i];
    }
    g_preview.clear();
    if (g_previewing) ResetIdle();
    g_previewing = false;
    g_previewBlackAt = 0;
    g_previewEnding = false;
    if (abort) Log(L"preview ended");
}

// ---------- settings window ----------
struct FieldDef
{
    const wchar_t* label;
    const wchar_t* hint;
    int* value;
    int  min, max;
};
static FieldDef g_fields[6];

static void BuildFields()
{
    g_fields[0].label = L"空闲超时（秒）";        g_fields[0].hint = L"鼠标离开多久开始熄灭"; g_fields[0].value = &g_idleSec;     g_fields[0].min = 5; g_fields[0].max = 86400;
    g_fields[1].label = L"亮起渐变（毫秒）";      g_fields[1].hint = L"鼠标进入后亮起用时";   g_fields[1].value = &g_wakeMs;      g_fields[1].min = 0; g_fields[1].max = 5000;
    g_fields[2].label = L"首次变暗渐入（毫秒）";  g_fields[2].hint = L"从亮到半暗用时";       g_fields[2].value = &g_dimFadeMs;   g_fields[2].min = 0; g_fields[2].max = 5000;
    g_fields[3].label = L"首次变暗不透明度（%）"; g_fields[3].hint = L"半暗时黑屏覆盖比例";   g_fields[3].value = &g_dimOpacity;  g_fields[3].min = 1; g_fields[3].max = 100;
    g_fields[4].label = L"首次变暗持续（秒）";    g_fields[4].hint = L"半暗保持多久";         g_fields[4].value = &g_dimHoldSec;  g_fields[4].min = 0; g_fields[4].max = 600;
    g_fields[5].label = L"二次渐变渐入（毫秒）";  g_fields[5].hint = L"从半暗到全黑用时";     g_fields[5].value = &g_blackFadeMs; g_fields[5].min = 0; g_fields[5].max = 5000;
}

static std::vector<MonInfo> GetCheckedMons(HWND dlg)
{
    std::vector<MonInfo> out;
    HWND lv = GetDlgItem(dlg, IDC_LIST);
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; i++)
        if (ListView_GetCheckState(lv, i) && i < (int)g_mons.size())
            out.push_back(g_mons[i]);
    return out;
}

static void ApplyFromDialog(HWND dlg)
{
    wchar_t buf[64];
    for (int i = 0; i < 6; i++)
    {
        GetDlgItemTextW(dlg, IDC_EDIT0 + i, buf, 64);
        int v = _wtoi(buf);
        if (v < g_fields[i].min) v = g_fields[i].min;
        if (v > g_fields[i].max) v = g_fields[i].max;
        *g_fields[i].value = v;
    }
    std::vector<MonInfo> picked = GetCheckedMons(dlg);
    g_selected.clear();
    for (size_t i = 0; i < picked.size(); i++) g_selected.push_back(picked[i].device);

    g_fsGuard = (SendDlgItemMessageW(dlg, IDC_FSGUARD, BM_GETCHECK, 0, 0) == BST_CHECKED);

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &k) == ERROR_SUCCESS)
    {
        bool on = SendDlgItemMessageW(dlg, IDC_AUTO, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (on)
        {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring q = L"\"";
            q += exe;
            q += L"\"";
            RegSetValueExW(k, L"BurnInGuard", 0, REG_SZ, (const BYTE*)q.c_str(), (DWORD)((q.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(k, L"BurnInGuard");
        }
        RegCloseKey(k);
    }

    std::wstring path = g_dir + L"\\settings.ini";
    wchar_t ini[2048];
    std::wstring mons = L"monitors=";
    for (size_t i = 0; i < g_selected.size(); i++)
    {
        if (i) mons += L"|";
        mons += g_selected[i];
    }
    swprintf(ini, 2048, L"idle=%d\r\nwake=%d\r\ndimfade=%d\r\ndimopacity=%d\r\ndimhold=%d\r\nblackfade=%d\r\nfsguard=%d\r\n%ls\r\n",
             g_idleSec, g_wakeMs, g_dimFadeMs, g_dimOpacity, g_dimHoldSec, g_blackFadeMs,
             g_fsGuard ? 1 : 0, mons.c_str());
    char utf8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, ini, -1, utf8, sizeof(utf8), nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(f, utf8, (DWORD)(n - 1), &w, nullptr);
        CloseHandle(f);
    }

    RefreshTargets(true);
    Log(L"settings idle=%d wake=%d dimIn=%d dim=%d hold=%d black=%d",
        g_idleSec, g_wakeMs, g_dimFadeMs, g_dimOpacity, g_dimHoldSec, g_blackFadeMs);
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                      int x, int y, int w, int hh, int id) -> HWND
        {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                     x, y, w, hh, h, (HMENU)(INT_PTR)id, g_inst, nullptr);
            if (c && g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
            return c;
        };

        mk(L"STATIC", L"要保护的显示器（可多选）", SS_LEFT, 14, 10, 320, 18, IDC_TITLE);

        HWND lv = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER, 14, 32, 512, 84, IDC_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW col;
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_WIDTH | LVCF_TEXT;
        col.cx = 500;
        col.pszText = (LPWSTR)L"显示器";
        ListView_InsertColumn(lv, 0, &col);
        for (int i = 0; i < (int)g_mons.size(); i++)
        {
            std::wstring label = g_mons[i].Label();
            LVITEMW it;
            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = (LPWSTR)label.c_str();
            ListView_InsertItem(lv, &it);
            ListView_SetCheckState(lv, i, IsSelected(g_mons[i].device) ? TRUE : FALSE);
        }

        for (int i = 0; i < 6; i++)
        {
            int y = 126 + i * 32;
            mk(L"STATIC", g_fields[i].label, SS_LEFT, 14, y, 162, 18, IDC_LBL0 + i);

            wchar_t buf[32];
            swprintf(buf, 32, L"%d", *g_fields[i].value);
            HWND ed = mk(L"EDIT", buf, ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL, 180, y - 4, 70, 22, IDC_EDIT0 + i);
            SendMessageW(ed, EM_SETLIMITTEXT, 6, 0);

            HWND ud = CreateWindowExW(0, L"msctls_updown32", L"",
                                      WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT,
                                      180 + 70 - 17, y - 4, 17, 22, h, (HMENU)(INT_PTR)(IDC_UD0 + i), g_inst, nullptr);
            SendMessageW(ud, UDM_SETBUDDY, (WPARAM)ed, 0);
            SendMessageW(ud, UDM_SETRANGE32, (WPARAM)g_fields[i].min, (LPARAM)g_fields[i].max);
            SendMessageW(ud, UDM_SETPOS32, 0, (LPARAM)*g_fields[i].value);

            mk(L"STATIC", g_fields[i].hint, SS_LEFT, 268, y, 250, 18, IDC_HNT0 + i);
        }

        mk(L"BUTTON", L"预览", BS_PUSHBUTTON, 14, 322, 110, 30, IDC_PREVIEW);
        mk(L"STATIC", L"按上面设定完整走一遍：半暗 → 保持 → 全黑 → 停 5 秒 → 渐隐恢复。按 ESC 可立刻退出。",
           SS_LEFT, 134, 316, 392, 44, IDC_TESTHINT);

        HWND chk = mk(L"BUTTON", L"开机自动启动（登录后自动在托盘运行）", BS_AUTOCHECKBOX, 14, 370, 420, 22, IDC_AUTO);
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS)
        {
            wchar_t val[MAX_PATH * 2];
            DWORD sz = sizeof(val), type = 0;
            if (RegQueryValueExW(k, L"BurnInGuard", nullptr, &type, (LPBYTE)val, &sz) == ERROR_SUCCESS)
                SendMessageW(chk, BM_SETCHECK, BST_CHECKED, 0);
            RegCloseKey(k);
        }

        HWND fs = mk(L"BUTTON", L"识别到全屏程序时不变暗（游戏 / 视频 / 演示）", BS_AUTOCHECKBOX, 14, 396, 460, 22, IDC_FSGUARD);
        SendMessageW(fs, BM_SETCHECK, g_fsGuard ? BST_CHECKED : BST_UNCHECKED, 0);

        mk(L"BUTTON", g_enabled ? L"关闭防烧屏" : L"开启防烧屏", BS_PUSHBUTTON, 14, 428, 140, 30, IDC_TOGGLE);
        mk(L"STATIC", g_enabled ? L"当前状态：已开启" : L"当前状态：已关闭", SS_LEFT, 170, 434, 360, 20, IDC_STATUS);

        mk(L"BUTTON", L"应用", BS_PUSHBUTTON, 266, 478, 80, 28, IDC_APPLY);
        mk(L"BUTTON", L"确定", BS_DEFPUSHBUTTON, 356, 478, 80, 28, IDOK);
        mk(L"BUTTON", L"取消", BS_PUSHBUTTON, 446, 478, 80, 28, IDCANCEL);

        g_inSettings = true;
        for (size_t i = 0; i < g_targets.size(); i++)
            if (g_targets[i]->phase != PH_HIDDEN) Wake(g_targets[i]);
        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDOK)
        {
            ApplyFromDialog(h);
            g_inSettings = false;
            DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL)
        {
            EndPreview(true);
            g_inSettings = false;
            DestroyWindow(h);
            return 0;
        }
        if (id == IDC_PREVIEW)
        {
            RunPreview(GetCheckedMons(h));
            return 0;
        }
        if (id == IDC_TOGGLE)
        {
            ToggleEnabled();
            return 0;
        }
        if (id == IDC_APPLY)
        {
            ApplyFromDialog(h);
            ResetIdle();
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == GetDlgItem(h, IDC_STATUS))
        {
            SetTextColor(dc, g_enabled ? RGB(0, 150, 60) : RGB(200, 40, 40));
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }

    case WM_CLOSE:
        EndPreview(true);
        g_inSettings = false;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        g_settingsWnd = nullptr;
        ResetIdle();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowSettings()
{
    if (g_settingsWnd)
    {
        SetForegroundWindow(g_settingsWnd);
        return;
    }
    EnumMons();

    if (!g_font)
    {
        int dpi = 96;
        HDC dc = GetDC(nullptr);
        if (dc) { dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc); }
        g_font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"BurnInGuardSettings";
    wc.hIcon = g_trayIcon;
    RegisterClassExW(&wc);

    RECT r = { 0, 0, 540, 520 };
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, 0);
    int w = r.right - r.left, hh = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - hh) / 2;

    g_settingsWnd = CreateWindowExW(0, L"BurnInGuardSettings", L"防烧屏 — 设置",
                                    WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    x, y, w, hh, nullptr, nullptr, g_inst, nullptr);
    if (!g_settingsWnd) return;
    ShowWindow(g_settingsWnd, SW_SHOW);
    SetForegroundWindow(g_settingsWnd);
}

// ---------- settings load ----------
static void LoadSettings()
{
    std::wstring path = g_dir + L"\\settings.ini";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(f, nullptr);
    if (size == 0 || size > 64 * 1024) { CloseHandle(f); return; }

    char* buf = new char[size + 2];
    DWORD n = 0;
    ReadFile(f, buf, size, &n, nullptr);
    buf[n] = 0;
    CloseHandle(f);

    std::wstring w;
    int wn = MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, nullptr, 0);
    if (wn > 0)
    {
        w.resize(wn);
        MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, &w[0], wn);
    }
    delete[] buf;

    size_t pos = 0;
    while (pos < w.size())
    {
        size_t e = w.find(L'\n', pos);
        if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line[line.size() - 1] == L'\r' || line[line.size() - 1] == L' '))
            line.erase(line.size() - 1);

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);

        if (k == L"monitors")
        {
            g_selected.clear();
            size_t s = 0;
            while (s <= v.size())
            {
                size_t p = v.find(L'|', s);
                std::wstring item = v.substr(s, p == std::wstring::npos ? std::wstring::npos : p - s);
                if (!item.empty()) g_selected.push_back(item);
                if (p == std::wstring::npos) break;
                s = p + 1;
            }
            continue;
        }
        int num = _wtoi(v.c_str());
        if (k == L"idle") g_idleSec = num;
        else if (k == L"wake") g_wakeMs = num;
        else if (k == L"dimfade") g_dimFadeMs = num;
        else if (k == L"dimopacity") g_dimOpacity = num;
        else if (k == L"dimhold") g_dimHoldSec = num;
        else if (k == L"blackfade") g_blackFadeMs = num;
        else if (k == L"fsguard") g_fsGuard = (num != 0);
    }
}

// ---------- tray ----------
static void AddTrayIcon()
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_trayIcon;
    wcsncpy(nid.szTip, L"防烧屏 — 运行中", 127);
    nid.szTip[127] = 0;
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    DWORD err = GetLastError();

    wchar_t wsName[128] = L"?", deskName[128] = L"?";
    DWORD need = 0;
    HWINSTA hwinsta = GetProcessWindowStation();
    if (hwinsta) GetUserObjectInformationW(hwinsta, UOI_NAME, wsName, sizeof(wsName) - 2, &need);
    HDESK hdesk = GetThreadDesktop(GetCurrentThreadId());
    if (hdesk) GetUserObjectInformationW(hdesk, UOI_NAME, deskName, sizeof(deskName) - 2, &need);
    HWND shellTray = FindWindowW(L"Shell_TrayWnd", nullptr);

    Log(L"tray add ok=%d err=%lu cbSize=%d hwnd=%p icon=%p shellTray=%p winsta=%ls desk=%ls",
        ok, err, (int)sizeof(nid), (void*)g_msgWnd, (void*)g_trayIcon, (void*)shellTray, wsName, deskName);
}

static void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void TrayMenu()
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"打开主界面");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : MF_UNCHECKED), IDM_ENABLE, L"防烧屏");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    SetForegroundWindow(g_msgWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, g_msgWnd, nullptr);
    PostMessageW(g_msgWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------- message window ----------
static LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) { TrayMenu(); return 0; }
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) { ShowSettings(); return 0; }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDM_OPEN) { ShowSettings(); return 0; }
        if (LOWORD(wp) == IDM_ENABLE) { ToggleEnabled(); return 0; }
        if (LOWORD(wp) == IDM_EXIT)
        {
            KillTimer(h, TIMER_TICK);
            KillTimer(h, TIMER_ANIM);
            KillTimer(h, TIMER_ESC);
            EndPreview(true);
            for (size_t i = 0; i < g_targets.size(); i++)
            {
                FreeOverlay(g_targets[i]->overlay);
                delete g_targets[i];
            }
            g_targets.clear();
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == TIMER_TICK)
        {
            RefreshTargets(false);

            if (!g_enabled)
            {
                // 总开关关闭：不主动变暗，已变暗的立刻恢复
                for (size_t i = 0; i < g_targets.size(); i++)
                    if (g_targets[i]->phase != PH_HIDDEN && g_targets[i]->phase != PH_FADEOUT)
                        Wake(g_targets[i]);
                return 0;
            }

            if (g_previewing || g_inSettings || g_targets.empty()) return 0;

            POINT pt;
            if (!GetCursorPos(&pt)) return 0;
            DWORD now = GetTickCount();

            for (size_t i = 0; i < g_targets.size(); i++)
            {
                Target* t = g_targets[i];

                // 全屏判定比较贵（要遍历 Z 序），每 500ms 算一次就够
                if ((long)(now - t->fsNext) >= 0)
                {
                    t->fsNext = now + 500;
                    bool full = g_fsGuard && FullscreenOn(t->mon.hmon, t->mon.rect);
                    if (full != t->fs)
                    {
                        t->fs = full;
                        Log(L"%ls fullscreen=%d", t->mon.device.c_str(), full ? 1 : 0);
                    }
                }
                if (t->fs)
                {
                    // 有程序在这块屏上全屏/独占全屏，永不变暗；同时把空闲计时推后，
                    // 免得退出全屏的瞬间立刻变黑
                    t->lastSeen = now;
                    if (t->phase != PH_HIDDEN && t->phase != PH_FADEOUT) Wake(t);
                    continue;
                }

                if (t->phase == PH_FADEOUT) continue;
                if (Contains(t->mon.rect, pt))
                {
                    t->lastSeen = now;
                    if (t->phase != PH_HIDDEN) Wake(t);
                    continue;
                }
                if (t->phase == PH_HIDDEN && (now - t->lastSeen) >= (DWORD)(g_idleSec * 1000))
                    StartBlank(t);
            }
            return 0;
        }
        if (wp == TIMER_ANIM)
        {
            DWORD now = GetTickCount();
            std::vector<Target*> all = g_targets;
            all.insert(all.end(), g_preview.begin(), g_preview.end());
            for (size_t i = 0; i < all.size(); i++) StepTarget(all[i], now);
            EnsureAnim();
            return 0;
        }
        if (wp == TIMER_ESC)
        {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                Log(L"preview aborted by ESC");
                EndPreview(true);
                return 0;
            }
            if (g_preview.empty()) return 0;

            DWORD now = GetTickCount();
            if (!g_previewEnding)
            {
                bool allBlack = true;
                for (size_t i = 0; i < g_preview.size(); i++)
                    if (g_preview[i]->phase != PH_BLACK) { allBlack = false; break; }

                if (allBlack)
                {
                    if (g_previewBlackAt == 0)
                    {
                        g_previewBlackAt = now;
                        Log(L"preview fully black, hold 5s");
                    }
                    else if ((now - g_previewBlackAt) >= 5000)
                    {
                        g_previewEnding = true;
                        Log(L"preview fade out");
                        for (size_t i = 0; i < g_preview.size(); i++)
                            Wake(g_preview[i]);
                    }
                }
            }
            else
            {
                bool allHidden = true;
                for (size_t i = 0; i < g_preview.size(); i++)
                    if (g_preview[i]->phase != PH_HIDDEN) { allHidden = false; break; }
                if (allHidden)
                {
                    Log(L"preview finished");
                    EndPreview(false);
                }
            }
            return 0;
        }
        break;
    }

    if (g_taskbarCreated && msg == g_taskbarCreated)
    {
        AddTrayIcon();
        return 0;
    }

    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- entry ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    g_inst = hInst;

    HANDLE once = CreateMutexW(nullptr, TRUE, L"BurnInGuard_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_dir = ExeDir();
    LoadSettings();
    BuildFields();

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_trayIcon = MakeTrayIcon();
    RegisterOverlayClass();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"BurnInGuardMsg";
    RegisterClassExW(&wc);

    g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"BurnInGuardMsg", L"BurnInGuard", WS_POPUP,
                               0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_msgWnd) return 1;

    EnumMons();
    DefaultSelection();
    RefreshTargets(true);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    Log(L"start idle=%d wake=%d dimIn=%d dim=%d hold=%d black=%d",
        g_idleSec, g_wakeMs, g_dimFadeMs, g_dimOpacity, g_dimHoldSec, g_blackFadeMs);

    SetTimer(g_msgWnd, TIMER_TICK, 200, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
