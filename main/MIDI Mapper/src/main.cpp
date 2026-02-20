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
#include <set>
#include <mutex>
#include <memory>
#include <algorithm>
#include <functional>
#include "RtMidi.h"

// WebView2
#include <wrl.h>
#include <wil/com.h>
// Suppress warnings from external libraries
#pragma warning(push)
#pragma warning(disable: 26819) // Unannotated fallthrough in json.hpp
#pragma warning(disable: 26495) // Uninitialized member in json.hpp
#pragma warning(disable: 28182) // Dereferencing null pointer in wil/resource.h
#include "json.hpp"
#include <wil/result.h>
#include <wil/resource.h>
#include "WebView2.h"
#pragma warning(pop)

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
#define IDI_APP_ICON 101
#define ID_TRAY_SHOW 4001
#define ID_TRAY_EXIT 4002

// ── Piano Roll ──
#define PIANO_TOTAL_KEYS 128
#define PIANO_DECAY_TIMER 503
#define PIANO_DECAY_MS 50
#define CHORD_TIMER_ID 504
#define CHORD_THRESHOLD_MS 60 // Window to group notes into a chord
#define WM_CHORD_SIGNAL (WM_USER + 201)
#define WM_LEARN_MIDI_SIGNAL (WM_USER + 202)
#define GESTURE_TIMER_ID 505
#define GESTURE_WINDOW_MS 300
#define LONG_HOLD_MS 800

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
    int midi_type;      // 0=Note, 1=CC, 2=Chord, 3=LayerKey, 4=Macro, 5=AI
    int midi_num;       // for Note/CC/LayerKey/Macro/AI
    std::vector<int> midi_chord; // for Chord
    int key_vk;
    int modifiers;      // bitmask: 1=Ctrl, 2=Shift, 4=Alt
    int vel_min;
    int vel_zone;       // 0=any, 1=soft(1-63), 2=hard(64-127)
    int cc_action;      // 0=keypress, 1=mouse_x, 2=mouse_y, 3=scroll, 4=hold_key
    int profile_switch; // -1=normal, 0+=profile slot index
    std::string macro_text; // for Macro
    std::string ai_prompt;  // for AI
    std::string title_pattern; // for Context Filter
    std::string app_pattern;   // for Process Filter (e.g. chrome.exe)
    int gesture_id;     // 0=Single/Any, 1=Double Tap, 2=Long Hold
};

std::vector<Mapping> g_mappings;
std::recursive_mutex g_mappingsMutex;
bool g_learning = false;
std::mutex g_learnMutex;
DWORD g_learnStartTime = 0;
Mapping g_learn_pending = { -1, -1, {}, -1, 0, 1, 0, 0, -1, "", "", "", "", 0 };

// ── Per-App Profile ──
std::map<std::wstring, std::wstring> g_appProfileBindings;
std::wstring g_currentApp;
std::wstring g_currentWindowTitle;
HWINEVENTHOOK g_hWinEventHook = nullptr;

// ── Gesture State ──
struct KeyState {
    DWORD lastPressTime = 0;
    int tapCount = 0;
    bool processingGesture = false;
    bool holdTriggered = false;
};
std::map<int, KeyState> g_keyStates;
std::mutex g_gestureMutex;

// ── Tray Icon ──
NOTIFYICONDATA g_nid = {};
bool g_minimizedToTray = false;

// ── Persistent Config ──
std::wstring g_configPath;
std::wstring g_lastProfilePath;

// ── Piano Roll State ──
int g_pianoVelocity[PIANO_TOTAL_KEYS] = { 0 };
bool g_pianoPhysicalDown[PIANO_TOTAL_KEYS] = { false };
int g_pianoCC[128] = { 0 };
bool g_sustainActive = false;
std::set<int> g_sustainedVKs;
std::mutex g_sustainMutex;

// ── Auto Reconnect & App Switching ──
bool g_autoReconnect = true;
bool g_appSwitchingEnabled = true;
bool g_velocityZonesEnabled = true;
bool g_minimizeToTrayEnabled = true;
std::string g_aiApiKey;
std::string g_aiGlobalPrompt = "You are a desktop automation assistant. Perform the following task briefly: {prompt}";

// ── Chord Collector ──
std::vector<int> g_chordBuffer;
std::mutex g_chordMutex;

// ── Profile Slots for MIDI switching ──
std::vector<std::wstring> g_profileSlots;

// ── CC Hold State ──
std::map<int, bool> g_ccHoldActive;

// ── Hook State ──
HHOOK g_hKeyboardHook = NULL;

// ── Forward Declarations ──
void SendMappingsToUI();
void ResolveGesture(int midi_num, int gesture_id);
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

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
    std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
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

        json item = {
            {"midi_type", m.midi_type}, {"midi_num", m.midi_num},
            {"key_vk", m.key_vk}, {"modifiers", m.modifiers},
            {"vel_min", m.vel_min}, {"vel_zone", m.vel_zone},
            {"cc_action", m.cc_action}, {"profile_switch", m.profile_switch},
            {"target_display", WideToUtf8(targetDisplay)},
            {"macro_text", m.macro_text},
            {"ai_prompt", m.ai_prompt},
            {"title_pattern", m.title_pattern},
            {"app_pattern", m.app_pattern},
            {"gesture_id", m.gesture_id}
        };
        if (m.midi_type == 2) item["midi_chord"] = m.midi_chord;
        arr.push_back(item);
    }
    PostToWebView({ {"type", "mappings"}, {"mappings", arr} });
}

// ══════════════════════════════════════════
//  Key Simulation (with modifiers)
// ══════════════════════════════════════════

// ── Improved Key Simulation (Game Compatible) ──
void SendKeyInput(int vk, bool down, int modifiers = 0) {
    std::vector<INPUT> inputs;
    
    // 1. Helper to create ScanCode INPUT
    auto CreateInput = [](int virtualKey, bool isDown) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        UINT sc = MapVirtualKey(virtualKey, MAPVK_VK_TO_VSC);
        input.ki.wScan = (WORD)sc;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | (isDown ? 0 : KEYEVENTF_KEYUP);
        
        // 2. Handle Extended Keys (Arrows, Numpad Enter, etc.)
        if (virtualKey == VK_LEFT || virtualKey == VK_UP || virtualKey == VK_RIGHT || virtualKey == VK_DOWN ||
            virtualKey == VK_PRIOR || virtualKey == VK_NEXT || virtualKey == VK_END || virtualKey == VK_HOME ||
            virtualKey == VK_INSERT || virtualKey == VK_DELETE || virtualKey == VK_DIVIDE || virtualKey == VK_RMENU || 
            virtualKey == VK_RCONTROL) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        return input;
    };

    // 3. Press Modifiers (if down)
    if (down && modifiers > 0) {
        if (modifiers & 1) inputs.push_back(CreateInput(VK_CONTROL, true));
        if (modifiers & 2) inputs.push_back(CreateInput(VK_SHIFT, true));
        if (modifiers & 4) inputs.push_back(CreateInput(VK_MENU, true)); // Alt
    }

    // 4. Main Key
    inputs.push_back(CreateInput(vk, down));

    // 5. Release Modifiers (if up)
    if (!down && modifiers > 0) {
        if (modifiers & 4) inputs.push_back(CreateInput(VK_MENU, false));
        if (modifiers & 2) inputs.push_back(CreateInput(VK_SHIFT, false));
        if (modifiers & 1) inputs.push_back(CreateInput(VK_CONTROL, false));
    }

    if (!inputs.empty()) {
        SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
    }
}

void SimulateKeyCombo(int vk, int modifiers) {
    SendKeyInput(vk, true, modifiers);
    SendKeyInput(vk, false, modifiers);
}

void SimulateHoldKey(int vk, bool down) {
    SendKeyInput(vk, down, 0); // Modifiers not used for simple CC-Hold tags
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

void SimulateText(const std::string& text) {
    if (text.empty()) return;
    std::wstring wtext = Utf8ToWide(text);
    std::vector<INPUT> inputs;
    for (wchar_t ch : wtext) {
        INPUT inDown = {};
        inDown.type = INPUT_KEYBOARD;
        inDown.ki.wScan = ch;
        inDown.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(inDown);

        INPUT inUp = inDown;
        inUp.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(inUp);
    }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

// ══════════════════════════════════════════
//  Mapping Persistence
// ══════════════════════════════════════════

void SaveMappings(const std::wstring& filename) {
    json j = json::array();
    {
        std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
        for (const auto& m : g_mappings) {
            json item = {
                {"midi_type", m.midi_type}, {"midi_num", m.midi_num},
                {"key_vk", m.key_vk}, {"modifiers", m.modifiers},
                {"vel_min", m.vel_min}, {"vel_zone", m.vel_zone},
                {"cc_action", m.cc_action}, {"profile_switch", m.profile_switch}
            };
            if (m.midi_type == 2) item["midi_chord"] = m.midi_chord;
            if (m.midi_type == 4) item["macro_text"] = m.macro_text;
            if (m.midi_type == 5) item["ai_prompt"] = m.ai_prompt;
            if (!m.title_pattern.empty()) item["title_pattern"] = m.title_pattern;
            if (!m.app_pattern.empty()) item["app_pattern"] = m.app_pattern;
            item["gesture_id"] = m.gesture_id;
            j.push_back(item);
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
        std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
        g_mappings.clear();
        for (const auto& it : j) {
            Mapping m = {};
            m.midi_type = it.value("midi_type", 0);
            m.midi_num = it.value("midi_num", 0);
            if (it.contains("midi_chord") && it["midi_chord"].is_array()) {
                m.midi_chord = it["midi_chord"].get<std::vector<int>>();
            }
            m.macro_text = it.value("macro_text", "");
            m.ai_prompt = it.value("ai_prompt", "");
            m.title_pattern = it.value("title_pattern", "");
            m.app_pattern = it.value("app_pattern", "");
            m.gesture_id = it.value("gesture_id", 0);
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
    cfg["app_switching_enabled"] = g_appSwitchingEnabled;
    json bindings = json::object();
    for (auto& [exe, profile] : g_appProfileBindings)
        bindings[WideToUtf8(exe)] = WideToUtf8(profile);
    cfg["app_bindings"] = bindings;
    json slots = json::array();
    for (auto& s : g_profileSlots)
        slots.push_back(WideToUtf8(s));
    cfg["profile_slots"] = slots;
    cfg["ai_api_key"] = g_aiApiKey;
    cfg["ai_global_prompt"] = g_aiGlobalPrompt;
    cfg["velocity_zones_enabled"] = g_velocityZonesEnabled;
    cfg["minimize_to_tray_enabled"] = g_minimizeToTrayEnabled;
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
    g_appSwitchingEnabled = cfg.value("app_switching_enabled", true);
    if (cfg.contains("app_bindings") && cfg["app_bindings"].is_object()) {
        for (auto& [k, v] : cfg["app_bindings"].items())
            g_appProfileBindings[Utf8ToWide(k)] = Utf8ToWide(v.get<std::string>());
    }
    g_aiApiKey = cfg.value("ai_api_key", "");
    g_aiGlobalPrompt = cfg.value("ai_global_prompt", "You are a desktop automation assistant. Perform the following task briefly: {prompt}");
    g_velocityZonesEnabled = cfg.value("velocity_zones_enabled", true);
    g_minimizeToTrayEnabled = cfg.value("minimize_to_tray_enabled", true);

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
    g_nid.hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_APP_ICON));
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

void ProcessMIDIEvent(int type, int number, int velocity);

void midiCallback(double, std::vector<unsigned char>* msg, void*) {
    if (msg->size() < 3) return;
    int status = (*msg)[0];
    int number = (*msg)[1];
    int velocity = (*msg)[2];

    bool isNoteOn = (status & 0xF0) == 0x90 && velocity > 0;
    bool isNoteOff = (status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && velocity == 0);
    bool isCC = (status & 0xF0) == 0xB0;

    // Learning mode check
    {
        std::lock_guard<std::mutex> lock(g_learnMutex);
        if (g_learning && g_learn_pending.midi_type == -1) {
            bool learnThis = false;
            int type = 0;
            if (isNoteOn) {
                type = 0;
                learnThis = true;
            } else if (isCC && number < 120) { // Filter CC noise
                type = 1;
                learnThis = true;
            }

            if (learnThis) {
                // Post to main thread to handle transition
                PostMessage(g_hwndMain, WM_LEARN_MIDI_SIGNAL, (WPARAM)type, (LPARAM)number);
                return; // Exit callback if learning
            }
        }
    }

    // Track physical state
    if (isNoteOn) g_pianoPhysicalDown[number] = true;
    if (isNoteOff) g_pianoPhysicalDown[number] = false;

    // CC immediately if not learning
    if (isCC) {
        ProcessMIDIEvent(status & 0xF0, number, velocity);
    }
    
    // Chord grouping logic for Note On
    if (isNoteOn) {
        std::lock_guard<std::mutex> lock(g_chordMutex);
        g_chordBuffer.push_back(number);
        // Signal main thread to reset/start the chord timer
        PostMessage(g_hwndMain, WM_CHORD_SIGNAL, 0, 0);
    }
    
    // Process Note Off immediately
    if (isNoteOff) {
        ProcessMIDIEvent(status & 0xF0, number, velocity);
    }
}

void ProcessChord(const std::vector<int>& chord) {
    if (chord.empty()) return;

    std::vector<int> sortedChord = chord;
    std::sort(sortedChord.begin(), sortedChord.end());
    sortedChord.erase(std::unique(sortedChord.begin(), sortedChord.end()), sortedChord.end());

    std::string chordStr = "";
    for (int n : sortedChord) chordStr += std::to_string(n) + " ";
    SendLog("Processing MIDI chord: [ " + chordStr + "]");
    
    std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
    bool found = false;

    // 1. Try to find a specific chord mapping
    if (sortedChord.size() > 1) {
        for (const auto& m : g_mappings) {
            if (m.midi_type != 2) continue;

            // Context Stack Filtering
            if (!m.title_pattern.empty()) {
                std::string currentTitle = WideToUtf8(g_currentWindowTitle);
                if (currentTitle.find(m.title_pattern) == std::string::npos) continue;
            }
            if (!m.app_pattern.empty()) {
                std::string currentApp = WideToUtf8(g_currentApp);
                if (currentApp.find(m.app_pattern) == std::string::npos) continue;
            }

            // Compare sorted notes
            std::vector<int> targetChord = m.midi_chord;
            std::sort(targetChord.begin(), targetChord.end());
            targetChord.erase(std::unique(targetChord.begin(), targetChord.end()), targetChord.end());

            if (targetChord == sortedChord) {
                SimulateKeyCombo(m.key_vk, m.modifiers);
                SendLog("Match found! Triggering VK " + std::to_string(m.key_vk));
                found = true;
                break;
            }
        }
    }

    // 2. If no chord mapping or single note, process individual mappings
    if (!found) {
        for (int note : sortedChord) {
            ProcessMIDIEvent(0x90, note, 100); // Trigger as standard Note On
        }
    }
}

void ProcessMIDIEvent(int type, int number, int velocity) {
    bool isNoteOn = (type == 0x90) && velocity > 0;
    bool isNoteOff = (type == 0x80) || ((type == 0x90) && velocity == 0);
    bool isCC = (type == 0xB0);

    // Update Piano Roll and Gesture state
    if (isNoteOn && number >= 0 && number < 128) {
        g_pianoVelocity[number] = velocity;
        PostToWebView({ {"type", "midi_note"}, {"note", number}, {"velocity", velocity} });
        
        std::lock_guard<std::mutex> lock(g_gestureMutex);
        auto& state = g_keyStates[number];
        DWORD now = GetTickCount();
        if (now - state.lastPressTime < GESTURE_WINDOW_MS) {
            state.tapCount++;
        } else {
            state.tapCount = 1;
            SetTimer(g_hwndMain, GESTURE_TIMER_ID + number, GESTURE_WINDOW_MS, NULL);
        }
        state.lastPressTime = now;
        state.processingGesture = true;
        state.holdTriggered = false;
    }
    else if (isNoteOff && number >= 0 && number < 128) {
        g_pianoVelocity[number] = 0;
        
        std::lock_guard<std::mutex> lock(g_gestureMutex);
        auto& state = g_keyStates[number];
        DWORD duration = GetTickCount() - state.lastPressTime;
        if (duration >= LONG_HOLD_MS && !state.holdTriggered) {
            state.holdTriggered = true;
            KillTimer(g_hwndMain, GESTURE_TIMER_ID + number);
            state.tapCount = 0;
            state.processingGesture = false;
            ResolveGesture(number, 2); // Long Hold
        }
    }

    int oldCCVal = -1;
    if (isCC && number >= 0 && number < 128) {
        oldCCVal = g_pianoCC[number];
        g_pianoCC[number] = velocity;
        PostToWebView({ {"type", "midi_cc"}, {"cc", number}, {"value", velocity} });

        // Global Sustain Pedal Support (CC 64)
        if (number == 64) {
            std::lock_guard<std::mutex> lock(g_sustainMutex);
            if (velocity > 63 && !g_sustainActive) {
                g_sustainActive = true;
                SendLog("Sustain Pedal: ON", "mapping");
            } else if (velocity <= 63 && g_sustainActive) {
                g_sustainActive = false;
                SendLog("Sustain Pedal: OFF", "mapping");
                for (int vk : g_sustainedVKs) {
                    SendKeyInput(vk, false);
                }
                g_sustainedVKs.clear();
            }
        }
    }

    // CC edge detection flags
    bool ccCrossedUp = (isCC && oldCCVal <= 63 && velocity > 63);
    bool ccCrossedDown = (isCC && oldCCVal > 63 && velocity <= 63);

    // Execute mappings
    std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        // Context filtering
        if (!m.title_pattern.empty()) {
            std::string currentTitle = WideToUtf8(g_currentWindowTitle);
            if (currentTitle.find(m.title_pattern) == std::string::npos) continue;
        }
        if (!m.app_pattern.empty()) {
            std::string currentApp = WideToUtf8(g_currentApp);
            if (currentApp.find(m.app_pattern) == std::string::npos) continue;
        }

        // Profile switching
        if (m.profile_switch >= 0) {
            if (m.midi_type == 0 && isNoteOn && number == m.midi_num) {
                if (m.profile_switch < (int)g_profileSlots.size()) {
                    PostMessage(g_hwndMain, WM_USER + 100, m.profile_switch, 0);
                }
            }
            continue;
        }

        // Note-to-Key Mapping
        if (m.midi_type == 0 && number == m.midi_num && m.gesture_id == 0) {
            if (isNoteOn) {
                if (!g_pianoPhysicalDown[number]) continue; // Rapid tap safety
                if (velocity < m.vel_min) continue;
                if (g_velocityZonesEnabled) {
                    if (m.vel_zone == 1 && velocity > 63) continue;
                    if (m.vel_zone == 2 && velocity < 64) continue;
                }
                SendKeyInput(m.key_vk, true, m.modifiers);
                SendLog("Note " + std::to_string(number) + " -> Key Down: " + std::to_string(m.key_vk), "mapping");
            }
            else if (isNoteOff) {
                std::lock_guard<std::mutex> lock(g_sustainMutex);
                if (g_sustainActive) {
                    g_sustainedVKs.insert(m.key_vk);
                    SendLog("Note " + std::to_string(number) + " -> Sustaining VK " + std::to_string(m.key_vk), "mapping");
                } else {
                    SendKeyInput(m.key_vk, false, m.modifiers);
                    SendLog("Note " + std::to_string(number) + " -> Key Up: " + std::to_string(m.key_vk), "mapping");
                }
            }
        }

        // CC-to-Action Mapping (Edge Detected)
        if (m.midi_type == 1 && isCC && number == m.midi_num) {
            switch (m.cc_action) {
            case 0: // Keypress (Now Momentary by default for games)
                if (ccCrossedUp) {
                    SendKeyInput(m.key_vk, true, m.modifiers);
                    SendLog("CC " + std::to_string(number) + " -> Key Down: " + std::to_string(m.key_vk), "mapping");
                } else if (ccCrossedDown) {
                    SendKeyInput(m.key_vk, false, m.modifiers);
                    SendLog("CC " + std::to_string(number) + " -> Key Up: " + std::to_string(m.key_vk), "mapping");
                }
                break;
            case 1: SimulateMouseMove((velocity - 64) * 2, 0); break;
            case 2: SimulateMouseMove(0, (velocity - 64) * 2); break;
            case 3: SimulateScroll((velocity - 64) * 20); break;
            case 4: // Hold Key (Dedicated toggle behavior or held state)
                if (ccCrossedUp && !g_ccHoldActive[m.midi_num]) {
                    SendKeyInput(m.key_vk, true);
                    g_ccHoldActive[m.midi_num] = true;
                }
                else if (ccCrossedDown && g_ccHoldActive[m.midi_num]) {
                    SendKeyInput(m.key_vk, false);
                    g_ccHoldActive[m.midi_num] = false;
                }
                break;
            }
        }
        
        // Macros, AI, HUD
        if (m.midi_type == 4 && isNoteOn && number == m.midi_num && m.gesture_id == 0) {
            SimulateText(m.macro_text);
        }
        if (m.midi_type == 5 && isNoteOn && number == m.midi_num && m.gesture_id == 0) {
            PostToWebView({ {"type", "run_ai"}, {"prompt", m.ai_prompt} });
            SendLog("AI Prompt sent: " + m.ai_prompt);
        }
        if (m.midi_type == 3 && number == m.midi_num) {
            if (isNoteOn) PostToWebView({ {"type", "hud"}, {"active", true}, {"title", WideToUtf8(GetModifierString(m.modifiers) + GetKeyName(m.key_vk))} });
            else if (isNoteOff) PostToWebView({ {"type", "hud"}, {"active", false} });
        }
    }
}

void ResolveGesture(int midi_num, int gesture_id) {
    std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
    for (const auto& m : g_mappings) {
        if (m.midi_num != midi_num) continue;
        
        // Exact gesture match, and ignore gesture 0 here because it's handled immediately in ProcessMIDIEvent
        if (m.gesture_id != gesture_id || m.gesture_id == 0) continue;

        // Context check
        if (!m.title_pattern.empty()) {
            std::string currentTitle = WideToUtf8(g_currentWindowTitle);
            if (currentTitle.find(m.title_pattern) == std::string::npos) continue;
        }
        if (!m.app_pattern.empty()) {
            std::string currentApp = WideToUtf8(g_currentApp);
            if (currentApp.find(m.app_pattern) == std::string::npos) continue;
        }

        // Execute (Simplified trigger for gesture demo)
        if (m.midi_type == 0) SimulateKeyCombo(m.key_vk, m.modifiers);
        else if (m.midi_type == 4) SimulateText(m.macro_text);
        else if (m.midi_type == 5) PostToWebView({ {"type", "run_ai"}, {"prompt", m.ai_prompt} });
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

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

void StartAppMonitoring() {
    if (!g_hWinEventHook) {
        g_hWinEventHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
}

void StopAppMonitoring() {
    if (g_hWinEventHook) {
        UnhookWinEvent(g_hWinEventHook);
        g_hWinEventHook = nullptr;
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd) {
        wchar_t path[MAX_PATH];
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess) {
            if (GetProcessImageFileNameW(hProcess, path, MAX_PATH)) {
                wchar_t* filename = wcsrchr(path, L'\\');
                if (filename) {
                    g_currentApp = filename + 1;
                    
                    // Capture title too
                    wchar_t title[512];
                    if (GetWindowTextW(hwnd, title, 512)) {
                        g_currentWindowTitle = title;
                    } else {
                        g_currentWindowTitle = L"";
                    }

                    PostToWebView({ 
                        {"type", "app_changed"}, 
                        {"app", WideToUtf8(g_currentApp)},
                        {"title", WideToUtf8(g_currentWindowTitle)}
                    });
                    
                    // Trigger profile switch if bound
                    if (g_appProfileBindings.count(g_currentApp)) {
                        LoadMappings(g_appProfileBindings[g_currentApp]);
                        SendLog("Auto-switched profile for: " + WideToUtf8(g_currentApp));
                    }
                }
            }
            CloseHandle(hProcess);
        }
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
        
        // Send current config to UI
        json cfgMsg = {
            {"type", "config"},
            {"config", {
                {"auto_reconnect", g_autoReconnect},
                {"app_switching", g_appSwitchingEnabled},
                {"ai_api_key", g_aiApiKey},
                {"ai_global_prompt", g_aiGlobalPrompt},
                {"velocity_zones", g_velocityZonesEnabled},
                {"minimize_to_tray", g_minimizeToTrayEnabled}
            }}
        };
        PostToWebView(cfgMsg);
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
        std::lock_guard<std::mutex> lock(g_learnMutex);
        g_learning = true;
        g_learnStartTime = GetTickCount();
        g_learn_pending = { -1, -1, {}, -1, 0, 1, 0, 0, -1, "", "", "", "", 0 };
        
        // Clean up any stale hook
        if (g_hKeyboardHook) {
            UnhookWindowsHookEx(g_hKeyboardHook);
            g_hKeyboardHook = NULL;
        }

        // Kill any pending chord timers and clear the buffer
        KillTimer(g_hwndMain, CHORD_TIMER_ID);
        {
            std::lock_guard<std::mutex> chordLock(g_chordMutex);
            g_chordBuffer.clear();
        }

        SendLog("Learning started: Waiting for MIDI...");
        SendStatus("Waiting for MIDI input...");
    }
    else if (action == "cancel_learn") {
        std::lock_guard<std::mutex> lock(g_learnMutex);
        g_learning = false;
        KillTimer(g_hwndMain, CHORD_TIMER_ID);
        if (g_hKeyboardHook) {
            UnhookWindowsHookEx(g_hKeyboardHook);
            g_hKeyboardHook = NULL;
        }
        g_learn_pending = { -1, -1, {}, -1, 0, 1, 0, 0, -1, "", "", "", "", 0 };
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
                std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
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
        if (index >= 0) {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            if (index < (int)g_mappings.size()) {
                g_mappings.erase(g_mappings.begin() + index);
            }
        }
        SendMappingsToUI();
        SendLog("Mapping removed.");
    }
    else if (action == "clear_mappings") {
        {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            g_mappings.clear();
        }
        SendMappingsToUI();
        SendLog("All mappings cleared.");
    }
    else if (action == "update_mapping") {
        int index = msg.value("index", -1);
        if (index >= 0) {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            if (index < (int)g_mappings.size()) {
                Mapping& m = g_mappings[index];
                m.midi_type = msg.value("midi_type", m.midi_type);
                m.key_vk = msg.value("key_vk", m.key_vk);
                m.macro_text = msg.value("macro_text", m.macro_text);
                m.ai_prompt = msg.value("ai_prompt", m.ai_prompt);
                
                if (msg.contains("midi_chord") && msg["midi_chord"].is_array()) {
                    std::vector<int> chord = msg["midi_chord"].get<std::vector<int>>();
                    std::sort(chord.begin(), chord.end());
                    chord.erase(std::unique(chord.begin(), chord.end()), chord.end());
                    m.midi_chord = chord;
                }

                m.title_pattern = msg.value("title_pattern", m.title_pattern);
                m.app_pattern = msg.value("app_pattern", m.app_pattern);
                m.gesture_id = msg.value("gesture_id", m.gesture_id);
            }
        }
        SendMappingsToUI();
        SendLog("Mapping updated.");
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
    else if (action == "update_config") {
        g_autoReconnect = msg.value("auto_reconnect", true);
        g_appSwitchingEnabled = msg.value("app_switching", false);
        g_velocityZonesEnabled = msg.value("velocity_zones", true);
        g_minimizeToTrayEnabled = msg.value("minimize_to_tray", true);
        if (g_appSwitchingEnabled != (g_hWinEventHook != nullptr)) {
            if (g_appSwitchingEnabled) StartAppMonitoring();
            else StopAppMonitoring();
        }
        g_aiApiKey = msg.value("ai_api_key", g_aiApiKey);
        g_aiGlobalPrompt = msg.value("ai_global_prompt", g_aiGlobalPrompt);
        SaveConfig();
        SendLog("Settings updated.");
    }
    else if (action == "simulate_text") {
        std::string text = msg.value("text", "");
        SimulateText(text);
    }
    else if (action == "add_mapping") {
        {
            std::lock_guard<std::recursive_mutex> lock(g_mappingsMutex);
            Mapping m = { 0, 0, {}, 0, 0, 1, 0, 0, -1, "", "", "", "", 0 };
            g_mappings.push_back(m);
        }
        SendMappingsToUI();
        SendLog("Manual mapping added.");
    }
    else if (action == "open_settings") {
        // Overlay is handled in JS, but we could trigger it from backend if needed
    }
    else if (action == "clear_log") {
        // Handled in JS, but could do backend cleanup if needed
    }
    else if (action == "scan_ports") {
        ScanMidiPorts();
    }
    else if (action == "show_about") {
        MessageBox(g_hwndMain,
            L"MIDITypist v1.0\n"
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
        if (g_appSwitchingEnabled) StartAppMonitoring();
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
        if (wParam >= GESTURE_TIMER_ID && wParam < GESTURE_TIMER_ID + 128) {
            int note = (int)(wParam - GESTURE_TIMER_ID);
            KillTimer(hwnd, wParam);
            
            int finalTapCount = 0;
            {
                std::lock_guard<std::mutex> lock(g_gestureMutex);
                auto& state = g_keyStates[note];
                finalTapCount = state.tapCount;
                state.tapCount = 0;
                state.processingGesture = false;
            }
            
            if (finalTapCount == 1) ResolveGesture(note, 0); // Single
            else if (finalTapCount == 2) ResolveGesture(note, 1); // Double
            return 0;
        }
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
        else if (wParam == CHORD_TIMER_ID) {
            KillTimer(hwnd, CHORD_TIMER_ID);
            std::vector<int> chord;
            {
                std::lock_guard<std::mutex> lock(g_chordMutex);
                chord = g_chordBuffer;
                g_chordBuffer.clear();
            }
            ProcessChord(chord);
        }
        break;
    case WM_CHORD_SIGNAL:
        if (!g_learning) {
            KillTimer(hwnd, CHORD_TIMER_ID);
            SetTimer(hwnd, CHORD_TIMER_ID, CHORD_THRESHOLD_MS, NULL);
        }
        break;

    case WM_LEARN_MIDI_SIGNAL:
    {
        int type = (int)wParam;
        int num = (int)lParam;
        std::lock_guard<std::mutex> lock(g_learnMutex);
        if (g_learning && g_learn_pending.midi_type == -1) {
            // Grace period: ignore events too close to start
            if (GetTickCount() - g_learnStartTime < 200) {
                return 0;
            }

            g_learn_pending.midi_type = type;
            g_learn_pending.midi_num = num;
            
            SendLog("Capture: MIDI " + std::string(type == 0 ? "Note " : "CC ") + std::to_string(num));
            SendLog("Transitioning to Phase 2 (Keyboard Capture)");
            
            PostToWebView({ {"type", "learn_phase"}, {"phase", 2}, {"text", "Now press a keyboard key..."} });
            SendStatus("Waiting for keyboard key...");

            if (g_hKeyboardHook) UnhookWindowsHookEx(g_hKeyboardHook);
            g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
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
        if (g_minimizeToTrayEnabled) {
            MinimizeToTray(hwnd);
        } else {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        if (g_hKeyboardHook) { UnhookWindowsHookEx(g_hKeyboardHook); g_hKeyboardHook = NULL; }
        SaveConfig();
        KillTimer(hwnd, RECONNECT_TIMER_ID);
        KillTimer(hwnd, PIANO_DECAY_TIMER);
        KillTimer(hwnd, CHORD_TIMER_ID);
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

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR pCmdLine, _In_ int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initialize COM for WebView2
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

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
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
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

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
        
        {
            std::lock_guard<std::mutex> lock(g_learnMutex);
            if (g_learning && g_learn_pending.midi_type != -1) {
                int vk = pKey->vkCode;
                int mods = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 1;
                if (GetKeyState(VK_SHIFT) & 0x8000) mods |= 2;
                if (GetKeyState(VK_MENU) & 0x8000) mods |= 4;

                SendLog("Capture: Keyboard VK " + std::to_string(vk) + " Mods " + std::to_string(mods));
                
                g_learn_pending.key_vk = vk;
                g_learn_pending.modifiers = mods;

                {
                    std::lock_guard<std::recursive_mutex> mlock(g_mappingsMutex);
                    g_mappings.push_back(g_learn_pending);
                }

                // Finish capture
                HHOOK h = g_hKeyboardHook;
                g_hKeyboardHook = NULL;
                g_learning = false;
                UnhookWindowsHookEx(h);

                std::wstring displayStr = L"Mapped MIDI ";
                displayStr += (g_learn_pending.midi_type == 0 ? L"Note" : L"CC");
                displayStr += L" " + std::to_wstring(g_learn_pending.midi_num);
                displayStr += L" -> " + GetModifierString(g_learn_pending.modifiers) + GetKeyName(g_learn_pending.key_vk);
                
                SendLog(WideToUtf8(displayStr));
                SendMappingsToUI();
                SendStatus("Mapped successfully.");
                PostToWebView({ {"type", "learn_done"} });
                
                return 1;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}
