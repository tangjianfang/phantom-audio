// uacstudio —— UAC 仿真测试台 Win32 GUI（C++20，标准控件 + owner-draw 键帽）
// 设计系统：深色仪表台面——底 #171A21 / 面板 #1F242E / 墨 #DDE3EA / 弱化 #8B93A1 /
//   仪器琥珀强调 #E8A33D / 信号绿 #4FB573 / 故障红 #E05252；
//   Segoe UI（句首大写标签）+ Cascadia Mono（日志/数据）；
//   布局：顶部设备状态条（LED+标题，全窗唯一强调处）→ 分组标签 → 键帽网格 → 日志满铺。
#include "http_client.h"
#include "env_check.h"
#include "auto_test.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr COLORREF C_BG = RGB(0x17, 0x1A, 0x21);
constexpr COLORREF C_PANEL = RGB(0x1F, 0x24, 0x2E);
constexpr COLORREF C_KEY = RGB(0x27, 0x2D, 0x39);
constexpr COLORREF C_KEY_HOVER = RGB(0x2F, 0x36, 0x44);
constexpr COLORREF C_KEY_DOWN = RGB(0x38, 0x40, 0x4F);
constexpr COLORREF C_KEY_BORDER = RGB(0x3C, 0x44, 0x52);
constexpr COLORREF C_INK = RGB(0xDD, 0xE3, 0xEA);
constexpr COLORREF C_MUTED = RGB(0x8B, 0x93, 0xA1);
constexpr COLORREF C_ACCENT = RGB(0xE8, 0xA3, 0x3D);
constexpr COLORREF C_OK = RGB(0x4F, 0xB5, 0x73);
constexpr COLORREF C_FAULT = RGB(0xE0, 0x52, 0x52);

constexpr uint16_t kDefaultPort = 3241;
uint16_t g_port = kDefaultPort;

HBRUSH g_brBg = nullptr, g_brPanel = nullptr;
HFONT g_fUi = nullptr, g_fTitle = nullptr, g_fMono = nullptr;

struct Ctl {
    HWND wnd = nullptr;
    void create(HWND parent, int id, const wchar_t* cls, const wchar_t* text, DWORD style,
                DWORD exStyle, HINSTANCE hi, HFONT f) {
        wnd = CreateWindowExW(exStyle, cls, text, style, 0, 0, 10, 10, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
        SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    }
};

Ctl g_led, g_title, g_sub;
Ctl g_profileCombo, g_attachBtn, g_detachBtn, g_envBtn;
Ctl g_lblInject, g_btnVolUp, g_btnVolDown, g_btnMute, g_btnHidUp, g_btnHidDown, g_btnHidMute;
Ctl g_btnHidRel, g_btnStall, g_btnDelay, g_btnDisconnect, g_btnClear;
Ctl g_lblStream, g_hzEdit, g_btnSine, g_btnSilence, g_capEdit, g_btnCapStart, g_btnCapStop;
Ctl g_lblLog, g_logEdit, g_btnSelfTest;
std::atomic<bool> g_testRunning{false};
std::thread g_testThread{};
std::mutex g_lineMtx;
std::vector<std::string> g_lines;
constexpr UINT WM_APP_TESTLINE = WM_APP + 1;
HWND g_main = nullptr;
int g_ledState = 0; // 0 离线 1 监听 2 已导入

const wchar_t* kProfiles[] = {L"uac2-48k", L"uac1-cmedia-sim", L"uac2-hid-headset", L"uac2-sim"};

int Scale(HWND hwnd, int logical) {
    return MulDiv(logical, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

enum class KeyTone { Neutral, Accent, Fault, Quiet };
void DrawKey(LPDRAWITEMSTRUCT dis, const wchar_t* text, KeyTone tone) {
    const RECT& r = dis->rcItem;
    const bool hover = (dis->itemState & ODS_HOTLIGHT) != 0;
    const bool down = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF face = down ? C_KEY_DOWN : hover ? C_KEY_HOVER : C_KEY;
    COLORREF border = C_KEY_BORDER;
    COLORREF ink = C_INK;
    switch (tone) {
    case KeyTone::Accent: border = C_ACCENT; break;
    case KeyTone::Fault: border = C_FAULT; break;
    case KeyTone::Quiet: ink = C_MUTED; break;
    default: break;
    }
    HBRUSH b = CreateSolidBrush(face);
    FillRect(dis->hDC, &r, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dis->hDC, pen));
    HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(dis->hDC, GetStockObject(NULL_BRUSH)));
    Rectangle(dis->hDC, r.left, r.top, r.right, r.bottom);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBr);
    DeleteObject(pen);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, ink);
    HFONT oldF = static_cast<HFONT>(SelectObject(dis->hDC, g_fUi));
    RECT tr = r;
    DrawTextW(dis->hDC, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, oldF);
}

int LayoutSections(int W) {
    const int m = Scale(g_main, 12);
    int y = m;
    const int barH = Scale(g_main, 44);
    MoveWindow(g_led.wnd, m + Scale(g_main, 10), y + Scale(g_main, 12), Scale(g_main, 18),
               Scale(g_main, 18), TRUE);
    MoveWindow(g_title.wnd, m + Scale(g_main, 36), y + Scale(g_main, 5), Scale(g_main, 340),
               Scale(g_main, 22), TRUE);
    MoveWindow(g_sub.wnd, m + Scale(g_main, 36), y + Scale(g_main, 25), Scale(g_main, 400),
               Scale(g_main, 16), TRUE);
    MoveWindow(g_profileCombo.wnd, W - m - Scale(g_main, 452), y + Scale(g_main, 9),
               Scale(g_main, 158), Scale(g_main, 200), TRUE);
    MoveWindow(g_envBtn.wnd, W - m - Scale(g_main, 284), y + Scale(g_main, 8),
               Scale(g_main, 96), Scale(g_main, 28), TRUE);
    MoveWindow(g_attachBtn.wnd, W - m - Scale(g_main, 182), y + Scale(g_main, 8),
               Scale(g_main, 86), Scale(g_main, 28), TRUE);
    MoveWindow(g_detachBtn.wnd, W - m - Scale(g_main, 90), y + Scale(g_main, 8),
               Scale(g_main, 86), Scale(g_main, 28), TRUE);
    y += barH + Scale(g_main, 16);
    MoveWindow(g_lblInject.wnd, m, y, Scale(g_main, 320), Scale(g_main, 18), TRUE);
    y += Scale(g_main, 24);
    const int keyW = Scale(g_main, 88), keyH = Scale(g_main, 28), gap = Scale(g_main, 6);
    Ctl row1[] = {g_btnVolUp, g_btnVolDown, g_btnMute, g_btnHidUp, g_btnHidDown};
    Ctl row2[] = {g_btnHidMute, g_btnHidRel, g_btnStall, g_btnDelay, g_btnDisconnect, g_btnClear};
    for (int i = 0; i < 5; ++i) MoveWindow(row1[i].wnd, m + i * (keyW + gap), y, keyW, keyH, TRUE);
    y += keyH + gap;
    for (int i = 0; i < 6; ++i) MoveWindow(row2[i].wnd, m + i * (keyW + gap), y, keyW, keyH, TRUE);
    y += keyH + Scale(g_main, 16);
    MoveWindow(g_lblStream.wnd, m, y, Scale(g_main, 320), Scale(g_main, 18), TRUE);
    y += Scale(g_main, 24);
    MoveWindow(g_hzEdit.wnd, m, y, Scale(g_main, 64), Scale(g_main, 26), TRUE);
    MoveWindow(g_btnSine.wnd, m + Scale(g_main, 68), y, keyW, keyH, TRUE);
    MoveWindow(g_btnSilence.wnd, m + Scale(g_main, 68) + keyW + gap, y, keyW, keyH, TRUE);
    MoveWindow(g_btnCapStart.wnd, W - m - 2 * (keyW + gap), y, keyW, keyH, TRUE);
    MoveWindow(g_btnCapStop.wnd, W - m - (keyW + gap), y, keyW, keyH, TRUE);
    const int capX = m + Scale(g_main, 68) + 2 * (keyW + gap);
    MoveWindow(g_capEdit.wnd, capX, y,
               std::max(W - m - capX - 2 * (keyW + gap), Scale(g_main, 80)), Scale(g_main, 26),
               TRUE);
    y += keyH + Scale(g_main, 16);
    MoveWindow(g_lblLog.wnd, m, y, Scale(g_main, 320), Scale(g_main, 18), TRUE);
    y += Scale(g_main, 24);
    return y;
}

void DoLayout() {
    if (!g_main) return;
    RECT rc{};
    GetClientRect(g_main, &rc);
    const int y = LayoutSections(rc.right);
    MoveWindow(g_logEdit.wnd, Scale(g_main, 12), y, rc.right - 2 * Scale(g_main, 12),
               std::max(static_cast<int>(rc.bottom) - y - Scale(g_main, 12), Scale(g_main, 60)), TRUE);
    InvalidateRect(g_main, nullptr, TRUE);
}

void AppendLog(const std::string& text) {
    const int len = GetWindowTextLengthA(g_logEdit.wnd);
    SendMessageA(g_logEdit.wnd, EM_SETSEL, len, len);
    SendMessageA(g_logEdit.wnd, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>((text + "\r\n").c_str()));
}

std::string CurrentProfile() {
    wchar_t buf[128]{};
    GetWindowTextW(g_profileCombo.wnd, buf, 128);
    std::string s;
    for (int i = 0; buf[i]; ++i) s.push_back(static_cast<char>(buf[i]));
    return s;
}

void SetLed(int state, const wchar_t* sub) {
    g_ledState = state;
    InvalidateRect(g_led.wnd, nullptr, TRUE);
    SetWindowTextW(g_sub.wnd, sub);
}

void RefreshStatus() {
    const auto r = uacstudio::http_req(g_port, "GET", "/status", "");
    if (r.empty()) {
        SetLed(0, L"uacsimd 未连接 · 127.0.0.1:3241");
        return;
    }
    const bool listening = r.find("\"listening\":true") != std::string::npos;
    const bool imported = r.find("\"imported\":true") != std::string::npos;
    SetLed(imported ? 2 : (listening ? 1 : 0),
           imported ? L"已导入 · vhci 占用中"
                    : listening ? L"监听中 · 等待 usbip attach" : L"空闲 · 选择 Profile 后 Attach");
}

void RefreshLog() {
    if (g_testRunning.load()) return; // self-test output owns the log panel
    const auto r = uacstudio::http_req(g_port, "GET", "/log?n=200", "");
    if (r.empty() || r.size() < 3 || r.size() > 60000) return;
    SetWindowTextA(g_logEdit.wnd, r.c_str());
    SendMessageA(g_logEdit.wnd, EM_SETSEL, r.size() - 1, r.size() - 1);
}

void SetEnabled(bool on) { EnableWindow(g_main, on ? TRUE : FALSE); }

void Post(const std::string& path, const std::string& body, const char* what) {
    std::string out;
    if (uacstudio::http_post(g_port, path, body, &out))
        AppendLog(std::string("[") + what + "] " + out);
    else
        AppendLog(std::string("[") + what + "] 请求失败（uacsimd 未启动？）");
    RefreshStatus();
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        const DWORD vis = WS_CHILD | WS_VISIBLE;
        const DWORD key = vis | BS_OWNERDRAW | WS_TABSTOP;
        g_brBg = CreateSolidBrush(C_BG);
        g_brPanel = CreateSolidBrush(C_PANEL);
        g_fUi = CreateFontW(-Scale(h, 14), 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fTitle = CreateFontW(-Scale(h, 22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fMono = CreateFontW(-Scale(h, 13), 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        g_led.create(h, 900, L"STATIC", nullptr, vis | SS_OWNERDRAW, 0, cs->hInstance, g_fUi);
        g_title.create(h, 901, L"STATIC", L"uacstudio", vis, 0, cs->hInstance, g_fTitle);
        g_sub.create(h, 902, L"STATIC", L"...", vis, 0, cs->hInstance, g_fUi);
        g_profileCombo.create(h, 100, L"COMBOBOX", nullptr, vis | CBS_DROPDOWNLIST | WS_VSCROLL,
                              0, cs->hInstance, g_fUi);
        for (auto* p : kProfiles)
            SendMessageW(g_profileCombo.wnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
        SendMessageW(g_profileCombo.wnd, CB_SETCURSEL, 0, 0);
        g_attachBtn.create(h, 101, L"BUTTON", L"Attach", key, 0, cs->hInstance, g_fUi);
        g_detachBtn.create(h, 102, L"BUTTON", L"Detach", key, 0, cs->hInstance, g_fUi);
        g_envBtn.create(h, 910, L"BUTTON", L"环境检查", key, 0, cs->hInstance, g_fUi);
        g_lblInject.create(h, 903, L"STATIC", L"设备侧事件与故障注入", vis, 0, cs->hInstance,
                           g_fUi);
        g_btnVolUp.create(h, 200, L"BUTTON", L"音量 +", key, 0, cs->hInstance, g_fUi);
        g_btnVolDown.create(h, 201, L"BUTTON", L"音量 -", key, 0, cs->hInstance, g_fUi);
        g_btnMute.create(h, 202, L"BUTTON", L"静音", key, 0, cs->hInstance, g_fUi);
        g_btnHidUp.create(h, 203, L"BUTTON", L"HID 音量+", key, 0, cs->hInstance, g_fUi);
        g_btnHidDown.create(h, 204, L"BUTTON", L"HID 音量-", key, 0, cs->hInstance, g_fUi);
        g_btnHidMute.create(h, 205, L"BUTTON", L"HID 静音", key, 0, cs->hInstance, g_fUi);
        g_btnHidRel.create(h, 206, L"BUTTON", L"HID 释放", key, 0, cs->hInstance, g_fUi);
        g_btnStall.create(h, 210, L"BUTTON", L"STALL", key, 0, cs->hInstance, g_fUi);
        g_btnDelay.create(h, 211, L"BUTTON", L"延迟注入", key, 0, cs->hInstance, g_fUi);
        g_btnDisconnect.create(h, 212, L"BUTTON", L"拔线", key, 0, cs->hInstance, g_fUi);
        g_btnClear.create(h, 213, L"BUTTON", L"清注入", key, 0, cs->hInstance, g_fUi);
        g_btnSelfTest.create(h, 500, L"BUTTON", L"自测", key, 0, cs->hInstance, g_fUi);
        g_lblStream.create(h, 904, L"STATIC", L"音频流（麦克风源 / 扬声器录音）", vis, 0,
                           cs->hInstance, g_fUi);
        g_hzEdit.create(h, 400, L"EDIT", L"1000", vis | ES_AUTOHSCROLL | WS_BORDER,
                        WS_EX_CLIENTEDGE, cs->hInstance, g_fMono);
        g_btnSine.create(h, 401, L"BUTTON", L"正弦", key, 0, cs->hInstance, g_fUi);
        g_btnSilence.create(h, 402, L"BUTTON", L"静音源", key, 0, cs->hInstance, g_fUi);
        g_capEdit.create(h, 403, L"EDIT", L"capture.wav", vis | ES_AUTOHSCROLL | WS_BORDER,
                         WS_EX_CLIENTEDGE, cs->hInstance, g_fMono);
        g_btnCapStart.create(h, 404, L"BUTTON", L"录音开", key, 0, cs->hInstance, g_fUi);
        g_btnCapStop.create(h, 405, L"BUTTON", L"录音停", key, 0, cs->hInstance, g_fUi);
        g_lblLog.create(h, 905, L"STATIC", L"请求日志（NDJSON，500ms 刷新）", vis, 0,
                        cs->hInstance, g_fUi);
        g_logEdit.create(h, 300, L"EDIT", nullptr,
                         vis | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                         WS_EX_CLIENTEDGE, cs->hInstance, g_fMono);
        SetTimer(h, 1, 1000, nullptr);
        SetTimer(h, 2, 500, nullptr);
        DoLayout();
        RefreshStatus();
        return 0;
    }
    case WM_SIZE:
        DoLayout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {Scale(h, 720), Scale(h, 480)};
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) RefreshStatus();
        if (wp == 2) RefreshLog();
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: { // 下拉列表走 LISTBOX，编辑段走 EDIT
        auto dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, C_PANEL);
        SetTextColor(dc, C_INK);
        return reinterpret_cast<LRESULT>(g_brPanel);
    }
    case WM_CTLCOLORSTATIC: {
        auto dc = reinterpret_cast<HDC>(wp);
        auto ctrl = reinterpret_cast<HWND>(lp);
        SetBkColor(dc, C_BG);
        SetTextColor(dc, ctrl == g_sub.wnd || ctrl == g_lblInject.wnd || ctrl == g_lblStream.wnd ||
                                 ctrl == g_lblLog.wnd
                             ? C_MUTED
                             : C_INK);
        return reinterpret_cast<LRESULT>(g_brBg);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lp);
        if (dis->CtlID == 900) { // 状态灯：灰=离线 琥珀=监听 绿=已导入
            const COLORREF c = g_ledState == 2 ? C_OK : g_ledState == 1 ? C_ACCENT : C_KEY_BORDER;
            HBRUSH b = CreateSolidBrush(c);
            const int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
            const int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            const int r = Scale(h, 5);
            auto old = static_cast<HBRUSH>(SelectObject(dis->hDC, b));
            HPEN pn = static_cast<HPEN>(SelectObject(dis->hDC, GetStockObject(NULL_PEN)));
            Ellipse(dis->hDC, cx - r, cy - r, cx + r, cy + r);
            SelectObject(dis->hDC, pn);
            SelectObject(dis->hDC, old);
            DeleteObject(b);
            return TRUE;
        }
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        KeyTone tone = KeyTone::Neutral;
        switch (dis->CtlID) {
        case 101:
        case 500: tone = KeyTone::Accent; break;
        case 212: tone = KeyTone::Fault; break;
        case 206:
        case 213:
        case 402:
        case 405: tone = KeyTone::Quiet; break;
        default: break;
        }
        DrawKey(dis, text, tone);
        return TRUE;
    }
    case WM_APP_TESTLINE: {
        std::vector<std::string> drain;
        {
            std::lock_guard lk(g_lineMtx);
            drain.swap(g_lines);
        }
        for (auto& l : drain) AppendLog(l);
        if (wp == 1) { // suite finished on the worker thread
            if (g_testThread.joinable()) g_testThread.join(); // must join before reuse
            g_testRunning.store(false);
            EnableWindow(g_btnSelfTest.wnd, TRUE);
            RefreshStatus();
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        switch (id) {
        case 101: Post("/attach", "{\"profile\":\"" + CurrentProfile() + "\"}", "attach"); break;
        case 102: Post("/detach", "", "detach"); break;
        case 500: {
            if (g_testRunning.exchange(true)) break;
            EnableWindow(g_btnSelfTest.wnd, FALSE);
            AppendLog("==== starting self-test suite (worker thread) ====");
            g_testThread = std::thread([h] {
                const int failed = uacstudio::RunAutoTestSuite(g_port, [h](const std::string& line) {
                    {
                        std::lock_guard lk(g_lineMtx);
                        g_lines.push_back(line);
                    }
                    PostMessageA(h, WM_APP_TESTLINE, 0, 0);
                });
                {
                    std::lock_guard lk(g_lineMtx);
                    g_lines.push_back(failed == 0 ? "SELF-TEST RESULT: ALL PASS"
                                                  : "SELF-TEST RESULT: " +
                                                        std::to_string(failed) + " FAILED");
                }
                PostMessageA(h, WM_APP_TESTLINE, 1, 0); // 1 = finished
            });
            break;
        }
        case 910:
            SetEnabled(false);
            uacstudio::ShowEnvCheck(h, g_port);
            SetEnabled(true);
            RefreshStatus();
            break;
        case 200: Post("/event", "{\"type\":\"volume_up\"}", "音量+"); break;
        case 201: Post("/event", "{\"type\":\"volume_down\"}", "音量-"); break;
        case 202: Post("/event", "{\"type\":\"mute\"}", "静音"); break;
        case 203: Post("/event", "{\"type\":\"hid_vol_up\"}", "HID音量+"); break;
        case 204: Post("/event", "{\"type\":\"hid_vol_down\"}", "HID音量-"); break;
        case 205: Post("/event", "{\"type\":\"hid_mute\"}", "HID静音"); break;
        case 206: Post("/event", "{\"type\":\"hid_release\"}", "HID释放"); break;
        case 210:
            Post("/inject", "{\"kind\":\"stall\",\"bRequest\":129,\"nth\":1}", "STALL");
            break;
        case 211:
            Post("/inject", "{\"kind\":\"delay_ms\",\"bRequest\":6,\"ms\":200}", "延迟");
            break;
        case 212: Post("/inject", "{\"kind\":\"disconnect\",\"afterMs\":0}", "拔线"); break;
        case 213: Post("/inject", "{\"kind\":\"clear\"}", "清注入"); break;
        case 401: {
            wchar_t hz[32]{};
            GetWindowTextW(g_hzEdit.wnd, hz, 32);
            char a[32]{};
            WideCharToMultiByte(CP_UTF8, 0, hz, -1, a, 32, nullptr, nullptr);
            Post("/source", std::string("{\"kind\":\"sine\",\"hz\":") + a + "}", "正弦");
            break;
        }
        case 402: Post("/source", "{\"kind\":\"silence\"}", "静音源"); break;
        case 404: {
            wchar_t p[MAX_PATH]{};
            GetWindowTextW(g_capEdit.wnd, p, MAX_PATH);
            char a[MAX_PATH]{};
            WideCharToMultiByte(CP_UTF8, 0, p, -1, a, MAX_PATH, nullptr, nullptr);
            Post("/capture", std::string("{\"path\":\"") + uac::JsonEscape(a) +
                           "\"}", "录音");
            break;
        }
        case 405: Post("/capture", "{\"path\":\"\"}", "录音停"); break;
        default: break;
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        auto dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_brBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        const int m = Scale(h, 12);
        RECT bar{m, m, rc.right - m, m + Scale(h, 44)};
        FillRect(dc, &bar, g_brPanel); // 顶部设备状态条底板（唯一面板化元素）
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, 1);
        KillTimer(h, 2);
        if (g_testThread.joinable()) g_testThread.join(); // no detached workers at exit
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // 自绘背景（WM_ERASEBKGND）
    wc.lpszClassName = L"UacStudio";
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, L"UacStudio", L"uacstudio - UAC 仿真测试台", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 880, 640, nullptr, nullptr, hi,
                             nullptr);
    ShowWindow(g_main, nCmdShow);
    UpdateWindow(g_main);
    if (uacstudio::IsFirstRun()) {
        uacstudio::ShowEnvCheck(g_main, g_port);
        RefreshStatus();
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    return static_cast<int>(msg.wParam);
}
