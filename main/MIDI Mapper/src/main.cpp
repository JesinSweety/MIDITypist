#include <windows.h>
#include <CommCtrl.h>
#include <Psapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <algorithm>
#include "RtMidi.h"
#include "json.hpp"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_MICA_EFFECT
#define DWMWA_MICA_EFFECT 1029
#endif

// ── Control IDs ──
#define IDC_CB_PORTS 101
#define IDC_BTN_CONNECT 102
#define IDC_BTN_LEARN 104
#define IDC_BTN_SETTINGS 150
#define IDC_LOGBOX 103
#define IDC_LIST_MAPPINGS 106
#define IDC_STATUSBAR 108
#define IDD_SETTINGS 400
#define IDC_BTN_SAVEPROFILE 120
#define IDC_BTN_LOADPROFILE 121
#define IDC_BTN_BINDPROFILE 130
#define IDC_BTN_CLEARLOG 105
#define IDC_BTN_CLEARMAPS 107
#define IDC_BTN_ABOUT 109

// ── Timers & Tray ──
#define RECONNECT_TIMER_ID 502
#define RECONNECT_INTERVAL 3000
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_SHOW 4001
#define ID_TRAY_EXIT 4002

// ── Piano Roll ──
#define IDC_PIANO_ROLL 160
#define PIANO_ROLL_HEIGHT 60
#define PIANO_TOTAL_KEYS 128
#define PIANO_DECAY_TIMER 503
#define PIANO_DECAY_MS 50

// ── Colors ──
// ── Rich Aesthetics (Glassmorphism) ──
Color CLR_GLASS_BG(200, 15, 15, 18);        // Semi-transparent base
Color CLR_GLASS_CARD(40, 255, 255, 255);    // Very thin glass
Color CLR_GLASS_BORDER(60, 255, 255, 255);  // Subtle glass edge
Color CLR_ACCENT(255, 0, 120, 212);         // Bright blue
Color CLR_ACCENT_HOVER(255, 30, 150, 240);  // Hover blue
Color CLR_TEXT(255, 245, 245, 250);         // Text color
Color CLR_TEXT_DIM(180, 200, 200, 210);     // Dimmed text

ULONG_PTR g_gdiplusToken;
int g_dpi = 96;

int Scale(int p) { return MulDiv(p, g_dpi, 96); }

COLORREF ColorToRGB(Color c) {
    return RGB(c.GetR(), c.GetG(), c.GetB());
}

using json = nlohmann::json;

// ── Global State ──
HINSTANCE g_hInst;
HFONT g_hFontTitle = nullptr;
HFONT g_hFontNormal = nullptr;
HWND g_hwndPorts, g_hwndButton, g_hwndLog, g_hwndLearn, g_hwndMappingList,
    g_hwndStatus, g_hwndSettings, g_hwndPianoRoll;
HWND g_hwndMain = nullptr;
std::unique_ptr<RtMidiIn> g_midiIn;
std::vector<std::string> g_ports;
bool g_connected = false;
int g_lastConnectedPort = -1;
std::string g_lastConnectedPortName;

// ── Mapping struct with modifiers and velocity ──
struct Mapping {
    int midi_type;      // 0=Note, 1=CC
    int midi_num;
    int key_vk;
    int modifiers;      // bitmask: 1=Ctrl, 2=Shift, 4=Alt
    int vel_min;        // velocity threshold (0-127, default 1)
    int vel_zone;       // 0=any, 1=soft(1-63), 2=hard(64-127)
    int cc_action;      // 0=keypress, 1=mouse_x, 2=mouse_y, 3=scroll, 4=hold_key
    int profile_switch; // -1=normal, 0+=profile slot index to switch to
};

std::vector<Mapping> g_mappings;
std::mutex g_mappingsMutex;
bool g_learning = false;
Mapping g_learn_pending = { -1, -1, -1, 0, 1, 0, 0, -1 };

// ── Per-App Profile ──
std::map<std::wstring, std::wstring> g_appProfileBindings;
std::wstring g_lastProfileLoadedForApp;
HWINEVENTHOOK g_hWinEventHook = nullptr;

// ── Tray Icon ──
NOTIFYICONDATA g_nid = {};
bool g_minimizedToTray = false;

// ── Persistent Config ──
std::wstring g_configPath;
std::wstring g_lastProfilePath;

// ── Piano Roll State ──
int g_pianoVelocity[PIANO_TOTAL_KEYS] = { 0 };
int g_pianoCC[128] = { 0 };

// ── Auto Reconnect ──
bool g_autoReconnect = true;

// ── Profile Slots for MIDI switching ──
std::vector<std::wstring> g_profileSlots;

// ── CC Hold State ──
std::map<int, bool> g_ccHoldActive;

// ══════════════════════════════════════════
//  Utility Functions
// ══════════════════════════════════════════

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], len);
    return wstr;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], len, nullptr, nullptr);
    return str;
}

std::wstring GetKeyName(int vk) {
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    long param = (sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
        vk == VK_NEXT || vk == VK_PRIOR || vk == VK_LEFT || vk == VK_RIGHT ||
        vk == VK_UP || vk == VK_DOWN || vk == VK_DIVIDE) {
        param |= (1 << 24);
    }
    wchar_t name[64] = { 0 };
    if (GetKeyNameText(param, name, 64) > 0) return name;
    std::wstringstream ws;
    ws << L"VK_" << vk;
    return ws.str();
}

std::wstring GetModifierString(int mods) {
    std::wstring s;
    if (mods & 1) s += L"Ctrl+";
    if (mods & 2) s += L"Shift+";
    if (mods & 4) s += L"Alt+";
    return s;
}

std::wstring GetConfigDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\") + 1);
}

// ══════════════════════════════════════════
//  UI Helpers
// ══════════════════════════════════════════

void SetUIFont(HWND hwnd, bool isTitle = false) {
    if (!g_hFontNormal) {
        LOGFONT lf = { 0 };
        lf.lfHeight = -Scale(16);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Display");
        g_hFontNormal = CreateFontIndirect(&lf);

        lf.lfHeight = -Scale(22);
        lf.lfWeight = FW_BOLD;
        g_hFontTitle = CreateFontIndirect(&lf);
    }
    SendMessage(hwnd, WM_SETFONT, (WPARAM)(isTitle ? g_hFontTitle : g_hFontNormal), TRUE);
}

void ApplyModernStyle(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    
    // Mica Alt (Windows 11)
    int mica = 2; 
    DwmSetWindowAttribute(hwnd, DWMWA_MICA_EFFECT, &mica, sizeof(mica));

    // Extend frame for total glass effect
    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
}

void DrawGlassCard(Graphics& g, Rect r, const wchar_t* title) {
    int radius = Scale(16);
    GraphicsPath path;
    path.AddArc(r.X, r.Y, radius, radius, 180, 90);
    path.AddArc(r.X + r.Width - radius, r.Y, radius, radius, 270, 90);
    path.AddArc(r.X + r.Width - radius, r.Y + r.Height - radius, radius, radius, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - radius, radius, radius, 90, 90);
    path.CloseFigure();

    // Translucent fill
    SolidBrush cardBrush(CLR_GLASS_CARD);
    g.FillPath(&cardBrush, &path);

    // Subtle edge highlight
    Pen glassPen(CLR_GLASS_BORDER, 1.2f);
    g.DrawPath(&glassPen, &path);

    if (title) {
        Font font(L"Segoe UI Variable Display", (REAL)Scale(10), FontStyleBold);
        SolidBrush textBrush(CLR_TEXT_DIM);
        g.DrawString(title, -1, &font, PointF((REAL)r.X + 18, (REAL)r.Y + 12), &textBrush);
    }
}

void DrawRoundButton(LPDRAWITEMSTRUCT pDIS, Color baseColor) {
    Graphics g(pDIS->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    Rect r(0, 0, pDIS->rcItem.right, pDIS->rcItem.bottom);
    int radius = Scale(10);

    GraphicsPath path;
    path.AddArc(r.X, r.Y, radius, radius, 180, 90);
    path.AddArc(r.X + r.Width - radius, r.Y, radius, radius, 270, 90);
    path.AddArc(r.X + r.Width - radius, r.Y + r.Height - radius, radius, radius, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - radius, radius, radius, 90, 90);
    path.CloseFigure();

    // Linear Gradient
    LinearGradientBrush brush(r, baseColor, Color(255, 
        (int)(baseColor.GetR() * 0.7), (int)(baseColor.GetG() * 0.7), (int)(baseColor.GetB() * 0.7)), 
        LinearGradientModeVertical);
    g.FillPath(&brush, &path);

    // Inner glow
    Pen shinePen(Color(120, 255, 255, 255), 1.0f);
    g.DrawPath(&shinePen, &path);

    wchar_t text[64];
    GetWindowText(pDIS->hwndItem, text, 64);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush textBrush(Color(255, 255, 255, 255));
    Font font(pDIS->hDC);
    RectF rectF((REAL)r.X, (REAL)r.Y, (REAL)r.Width, (REAL)r.Height);
    g.DrawString(text, -1, &font, rectF, &format, &textBrush);
}

void LayoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    int pad = Scale(24);
    int topH = Scale(70);
    int pianoH = Scale(PIANO_ROLL_HEIGHT);
    int statusH = Scale(24);
    int bodyH = ch - topH - pianoH - statusH - (pad * 2);
    int halfW = (cw - (pad * 3)) / 2;

    int btnY = Scale(25);
    MoveWindow(g_hwndPorts, pad, btnY, Scale(220), Scale(34), TRUE);
    MoveWindow(g_hwndButton, pad + Scale(230), btnY, Scale(120), Scale(34), TRUE);
    MoveWindow(g_hwndLearn, pad + Scale(360), btnY, Scale(150), Scale(34), TRUE);
    MoveWindow(g_hwndSettings, pad + Scale(520), btnY, Scale(120), Scale(34), TRUE);

    // Offset edit/listbox slightly to sit inside the glassy card
    MoveWindow(g_hwndLog, pad + Scale(8), topH + pad + Scale(32), halfW - Scale(16), bodyH - Scale(42), TRUE);
    MoveWindow(g_hwndMappingList, (pad * 2) + halfW + Scale(8), topH + pad + Scale(32), halfW - Scale(16), bodyH - Scale(42), TRUE);
    MoveWindow(g_hwndPianoRoll, pad, ch - pianoH - statusH - pad, cw - (pad * 2), pianoH, TRUE);
    if (g_hwndStatus) SendMessage(g_hwndStatus, WM_SIZE, 0, 0);
}

void AddLog(const std::string& msg) {
    std::string line = msg + "\r\n";
    int len = GetWindowTextLength(g_hwndLog);
    SendMessage(g_hwndLog, EM_SETSEL, len, len);
    SendMessageA(g_hwndLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessage(g_hwndLog, EM_SCROLLCARET, 0, 0);
}

void SetStatus(const wchar_t* status) {
    SendMessage(g_hwndStatus, SB_SETTEXT, 0, (LPARAM)status);
}

// ══════════════════════════════════════════
//  Key Simulation (with modifiers)
// ══════════════════════════════════════════

void SimulateKeyCombo(int vk, int modifiers) {
    std::vector<INPUT> inputs;
    // Press modifiers
    if (modifiers & 1) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_CONTROL; inputs.push_back(i); }
    if (modifiers & 2) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_SHIFT; inputs.push_back(i); }
    if (modifiers & 4) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_MENU; inputs.push_back(i); }
    // Press key via scancode
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    INPUT kd = {}; kd.type = INPUT_KEYBOARD; kd.ki.wScan = sc; kd.ki.dwFlags = KEYEVENTF_SCANCODE;
    inputs.push_back(kd);
    INPUT ku = kd; ku.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    inputs.push_back(ku);
    // Release modifiers (reverse order)
    if (modifiers & 4) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_MENU; i.ki.dwFlags = KEYEVENTF_KEYUP; inputs.push_back(i); }
    if (modifiers & 2) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_SHIFT; i.ki.dwFlags = KEYEVENTF_KEYUP; inputs.push_back(i); }
    if (modifiers & 1) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_CONTROL; i.ki.dwFlags = KEYEVENTF_KEYUP; inputs.push_back(i); }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

void SimulateHoldKey(int vk, bool down) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    input.ki.wScan = sc;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
}

void SimulateMouseMove(int dx, int dy) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void SimulateScroll(int amount) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = amount;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &input, sizeof(INPUT));
}

// ══════════════════════════════════════════
//  Mapping List & Persistence
// ══════════════════════════════════════════

void UpdateMappingList() {
    SendMessage(g_hwndMappingList, LB_RESETCONTENT, 0, 0);
    std::lock_guard<std::mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        std::wstringstream ws;
        if (m.profile_switch >= 0) {
            ws << (m.midi_type == 0 ? L"Note " : L"CC ") << m.midi_num << L"  ->  [Profile #" << m.profile_switch << L"]";
        }
        else if (m.midi_type == 1 && m.cc_action > 0) {
            const wchar_t* actions[] = { L"", L"MouseX", L"MouseY", L"Scroll", L"HoldKey" };
            ws << L"CC " << m.midi_num << L"  ->  " << actions[m.cc_action];
            if (m.cc_action == 4) ws << L"(" << GetKeyName(m.key_vk) << L")";
        }
        else {
            ws << (m.midi_type == 0 ? L"Note " : L"CC ") << m.midi_num << L"  ->  ";
            ws << GetModifierString(m.modifiers) << GetKeyName(m.key_vk);
            if (m.vel_zone == 1) ws << L" [soft]";
            else if (m.vel_zone == 2) ws << L" [hard]";
        }
        SendMessage(g_hwndMappingList, LB_ADDSTRING, 0, (LPARAM)ws.str().c_str());
    }
}

void SaveMappings(const std::wstring& filename) {
    json j = json::array();
    {
        std::lock_guard<std::mutex> lock(g_mappingsMutex);
        for (const auto& m : g_mappings) {
            j.push_back({
                {"midi_type", m.midi_type}, {"midi_num", m.midi_num},
                {"key_vk", m.key_vk}, {"modifiers", m.modifiers},
                {"vel_min", m.vel_min}, {"vel_zone", m.vel_zone},
                {"cc_action", m.cc_action}, {"profile_switch", m.profile_switch}
            });
        }
    }
    std::ofstream f(filename);
    if (f) f << j.dump(4);
}

void LoadMappings(const std::wstring& filename) {
    std::ifstream f(filename);
    if (!f) return;
    json j;
    try { f >> j; } catch (...) { return; }
    {
        std::lock_guard<std::mutex> lock(g_mappingsMutex);
        g_mappings.clear();
        for (const auto& it : j) {
            Mapping m = {};
            m.midi_type = it.value("midi_type", 0);
            m.midi_num = it.value("midi_num", 0);
            m.key_vk = it.value("key_vk", 0);
            m.modifiers = it.value("modifiers", 0);
            m.vel_min = it.value("vel_min", 1);
            m.vel_zone = it.value("vel_zone", 0);
            m.cc_action = it.value("cc_action", 0);
            m.profile_switch = it.value("profile_switch", -1);
            g_mappings.push_back(m);
        }
    }
    g_lastProfilePath = filename;
    UpdateMappingList();
}

// ══════════════════════════════════════════
//  Persistent Config (auto-save/load)
// ══════════════════════════════════════════

void SaveConfig() {
    json cfg;
    cfg["last_port"] = g_lastConnectedPortName;
    cfg["last_profile"] = WideToUtf8(g_lastProfilePath);
    cfg["auto_reconnect"] = g_autoReconnect;
    // App bindings
    json bindings = json::object();
    for (auto& [exe, profile] : g_appProfileBindings)
        bindings[WideToUtf8(exe)] = WideToUtf8(profile);
    cfg["app_bindings"] = bindings;
    // Profile slots
    json slots = json::array();
    for (auto& s : g_profileSlots)
        slots.push_back(WideToUtf8(s));
    cfg["profile_slots"] = slots;

    std::ofstream f(g_configPath);
    if (f) f << cfg.dump(4);
}

void LoadConfig() {
    std::ifstream f(g_configPath);
    if (!f) return;
    json cfg;
    try { f >> cfg; } catch (...) { return; }

    g_lastConnectedPortName = cfg.value("last_port", "");
    g_lastProfilePath = Utf8ToWide(cfg.value("last_profile", ""));
    g_autoReconnect = cfg.value("auto_reconnect", true);

    if (cfg.contains("app_bindings") && cfg["app_bindings"].is_object()) {
        for (auto& [k, v] : cfg["app_bindings"].items())
            g_appProfileBindings[Utf8ToWide(k)] = Utf8ToWide(v.get<std::string>());
    }
    if (cfg.contains("profile_slots") && cfg["profile_slots"].is_array()) {
        g_profileSlots.clear();
        for (auto& s : cfg["profile_slots"])
            g_profileSlots.push_back(Utf8ToWide(s.get<std::string>()));
    }
}

// ══════════════════════════════════════════
//  System Tray
// ══════════════════════════════════════════

void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"MIDITypist");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

void MinimizeToTray(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
    g_minimizedToTray = true;
}

void RestoreFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    g_minimizedToTray = false;
}

// ══════════════════════════════════════════
//  Piano Roll Window
// ══════════════════════════════════════════

bool IsBlackKey(int note) {
    int n = note % 12;
    return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
}

LRESULT CALLBACK PianoRollProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        Bitmap buffer(w, h);
        Graphics* g = Graphics::FromImage(&buffer);
        g->SetSmoothingMode(SmoothingModeAntiAlias);

        // Neon Glow background behind keys
        SolidBrush bgBrush(Color(255, 10, 10, 12));
        g->FillRectangle(&bgBrush, 0, 0, w, h);

        float keyW = (float)w / PIANO_TOTAL_KEYS;
        for (int i = 0; i < PIANO_TOTAL_KEYS; i++) {
            float x1 = i * keyW;
            float x2 = (i + 1) * keyW;

            Color keyColor;
            if (g_pianoVelocity[i] > 0) {
                // Neon Glow effect
                int intensity = 150 + g_pianoVelocity[i];
                if (intensity > 255) intensity = 255;
                Color glowColor = Color(255, 0, intensity, 180);
                
                // Draw bloom glow using PathGradientBrush
                GraphicsPath glowPath;
                glowPath.AddEllipse((REAL)(x1 - Scale(10)), (REAL)(-Scale(10)), (REAL)((x2 - x1) + Scale(20)), (REAL)(h + Scale(20)));
                PathGradientBrush pgb(&glowPath);
                pgb.SetCenterColor(glowColor);
                Color surroundColors[] = { Color(0, 0, 0, 0) };
                int count = 1;
                pgb.SetSurroundColors(surroundColors, &count);
                g->FillPath(&pgb, &glowPath);

                keyColor = glowColor;
            }
            else {
                keyColor = IsBlackKey(i) ? Color(255, 30, 30, 32) : Color(255, 180, 180, 185);
            }

            SolidBrush kb(keyColor);
            g->FillRectangle(&kb, x1 + 1, 2.0f, x2 - x1 - 2, (REAL)h - 4);
        }

        Graphics graphics(hdc);
        graphics.DrawImage(&buffer, 0, 0);
        delete g;

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void InvalidatePianoRoll() {
    if (g_hwndPianoRoll) InvalidateRect(g_hwndPianoRoll, NULL, FALSE);
}

// ══════════════════════════════════════════
//  MIDI Callback
// ══════════════════════════════════════════

void midiCallback(double, std::vector<unsigned char>* msg, void*) {
    if (!msg || msg->empty()) return;
    int status = msg->at(0) & 0xF0;
    int number = (msg->size() > 1) ? msg->at(1) : -1;
    int velocity = (msg->size() > 2) ? msg->at(2) : 0;

    bool isNoteOn = (status == 0x90 && velocity > 0);
    bool isNoteOff = (status == 0x80 || (status == 0x90 && velocity == 0));
    bool isCC = (status == 0xB0);

    // Update Piano Roll
    if (isNoteOn && number >= 0 && number < 128) {
        g_pianoVelocity[number] = velocity;
        InvalidatePianoRoll();
    }
    else if (isNoteOff && number >= 0 && number < 128) {
        g_pianoVelocity[number] = 0;
        InvalidatePianoRoll();
    }
    if (isCC && number >= 0 && number < 128) {
        g_pianoCC[number] = velocity;
    }

    // Log only press/CC
    if (isNoteOn || isCC) {
        std::stringstream ss;
        ss << (isNoteOn ? "Note On: " : "CC: ") << number << " (v:" << velocity << ")";
        AddLog(ss.str());
    }

    // Learning mode
    if (g_learning && g_learn_pending.midi_type == -1) {
        if (isNoteOn) {
            g_learn_pending.midi_type = 0;
            g_learn_pending.midi_num = number;
            AddLog("Now press desired keyboard key to map...");
            SetStatus(L"Waiting for keyboard key...");
        }
        else if (isCC) {
            g_learn_pending.midi_type = 1;
            g_learn_pending.midi_num = number;
            AddLog("Now press desired keyboard key to map...");
            SetStatus(L"Waiting for keyboard key...");
        }
        return;
    }

    // Execute mappings
    std::lock_guard<std::mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        // --- Profile switching ---
        if (m.profile_switch >= 0) {
            if (m.midi_type == 0 && isNoteOn && number == m.midi_num) {
                if (m.profile_switch < (int)g_profileSlots.size()) {
                    PostMessage(g_hwndMain, WM_USER + 100, m.profile_switch, 0);
                }
            }
            continue;
        }

        // --- Note mapping ---
        if (m.midi_type == 0 && isNoteOn && number == m.midi_num) {
            if (velocity < m.vel_min) continue;
            if (m.vel_zone == 1 && velocity > 63) continue;
            if (m.vel_zone == 2 && velocity < 64) continue;
            SimulateKeyCombo(m.key_vk, m.modifiers);
        }

        // --- CC mapping ---
        if (m.midi_type == 1 && isCC && number == m.midi_num) {
            int val = velocity; // 0-127
            switch (m.cc_action) {
            case 0: // keypress
                SimulateKeyCombo(m.key_vk, m.modifiers);
                break;
            case 1: // mouse X
                SimulateMouseMove((val - 64) * 2, 0);
                break;
            case 2: // mouse Y
                SimulateMouseMove(0, (val - 64) * 2);
                break;
            case 3: // scroll
                SimulateScroll((val - 64) * 20);
                break;
            case 4: // hold key while above threshold
                if (val > 63 && !g_ccHoldActive[m.midi_num]) {
                    SimulateHoldKey(m.key_vk, true);
                    g_ccHoldActive[m.midi_num] = true;
                }
                else if (val <= 63 && g_ccHoldActive[m.midi_num]) {
                    SimulateHoldKey(m.key_vk, false);
                    g_ccHoldActive[m.midi_num] = false;
                }
                break;
            }
        }
    }
}

// ══════════════════════════════════════════
//  MIDI Port Management & Auto-Reconnect
// ══════════════════════════════════════════

void ScanMidiPorts(HWND hwnd) {
    g_ports.clear();
    RtMidiIn tempIn;
    int n = tempIn.getPortCount();
    SendMessage(g_hwndPorts, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; ++i) {
        std::string name = tempIn.getPortName(i);
        g_ports.push_back(name);
        SendMessage(g_hwndPorts, CB_ADDSTRING, 0, (LPARAM)Utf8ToWide(name).c_str());
    }
    if (n > 0) SendMessage(g_hwndPorts, CB_SETCURSEL, 0, 0);
}

void ConnectMidi(HWND hwnd) {
    int sel = (int)SendMessage(g_hwndPorts, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_ports.size()) return;
    try {
        g_midiIn = std::make_unique<RtMidiIn>();
        g_midiIn->openPort(sel);
        g_midiIn->setCallback(&midiCallback);
        g_connected = true;
        g_lastConnectedPort = sel;
        g_lastConnectedPortName = g_ports[sel];
        SetWindowText(g_hwndButton, L"Disconnect");
        AddLog("Connected to: " + g_ports[sel]);
        SetStatus(L"Connected.");
        SaveConfig();
    }
    catch (RtMidiError& e) {
        AddLog("Connection failed: " + std::string(e.getMessage()));
        g_midiIn.reset();
    }
}

void DisconnectMidi(HWND hwnd) {
    g_midiIn.reset();
    g_connected = false;
    SetWindowText(g_hwndButton, L"Connect");
    AddLog("MIDI disconnected.");
    SetStatus(L"Disconnected.");
}

void TryAutoReconnect() {
    if (g_connected || !g_autoReconnect || g_lastConnectedPortName.empty()) return;
    RtMidiIn tempIn;
    int n = tempIn.getPortCount();
    for (int i = 0; i < n; i++) {
        if (tempIn.getPortName(i) == g_lastConnectedPortName) {
            // Port is back, rescan and connect
            ScanMidiPorts(g_hwndMain);
            SendMessage(g_hwndPorts, CB_SETCURSEL, i, 0);
            ConnectMidi(g_hwndMain);
            if (g_connected) {
                AddLog("Auto-reconnected to: " + g_lastConnectedPortName);
                // Reload last profile
                if (!g_lastProfilePath.empty()) {
                    LoadMappings(g_lastProfilePath);
                    AddLog("Auto-loaded last profile.");
                }
            }
            return;
        }
    }
}

// ══════════════════════════════════════════
//  Other Helpers
// ══════════════════════════════════════════

void ShowAbout(HWND hwnd) {
    MessageBox(hwnd,
        L"MIDITypist v2.0\n"
        L"Modern MIDI-to-Keyboard Mapper\n\n"
        L"Features:\n"
        L"• Key combos (Ctrl/Shift/Alt)\n"
        L"• Velocity zones & thresholds\n"
        L"• CC-to-mouse/scroll mapping\n"
        L"• Per-app auto-switching\n"
        L"• System tray & auto-reconnect\n"
        L"• Visual MIDI Piano Roll\n\n"
        L"(C) 2025",
        L"About MIDITypist", MB_OK | MB_ICONINFORMATION);
}

void RemoveSelectedMapping() {
    int sel = (int)SendMessage(g_hwndMappingList, LB_GETCURSEL, 0, 0);
    {
        std::lock_guard<std::mutex> lock(g_mappingsMutex);
        if (sel >= 0 && sel < (int)g_mappings.size())
            g_mappings.erase(g_mappings.begin() + sel);
        else return;
    }
    AddLog("Mapping removed.");
    UpdateMappingList();
    SetStatus(L"Mapping removed.");
}

void PerAppSwitchMapping() {
    HWND hwndFg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwndFg, &pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProc) {
        wchar_t exeName[MAX_PATH] = { 0 };
        if (GetModuleFileNameExW(hProc, NULL, exeName, MAX_PATH)) {
            std::wstring exe(exeName);
            exe = exe.substr(exe.find_last_of(L"\\") + 1);
            auto it = g_appProfileBindings.find(exe);
            if (it != g_appProfileBindings.end() && g_lastProfileLoadedForApp != exe) {
                LoadMappings(it->second);
                AddLog("Switched mappings for app: " + WideToUtf8(exe));
                SetStatus((std::wstring(L"Auto-loaded for ") + exe).c_str());
                g_lastProfileLoadedForApp = exe;
            }
        }
        CloseHandle(hProc);
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) PerAppSwitchMapping();
}

// ══════════════════════════════════════════
//  Settings Dialog
// ══════════════════════════════════════════

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetUIFont(GetDlgItem(hDlg, IDC_BTN_SAVEPROFILE));
        SetUIFont(GetDlgItem(hDlg, IDC_BTN_LOADPROFILE));
        SetUIFont(GetDlgItem(hDlg, IDC_BTN_BINDPROFILE));
        SetUIFont(GetDlgItem(hDlg, IDC_BTN_CLEARLOG));
        SetUIFont(GetDlgItem(hDlg, IDC_BTN_CLEARMAPS));
        SetUIFont(GetDlgItem(hDlg, IDC_BTN_ABOUT));
        SetUIFont(GetDlgItem(hDlg, IDCANCEL));
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_SAVEPROFILE: {
            wchar_t szFile[MAX_PATH] = L"profile.json";
            OPENFILENAME ofn = { sizeof(ofn) };
            ofn.hwndOwner = hDlg;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Save MIDI Mapping Profile";
            ofn.lpstrDefExt = L"json";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
            if (GetSaveFileName(&ofn)) {
                SaveMappings(szFile);
                g_lastProfilePath = szFile;
                SaveConfig();
                AddLog("Profile saved.");
            }
            break;
        }
        case IDC_BTN_LOADPROFILE: {
            wchar_t szFile[MAX_PATH] = L"";
            OPENFILENAME ofn = { sizeof(ofn) };
            ofn.hwndOwner = hDlg;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Load MIDI Mapping Profile";
            ofn.lpstrDefExt = L"json";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
            if (GetOpenFileName(&ofn)) {
                LoadMappings(szFile);
                SaveConfig();
                AddLog("Profile loaded.");
            }
            break;
        }
        case IDC_BTN_BINDPROFILE: {
            wchar_t szProfile[MAX_PATH] = L"";
            OPENFILENAME ofn = { sizeof(ofn) };
            ofn.hwndOwner = hDlg;
            ofn.lpstrFile = szProfile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Choose Profile to Bind";
            ofn.lpstrDefExt = L"json";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
            if (!GetOpenFileName(&ofn)) break;

            wchar_t szExe[MAX_PATH] = L"";
            HWND hwndFg = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(hwndFg, &pid);
            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hProc) {
                if (GetModuleFileNameExW(hProc, NULL, szExe, MAX_PATH)) {
                    std::wstring exeName(szExe);
                    exeName = exeName.substr(exeName.find_last_of(L"\\") + 1);
                    g_appProfileBindings[exeName] = szProfile;
                    SaveConfig();
                    AddLog("App profile binding stored: " + WideToUtf8(exeName));
                }
                CloseHandle(hProc);
            }
            break;
        }
        case IDC_BTN_CLEARLOG:
            SetWindowTextA(g_hwndLog, "");
            AddLog("Log cleared.");
            break;
        case IDC_BTN_CLEARMAPS: {
            std::lock_guard<std::mutex> lock(g_mappingsMutex);
            g_mappings.clear();
        }
            UpdateMappingList();
            AddLog("All mappings cleared.");
            break;
        case IDC_BTN_ABOUT:
            ShowAbout(hDlg);
            break;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            break;
        }
        break;
    }
    return FALSE;
}

// ══════════════════════════════════════════
//  Main Window Procedure
// ══════════════════════════════════════════

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwndMain = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        ApplyModernStyle(hwnd);

        int btn_h = Scale(34);
        g_hwndPorts = CreateWindow(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            0, 0, 0, 0, hwnd, (HMENU)IDC_CB_PORTS, g_hInst, nullptr);
        SetUIFont(g_hwndPorts);

        g_hwndButton = CreateWindow(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_CONNECT, g_hInst, nullptr);
        SetUIFont(g_hwndButton);

        g_hwndLearn = CreateWindow(L"BUTTON", L"Learn Mapping", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_LEARN, g_hInst, nullptr);
        SetUIFont(g_hwndLearn);

        g_hwndSettings = CreateWindow(L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_SETTINGS, g_hInst, nullptr);
        SetUIFont(g_hwndSettings);

        g_hwndLog = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LOGBOX, g_hInst, nullptr);
        SetUIFont(g_hwndLog);

        g_hwndMappingList = CreateWindowEx(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_EXTENDEDSEL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LIST_MAPPINGS, g_hInst, nullptr);
        SetUIFont(g_hwndMappingList);

        // Register Piano Roll class
        WNDCLASS pianoWc = {};
        pianoWc.lpfnWndProc = PianoRollProc;
        pianoWc.hInstance = g_hInst;
        pianoWc.lpszClassName = L"MIDITypistPianoRoll";
        pianoWc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClass(&pianoWc);
        g_hwndPianoRoll = CreateWindow(L"MIDITypistPianoRoll", nullptr,
            WS_CHILD | WS_VISIBLE, 20, 420, 1100, PIANO_ROLL_HEIGHT,
            hwnd, (HMENU)IDC_PIANO_ROLL, g_hInst, nullptr);

        g_hwndStatus = CreateWindow(STATUSCLASSNAME, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, g_hInst, nullptr);

        ScanMidiPorts(hwnd);
        AddLog("MIDITypist v2.0 Ready.");
        SetStatus(L"Ready.");

        // Load persistent config
        g_configPath = GetConfigDir() + L"config.json";
        LoadConfig();

        // Auto-connect to last port
        if (!g_lastConnectedPortName.empty()) {
            for (int i = 0; i < (int)g_ports.size(); i++) {
                if (g_ports[i] == g_lastConnectedPortName) {
                    SendMessage(g_hwndPorts, CB_SETCURSEL, i, 0);
                    ConnectMidi(hwnd);
                    break;
                }
            }
        }
        // Auto-load last profile
        if (!g_lastProfilePath.empty()) {
            LoadMappings(g_lastProfilePath);
            AddLog("Auto-loaded last profile.");
        }
        UpdateMappingList();

        // Set up hooks and timers
        g_hwndMain = hwnd;
        g_hWinEventHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(hwnd, RECONNECT_TIMER_ID, RECONNECT_INTERVAL, NULL);
        SetTimer(hwnd, PIANO_DECAY_TIMER, PIANO_DECAY_MS, NULL);

        AddTrayIcon(hwnd);
        LayoutControls(hwnd);
        break;
    }
    case WM_SIZE:
        LayoutControls(hwnd);
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 700;
        mmi->ptMinTrackSize.y = 400;
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_CONNECT) {
            if (!g_connected) ConnectMidi(hwnd);
            else DisconnectMidi(hwnd);
        }
        else if (LOWORD(wParam) == IDC_BTN_LEARN) {
            g_learning = true;
            g_learn_pending = { -1, -1, -1, 0, 1, 0, 0, -1 };
            // Detect held modifiers at learn time
            if (GetKeyState(VK_CONTROL) & 0x8000) g_learn_pending.modifiers |= 1;
            if (GetKeyState(VK_SHIFT) & 0x8000) g_learn_pending.modifiers |= 2;
            if (GetKeyState(VK_MENU) & 0x8000) g_learn_pending.modifiers |= 4;
            AddLog("Learning: Press a MIDI note or CC, then a keyboard key. Hold Ctrl/Shift/Alt for combos.");
            SetStatus(L"Waiting for MIDI input...");
            SetFocus(hwnd);
        }
        else if (LOWORD(wParam) == IDC_BTN_SETTINGS) {
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_SETTINGS), hwnd, SettingsDlgProc);
        }
        else if (LOWORD(wParam) == ID_TRAY_SHOW) {
            RestoreFromTray(hwnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        break;
    case WM_KEYDOWN:
        if (g_learning && g_learn_pending.midi_type != -1) {
            // Ignore modifier-only keys
            if (wParam == VK_CONTROL || wParam == VK_SHIFT || wParam == VK_MENU ||
                wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
                wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
                wParam == VK_LMENU || wParam == VK_RMENU) break;

            g_learn_pending.key_vk = (int)wParam;
            // Capture current modifier state
            g_learn_pending.modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) g_learn_pending.modifiers |= 1;
            if (GetKeyState(VK_SHIFT) & 0x8000) g_learn_pending.modifiers |= 2;
            if (GetKeyState(VK_MENU) & 0x8000) g_learn_pending.modifiers |= 4;

            {
                std::lock_guard<std::mutex> lock(g_mappingsMutex);
                g_mappings.push_back(g_learn_pending);
            }
            std::wstringstream ws;
            ws << L"Mapped MIDI " << (g_learn_pending.midi_type == 0 ? L"Note" : L"CC")
                << L" " << g_learn_pending.midi_num
                << L" -> " << GetModifierString(g_learn_pending.modifiers) << GetKeyName(g_learn_pending.key_vk);
            AddLog(WideToUtf8(ws.str()));
            UpdateMappingList();
            SetStatus(L"Mapped and saved.");
            g_learning = false;
        }
        if (GetFocus() == g_hwndMappingList && wParam == VK_DELETE) {
            RemoveSelectedMapping();
        }
        break;
    case WM_TIMER:
        if (wParam == RECONNECT_TIMER_ID) {
            TryAutoReconnect();
        }
        else if (wParam == PIANO_DECAY_TIMER) {
            // Gentle decay for visual effect
            bool changed = false;
            for (int i = 0; i < PIANO_TOTAL_KEYS; i++) {
                if (g_pianoVelocity[i] > 0) {
                    int decayed = g_pianoVelocity[i] - 8;
                    g_pianoVelocity[i] = (decayed > 0) ? decayed : 0;
                    changed = true;
                }
            }
            if (changed) InvalidatePianoRoll();
        }
        break;
    case WM_USER + 100: {
        // Profile switch from MIDI callback
        int slot = (int)wParam;
        if (slot >= 0 && slot < (int)g_profileSlots.size()) {
            LoadMappings(g_profileSlots[slot]);
            AddLog("Switched to profile slot #" + std::to_string(slot));
            SetStatus(L"Profile switched via MIDI.");
        }
        break;
    }
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            RestoreFromTray(hwnd);
        }
        else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, ID_TRAY_SHOW, L"Show MIDITypist");
            AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;

        Bitmap buffer(cw, ch);
        Graphics* g = Graphics::FromImage(&buffer);
        g->SetSmoothingMode(SmoothingModeAntiAlias);
        g->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // 1. Semi-transparent wash for Acrylic/Mica context
        SolidBrush bgBrush(CLR_GLASS_BG);
        g->FillRectangle(&bgBrush, 0, 0, cw, ch);

        // 2. Premium Title
        Font titleFont(L"Segoe UI Variable Display", (REAL)Scale(24), FontStyleBold);
        SolidBrush textBrush(CLR_TEXT);
        g->DrawString(L"MIDITypist", -1, &titleFont, PointF((REAL)Scale(24), (REAL)Scale(20)), &textBrush);

        // 3. Floating Cards
        int pad = Scale(24);
        int topH = Scale(70);
        int pianoH = Scale(PIANO_ROLL_HEIGHT);
        int statusH = Scale(24);
        int bodyH = ch - topH - pianoH - statusH - (pad * 2);
        int halfW = (cw - (pad * 3)) / 2;

        DrawGlassCard(*g, Rect(pad, topH + pad, halfW, bodyH), L"SYSTEM LOG");
        DrawGlassCard(*g, Rect((pad * 2) + halfW, topH + pad, halfW, bodyH), L"KEY MAPPINGS");

        Graphics graphics(hdc);
        graphics.DrawImage(&buffer, 0, 0);
        delete g;
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlType == ODT_BUTTON) {
            BOOL pressed = (pDIS->itemState & ODS_SELECTED);
            DrawRoundButton(pDIS, pressed ? CLR_ACCENT_HOVER : CLR_ACCENT);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ColorToRGB(CLR_TEXT));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ColorToRGB(CLR_TEXT));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        RECT* prcNew = (RECT*)lParam;
        SetWindowPos(hwnd, NULL, prcNew->left, prcNew->top, 
            prcNew->right - prcNew->left, prcNew->bottom - prcNew->top, 
            SWP_NOZORDER | SWP_NOACTIVATE);
        // Regenerate fonts on DPI change
        if (g_hFontNormal) { DeleteObject(g_hFontNormal); g_hFontNormal = nullptr; }
        if (g_hFontTitle) { DeleteObject(g_hFontTitle); g_hFontTitle = nullptr; }
        SetUIFont(g_hwndPorts);
        SetUIFont(g_hwndButton);
        SetUIFont(g_hwndLearn);
        SetUIFont(g_hwndSettings);
        SetUIFont(g_hwndLog);
        SetUIFont(g_hwndMappingList);
        LayoutControls(hwnd);
        break;
    }
    case WM_CLOSE:
        MinimizeToTray(hwnd);
        return 0;
    case WM_DESTROY:
        SaveConfig();
        KillTimer(hwnd, RECONNECT_TIMER_ID);
        KillTimer(hwnd, PIANO_DECAY_TIMER);
        if (g_hWinEventHook) { UnhookWinEvent(g_hWinEventHook); g_hWinEventHook = nullptr; }
        RemoveTrayIcon();
        if (g_hFontNormal) DeleteObject(g_hFontNormal);
        if (g_hFontTitle) DeleteObject(g_hFontTitle);
        GdiplusShutdown(g_gdiplusToken);
        g_midiIn.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ══════════════════════════════════════════
//  Entry Point
// ══════════════════════════════════════════

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    g_hInst = hInstance;
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"MIDITypist";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"MIDITypist",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, Scale(1140), Scale(600),
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
