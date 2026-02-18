#include <windows.h>
#include <CommCtrl.h>
#include <Psapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <algorithm>
#include <functional>
#include "RtMidi.h"
#include "json.hpp"

// WebView2
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Shlwapi.lib")

using namespace Microsoft::WRL;
using json = nlohmann::json;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_MICA_EFFECT
#define DWMWA_MICA_EFFECT 1029
#endif

// ── Timers & Tray ──
#define RECONNECT_TIMER_ID 502
#define RECONNECT_INTERVAL 3000
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_SHOW 4001
#define ID_TRAY_EXIT 4002

// ── Piano Roll ──
#define PIANO_TOTAL_KEYS 128
#define PIANO_DECAY_TIMER 503
#define PIANO_DECAY_MS 50

// ── Global State ──
HINSTANCE g_hInst;
HWND g_hwndMain = nullptr;
wil::com_ptr<ICoreWebView2> g_webview;
wil::com_ptr<ICoreWebView2Controller> g_controller;

std::unique_ptr<RtMidiIn> g_midiIn;
std::vector<std::string> g_ports;
bool g_connected = false;
int g_lastConnectedPort = -1;
std::string g_lastConnectedPortName;

// ── Mapping struct ──
struct Mapping {
    int midi_type;      // 0=Note, 1=CC
    int midi_num;
    int key_vk;
    int modifiers;      // bitmask: 1=Ctrl, 2=Shift, 4=Alt
    int vel_min;
    int vel_zone;       // 0=any, 1=soft(1-63), 2=hard(64-127)
    int cc_action;      // 0=keypress, 1=mouse_x, 2=mouse_y, 3=scroll, 4=hold_key
    int profile_switch; // -1=normal, 0+=profile slot index
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
//  WebView2 Message Bridge
// ══════════════════════════════════════════

void PostToWebView(const json& msg) {
    if (!g_webview) return;
    std::string s = msg.dump();
    std::wstring ws = Utf8ToWide(s);
    g_webview->PostWebMessageAsJson(ws.c_str());
}

void SendLog(const std::string& text, const std::string& category = "system") {
    PostToWebView({ {"type", "log"}, {"text", text}, {"category", category} });
}

void SendStatus(const std::string& text) {
    PostToWebView({ {"type", "status"}, {"text", text} });
}

void SendMappingsToUI() {
    json arr = json::array();
    std::lock_guard<std::mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        std::wstring targetDisplay;
        if (m.profile_switch >= 0) {
            targetDisplay = L"[Profile #" + std::to_wstring(m.profile_switch) + L"]";
        } else if (m.midi_type == 1 && m.cc_action > 0) {
            const wchar_t* actions[] = { L"", L"MouseX", L"MouseY", L"Scroll", L"HoldKey" };
            targetDisplay = actions[m.cc_action];
            if (m.cc_action == 4) targetDisplay += L"(" + GetKeyName(m.key_vk) + L")";
        } else {
            targetDisplay = GetModifierString(m.modifiers) + GetKeyName(m.key_vk);
        }

        arr.push_back({
            {"midi_type", m.midi_type}, {"midi_num", m.midi_num},
            {"key_vk", m.key_vk}, {"modifiers", m.modifiers},
            {"vel_min", m.vel_min}, {"vel_zone", m.vel_zone},
            {"cc_action", m.cc_action}, {"profile_switch", m.profile_switch},
            {"target_display", WideToUtf8(targetDisplay)}
        });
    }
    PostToWebView({ {"type", "mappings"}, {"mappings", arr} });
}

// ══════════════════════════════════════════
//  Key Simulation (with modifiers)
// ══════════════════════════════════════════

void SimulateKeyCombo(int vk, int modifiers) {
    std::vector<INPUT> inputs;
    if (modifiers & 1) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_CONTROL; inputs.push_back(i); }
    if (modifiers & 2) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_SHIFT; inputs.push_back(i); }
    if (modifiers & 4) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = VK_MENU; inputs.push_back(i); }
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    INPUT kd = {}; kd.type = INPUT_KEYBOARD; kd.ki.wScan = sc; kd.ki.dwFlags = KEYEVENTF_SCANCODE;
    inputs.push_back(kd);
    INPUT ku = kd; ku.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    inputs.push_back(ku);
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
//  Mapping Persistence
// ══════════════════════════════════════════

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
    SendMappingsToUI();
}

// ══════════════════════════════════════════
//  Persistent Config
// ══════════════════════════════════════════

void SaveConfig() {
    json cfg;
    cfg["last_port"] = g_lastConnectedPortName;
    cfg["last_profile"] = WideToUtf8(g_lastProfilePath);
    cfg["auto_reconnect"] = g_autoReconnect;
    json bindings = json::object();
    for (auto& [exe, profile] : g_appProfileBindings)
        bindings[WideToUtf8(exe)] = WideToUtf8(profile);
    cfg["app_bindings"] = bindings;
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
        PostToWebView({ {"type", "midi_note"}, {"note", number}, {"velocity", velocity} });
    }
    else if (isNoteOff && number >= 0 && number < 128) {
        g_pianoVelocity[number] = 0;
    }
    if (isCC && number >= 0 && number < 128) {
        g_pianoCC[number] = velocity;
        PostToWebView({ {"type", "midi_cc"}, {"cc", number}, {"value", velocity} });
    }

    // Learning mode
    if (g_learning && g_learn_pending.midi_type == -1) {
        if (isNoteOn) {
            g_learn_pending.midi_type = 0;
            g_learn_pending.midi_num = number;
            PostToWebView({ {"type", "learn_phase"}, {"phase", 2}, {"text", "Now press a keyboard key..."} });
            SendStatus("Waiting for keyboard key...");
        }
        else if (isCC) {
            g_learn_pending.midi_type = 1;
            g_learn_pending.midi_num = number;
            PostToWebView({ {"type", "learn_phase"}, {"phase", 2}, {"text", "Now press a keyboard key..."} });
            SendStatus("Waiting for keyboard key...");
        }
        return;
    }

    // Execute mappings
    std::lock_guard<std::mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        if (m.profile_switch >= 0) {
            if (m.midi_type == 0 && isNoteOn && number == m.midi_num) {
                if (m.profile_switch < (int)g_profileSlots.size()) {
                    PostMessage(g_hwndMain, WM_USER + 100, m.profile_switch, 0);
                }
            }
            continue;
        }

        if (m.midi_type == 0 && isNoteOn && number == m.midi_num) {
            if (velocity < m.vel_min) continue;
            if (m.vel_zone == 1 && velocity > 63) continue;
            if (m.vel_zone == 2 && velocity < 64) continue;
            SimulateKeyCombo(m.key_vk, m.modifiers);
        }

        if (m.midi_type == 1 && isCC && number == m.midi_num) {
            int val = velocity;
            switch (m.cc_action) {
            case 0: SimulateKeyCombo(m.key_vk, m.modifiers); break;
            case 1: SimulateMouseMove((val - 64) * 2, 0); break;
            case 2: SimulateMouseMove(0, (val - 64) * 2); break;
            case 3: SimulateScroll((val - 64) * 20); break;
            case 4:
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

void ScanMidiPorts() {
    g_ports.clear();
    RtMidiIn tempIn;
    int n = tempIn.getPortCount();
    json portsArr = json::array();
    for (int i = 0; i < n; ++i) {
        std::string name = tempIn.getPortName(i);
        g_ports.push_back(name);
        portsArr.push_back(name);
    }
    PostToWebView({ {"type", "ports"}, {"ports", portsArr} });
}

void ConnectMidi(int portIndex) {
    if (portIndex < 0 || portIndex >= (int)g_ports.size()) return;
    try {
        g_midiIn = std::make_unique<RtMidiIn>();
        g_midiIn->openPort(portIndex);
        g_midiIn->setCallback(&midiCallback);
        g_connected = true;
        g_lastConnectedPort = portIndex;
        g_lastConnectedPortName = g_ports[portIndex];
        PostToWebView({ {"type", "connected"}, {"portName", g_ports[portIndex]} });
        SendLog("Connected to: " + g_ports[portIndex]);
        SendStatus("Connected.");
        SaveConfig();
    }
    catch (RtMidiError& e) {
        SendLog("Connection failed: " + std::string(e.getMessage()));
        g_midiIn.reset();
    }
}

void DisconnectMidi() {
    g_midiIn.reset();
    g_connected = false;
    PostToWebView({ {"type", "disconnected"} });
    SendLog("MIDI disconnected.");
    SendStatus("Disconnected.");
}

void TryAutoReconnect() {
    if (g_connected || !g_autoReconnect || g_lastConnectedPortName.empty()) return;
    RtMidiIn tempIn;
    int n = tempIn.getPortCount();
    for (int i = 0; i < n; i++) {
        if (tempIn.getPortName(i) == g_lastConnectedPortName) {
            ScanMidiPorts();
            ConnectMidi(i);
            if (g_connected) {
                SendLog("Auto-reconnected to: " + g_lastConnectedPortName);
                if (!g_lastProfilePath.empty()) {
                    LoadMappings(g_lastProfilePath);
                    SendLog("Auto-loaded last profile.");
                }
            }
            return;
        }
    }
}

// ══════════════════════════════════════════
//  Per-App Switching
// ══════════════════════════════════════════

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProc) {
        wchar_t exeName[MAX_PATH] = { 0 };
        if (GetModuleFileNameExW(hProc, NULL, exeName, MAX_PATH)) {
            std::wstring exe(exeName);
            exe = exe.substr(exe.find_last_of(L"\\") + 1);
            auto it = g_appProfileBindings.find(exe);
            if (it != g_appProfileBindings.end() && it->second != g_lastProfileLoadedForApp) {
                g_lastProfileLoadedForApp = it->second;
                LoadMappings(it->second);
                SendLog("Auto-switched profile for: " + WideToUtf8(exe));
            }
        }
        CloseHandle(hProc);
    }
}

// ══════════════════════════════════════════
//  Handle messages from WebView2 (JS -> C++)
// ══════════════════════════════════════════

void HandleWebMessage(const std::string& messageStr) {
    json msg;
    try { msg = json::parse(messageStr); } catch (...) { return; }

    std::string action = msg.value("action", "");

    if (action == "init") {
        ScanMidiPorts();
        SendMappingsToUI();
        SendStatus("Ready");
        // Auto-connect to last port
        if (!g_lastConnectedPortName.empty()) {
            for (int i = 0; i < (int)g_ports.size(); i++) {
                if (g_ports[i] == g_lastConnectedPortName) {
                    ConnectMidi(i);
                    break;
                }
            }
        }
        if (!g_lastProfilePath.empty()) {
            LoadMappings(g_lastProfilePath);
        }
    }
    else if (action == "toggle_connect") {
        if (!g_connected) {
            int port = msg.value("port", 0);
            ConnectMidi(port);
        } else {
            DisconnectMidi();
        }
    }
    else if (action == "start_learn") {
        g_learning = true;
        g_learn_pending = { -1, -1, -1, 0, 1, 0, 0, -1 };
        SendLog("Learning: Press a MIDI note or CC, then a keyboard key.");
        SendStatus("Waiting for MIDI input...");
    }
    else if (action == "cancel_learn") {
        g_learning = false;
        g_learn_pending = { -1, -1, -1, 0, 1, 0, 0, -1 };
        SendStatus("Learning cancelled.");
    }
    else if (action == "learn_key") {
        if (g_learning && g_learn_pending.midi_type != -1) {
            int keyCode = msg.value("keyCode", 0);
            if (keyCode == 0) return;

            g_learn_pending.key_vk = keyCode;
            g_learn_pending.modifiers = 0;
            if (msg.value("ctrlKey", false)) g_learn_pending.modifiers |= 1;
            if (msg.value("shiftKey", false)) g_learn_pending.modifiers |= 2;
            if (msg.value("altKey", false)) g_learn_pending.modifiers |= 4;

            {
                std::lock_guard<std::mutex> lock(g_mappingsMutex);
                g_mappings.push_back(g_learn_pending);
            }

            std::wstring displayStr = L"Mapped MIDI ";
            displayStr += (g_learn_pending.midi_type == 0 ? L"Note" : L"CC");
            displayStr += L" " + std::to_wstring(g_learn_pending.midi_num);
            displayStr += L" -> " + GetModifierString(g_learn_pending.modifiers) + GetKeyName(g_learn_pending.key_vk);
            SendLog(WideToUtf8(displayStr));
            SendMappingsToUI();
            SendStatus("Mapped successfully.");

            g_learning = false;
            PostToWebView({ {"type", "learn_done"} });
        }
    }
    else if (action == "delete_mapping") {
        int index = msg.value("index", -1);
        {
            std::lock_guard<std::mutex> lock(g_mappingsMutex);
            if (index >= 0 && index < (int)g_mappings.size())
                g_mappings.erase(g_mappings.begin() + index);
        }
        SendMappingsToUI();
        SendLog("Mapping removed.");
    }
    else if (action == "clear_mappings") {
        {
            std::lock_guard<std::mutex> lock(g_mappingsMutex);
            g_mappings.clear();
        }
        SendMappingsToUI();
        SendLog("All mappings cleared.");
    }
    else if (action == "save_profile") {
        OPENFILENAME ofn = {};
        wchar_t file[MAX_PATH] = L"mappings.json";
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwndMain;
        ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = L"json";
        if (GetSaveFileName(&ofn)) {
            SaveMappings(file);
            g_lastProfilePath = file;
            SaveConfig();
            SendLog("Profile saved: " + WideToUtf8(file));
        }
    }
    else if (action == "load_profile") {
        OPENFILENAME ofn = {};
        wchar_t file[MAX_PATH] = L"";
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwndMain;
        ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileName(&ofn)) {
            LoadMappings(file);
            SaveConfig();
            SendLog("Profile loaded: " + WideToUtf8(file));
        }
    }
    else if (action == "open_settings") {
        // For now, show the about box or a placeholder since the old dialog-based settings 
        // need to be ported to web.
        MessageBox(g_hwndMain, L"Web-based settings are under construction. Please use the Save/Load profile buttons for now.", L"Settings", MB_OK | MB_ICONINFORMATION);
    }
    else if (action == "clear_log") {
        // Handled in JS, but could do backend cleanup if needed
    }
    else if (action == "scan_ports") {
        ScanMidiPorts();
    }
    else if (action == "show_about") {
        MessageBox(g_hwndMain,
            L"MIDITypist v3.0\n"
            L"Modern MIDI-to-Keyboard Mapper\n\n"
            L"Features:\n"
            L"\x2022 Hybrid WebView2 Architecture\n"
            L"\x2022 Premium Glassmorphism UI\n"
            L"\x2022 Low-latency C++ MIDI Engine\n\n"
            L"(C) 2026",
            L"About MIDITypist", MB_OK | MB_ICONINFORMATION);
    }
}

// ══════════════════════════════════════════
//  Window Procedure
// ══════════════════════════════════════════

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwndMain = hwnd;
        g_hWinEventHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(hwnd, RECONNECT_TIMER_ID, RECONNECT_INTERVAL, NULL);
        SetTimer(hwnd, PIANO_DECAY_TIMER, PIANO_DECAY_MS, NULL);
        AddTrayIcon(hwnd);
        break;
    case WM_SIZE:
        if (g_controller) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_controller->put_Bounds(rc);
        }
        break;
    case WM_TIMER:
        if (wParam == RECONNECT_TIMER_ID) {
            TryAutoReconnect();
        }
        else if (wParam == PIANO_DECAY_TIMER) {
            bool changed = false;
            for (int i = 0; i < PIANO_TOTAL_KEYS; i++) {
                if (g_pianoVelocity[i] > 0) {
                    int decayed = g_pianoVelocity[i] - 8;
                    g_pianoVelocity[i] = (decayed > 0) ? decayed : 0;
                    changed = true;
                }
            }
            if (changed) {
                json vel = json::array();
                for (int i = 0; i < PIANO_TOTAL_KEYS; i++) vel.push_back(g_pianoVelocity[i]);
                PostToWebView({ {"type", "piano_decay"}, {"velocities", vel} });
            }
        }
        break;
    case WM_USER + 100: {
        int slot = (int)wParam;
        if (slot >= 0 && slot < (int)g_profileSlots.size()) {
            LoadMappings(g_profileSlots[slot]);
            SendLog("Switched to profile slot #" + std::to_string(slot));
            SendStatus("Profile switched via MIDI.");
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
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_SHOW) RestoreFromTray(hwnd);
        else if (LOWORD(wParam) == ID_TRAY_EXIT) DestroyWindow(hwnd);
        break;
    case WM_CLOSE:
        MinimizeToTray(hwnd);
        return 0;
    case WM_DESTROY:
        SaveConfig();
        KillTimer(hwnd, RECONNECT_TIMER_ID);
        KillTimer(hwnd, PIANO_DECAY_TIMER);
        if (g_hWinEventHook) { UnhookWinEvent(g_hWinEventHook); g_hWinEventHook = nullptr; }
        RemoveTrayIcon();
        g_midiIn.reset();
        g_webview = nullptr;
        g_controller = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ══════════════════════════════════════════
//  WebView2 Initialization
// ══════════════════════════════════════════

void InitWebView2(HWND hwnd) {
    // Determine the path to the 'ui' folder
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\") + 1);

    std::wstring uiPath;
    std::vector<std::wstring> potentialPaths = {
        exeDir + L"ui",                                 // Local (Installed structure)
        exeDir + L"..\\..\\MIDI Mapper\\src\\ui",       // Dev (main/x64/Debug -> main/MIDI Mapper/src/ui)
        exeDir + L"..\\src\\ui",                        // Alternative dev
        exeDir                                          // Root fallback
    };

    for (const auto& path : potentialPaths) {
        std::wstring checkFile = path + L"\\index.html";
        if (GetFileAttributesW(checkFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
            uiPath = path;
            break;
        }
    }

    if (uiPath.empty()) {
        std::wstring msg = L"Could not find 'ui\\index.html' in potential locations.\n\nChecked near:\n" + exeDir + 
                          L"\n\nPlease ensure the 'ui' folder containing 'index.html' is next to the executable.";
        MessageBox(hwnd, msg.c_str(), L"MIDITypist Error", MB_ICONERROR);
        return;
    }

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, uiPath](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;

                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd, uiPath](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return result;

                            g_controller = controller;
                            controller->get_CoreWebView2(&g_webview);

                            // Settings
                            wil::com_ptr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            settings->put_IsScriptEnabled(TRUE);
                            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                            settings->put_IsWebMessageEnabled(TRUE);
                            settings->put_AreDevToolsEnabled(TRUE);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_IsZoomControlEnabled(FALSE);

                            // Virtual Host Mapping (Secure and robust way to load local files)
                            wil::com_ptr<ICoreWebView2_3> webView3;
                            if (SUCCEEDED(g_webview->QueryInterface(IID_PPV_ARGS(&webView3)))) {
                                webView3->SetVirtualHostNameToFolderMapping(
                                    L"app.miditypist", uiPath.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            // Make background transparent
                            wil::com_ptr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
                                COREWEBVIEW2_COLOR transparent = { 0, 0, 0, 0 };
                                controller2->put_DefaultBackgroundColor(transparent);
                            }

                            RECT rc;
                            GetClientRect(hwnd, &rc);
                            controller->put_Bounds(rc);

                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string messageRaw;
                                        args->TryGetWebMessageAsString(&messageRaw);
                                        if (messageRaw) {
                                            HandleWebMessage(WideToUtf8(messageRaw.get()));
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // Navigate to the virtual domain
                            g_webview->Navigate(L"https://app.miditypist/index.html");

                            return S_OK;
                        }).Get());

                return S_OK;
            }).Get());
}

// ══════════════════════════════════════════
//  Entry Point
// ══════════════════════════════════════════

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initialize COM for WebView2
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_hInst = hInstance;
    g_configPath = GetConfigDir() + L"midityper_config.json";
    LoadConfig();

    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"MIDITypist";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(RGB(28, 28, 30));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"MIDITypist",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1140, 640,
        nullptr, nullptr, hInstance, nullptr);

    // Apply dark mode & Mica backdrop
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int mica = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_MICA_EFFECT, &mica, sizeof(mica));
    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Initialize WebView2
    InitWebView2(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return 0;
}
