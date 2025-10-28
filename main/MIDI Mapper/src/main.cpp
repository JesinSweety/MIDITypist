#include <windows.h>
#include <CommCtrl.h>
#include <Psapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include "RtMidi.h"
#include "json.hpp"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Psapi.lib")

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

#define APP_PROFILE_TIMER 501

COLORREF ACCENT_COLOR = RGB(36, 99, 202);
COLORREF ACCENT_COLOR_HOVER = RGB(60, 130, 240);
COLORREF BUTTON_TEXT = RGB(255, 255, 255);
COLORREF BACKGROUND_COLOR = RGB(36, 36, 38);

using json = nlohmann::json;

HINSTANCE g_hInst;
HFONT g_hModernFont = nullptr;
HWND g_hwndPorts, g_hwndButton, g_hwndLog, g_hwndLearn, g_hwndMappingList,
g_hwndStatus, g_hwndSettings;
RtMidiIn* g_midiIn = nullptr;
std::vector<std::string> g_ports;
std::vector<std::string> g_log;
bool g_connected = false;

struct Mapping {
    int midi_type;
    int midi_num;
    int key_vk;
};
std::vector<Mapping> g_mappings;
bool g_learning = false;
Mapping g_learn_pending = { -1, -1, -1 };

std::map<std::wstring, std::wstring> g_appProfileBindings;
std::wstring g_lastProfileLoadedForApp;

void SetUIFont(HWND hwnd) {
    if (!g_hModernFont) {
        LOGFONT lf = { 0 };
        lf.lfHeight = 20;
        lf.lfWeight = FW_SEMIBOLD;
        wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
        g_hModernFont = CreateFontIndirect(&lf);
    }
    SendMessage(hwnd, WM_SETFONT, (WPARAM)g_hModernFont, TRUE);
}

void AddLog(const std::string& msg) {
    g_log.push_back(msg);
    std::stringstream ss;
    for (const auto& line : g_log)
        ss << line << "\r\n";
    SetWindowTextA(g_hwndLog, ss.str().c_str());
    SendMessage(g_hwndLog, EM_SETSEL, GetWindowTextLength(g_hwndLog), GetWindowTextLength(g_hwndLog));
    SendMessage(g_hwndLog, EM_SCROLLCARET, 0, 0);
}

void SetStatus(const wchar_t* status) {
    SendMessage(g_hwndStatus, SB_SETTEXT, 0, (LPARAM)status);
}

void SimulateKey(int vk) {
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    INPUT input[2] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = 0;
    input[0].ki.wScan = sc;
    input[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = 0;
    input[1].ki.wScan = sc;
    input[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(INPUT));
}

void UpdateMappingList() {
    SendMessage(g_hwndMappingList, LB_RESETCONTENT, 0, 0);
    for (const auto& m : g_mappings) {
        std::wstringstream ws;
        ws << (m.midi_type == 0 ? L"Note " : L"CC ");
        ws << m.midi_num << L"  ->  VK_" << m.key_vk;
        SendMessage(g_hwndMappingList, LB_ADDSTRING, 0, (LPARAM)ws.str().c_str());
    }
}

void SaveMappings(const std::wstring& filename) {
    json j_profiles = json::array();
    for (const auto& m : g_mappings) {
        j_profiles.push_back({
            {"midi_type", m.midi_type},
            {"midi_num", m.midi_num},
            {"key_vk", m.key_vk}
            });
    }
    std::ofstream f(filename);
    if (f) f << j_profiles.dump(4);
}

void LoadMappings(const std::wstring& filename) {
    std::ifstream f(filename);
    if (!f) return;
    json j_profiles;
    f >> j_profiles;
    g_mappings.clear();
    for (const auto& it : j_profiles) {
        Mapping m;
        m.midi_type = it.value("midi_type", 0);
        m.midi_num = it.value("midi_num", 0);
        m.key_vk = it.value("key_vk", 0);
        g_mappings.push_back(m);
    }
    UpdateMappingList();
}

// -- MIDI CALLBACK AND OTHER HELPERS, SAME AS BEFORE --

void midiCallback(double, std::vector<unsigned char>* msg, void*) {
    if (!msg || msg->empty()) return;
    int status = msg->at(0) & 0xF0;
    int number = (msg->size() > 1) ? msg->at(1) : -1;
    int velocity = (msg->size() > 2) ? msg->at(2) : 0;

    std::stringstream ss;
    ss << "MIDI: [ ";
    for (unsigned char b : *msg) ss << (int)b << " ";
    ss << "]";
    AddLog(ss.str());

    if (g_learning && g_learn_pending.midi_type == -1) {
        if (status == 0x90 && velocity > 0) {
            g_learn_pending.midi_type = 0;
            g_learn_pending.midi_num = number;
            AddLog("Now press desired keyboard key to map...");
            SetStatus(L"Waiting for keyboard key...");
        }
        else if (status == 0xB0) {
            g_learn_pending.midi_type = 1;
            g_learn_pending.midi_num = number;
            AddLog("Now press desired keyboard key to map...");
            SetStatus(L"Waiting for keyboard key...");
        }
    }
    else {
        for (const auto& m : g_mappings) {
            if ((m.midi_type == 0 && status == 0x90 && number == m.midi_num && velocity > 0) ||
                (m.midi_type == 1 && status == 0xB0 && number == m.midi_num)) {
                SimulateKey(m.key_vk);
                std::stringstream action;
                action << "Sent raw scancode for VK_" << m.key_vk << " from MIDI";
                AddLog(action.str());
                SetStatus(L"MIDI mapped key sent.");
            }
        }
    }
}

void ScanMidiPorts(HWND hwnd) {
    g_ports.clear();
    RtMidiIn tempIn;
    int n = tempIn.getPortCount();
    SendMessage(g_hwndPorts, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; ++i) {
        std::string name = tempIn.getPortName(i);
        g_ports.push_back(name);
        std::wstring wname(name.begin(), name.end());
        SendMessage(g_hwndPorts, CB_ADDSTRING, 0, (LPARAM)wname.c_str());
    }
    if (n > 0) SendMessage(g_hwndPorts, CB_SETCURSEL, 0, 0);
}

void ConnectMidi(HWND hwnd) {
    int sel = (int)SendMessage(g_hwndPorts, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_ports.size()) return;
    if (g_midiIn) delete g_midiIn;
    g_midiIn = new RtMidiIn();
    g_midiIn->openPort(sel);
    g_midiIn->setCallback(&midiCallback);
    g_connected = true;
    SetWindowText(g_hwndButton, L"Disconnect");
    AddLog("Connected to : " + g_ports[sel]);
    SetStatus(L"Connected to selected MIDI port.");
}

void DisconnectMidi(HWND hwnd) {
    if (g_midiIn) { delete g_midiIn; g_midiIn = nullptr; }
    g_connected = false;
    SetWindowText(g_hwndButton, L"Connect");
    AddLog("MIDI disconnected.");
    SetStatus(L"Disconnected.");
}

void ShowAbout(HWND hwnd) {
    MessageBox(hwnd, L"MIDITypist\nModern MIDI Mapper\n\nMap MIDI keys to keyboard keys.\nDouble-click mappings to remove.\n(C) 2025", L"About", MB_OK | MB_ICONINFORMATION);
}

void RemoveSelectedMapping() {
    int sel = (int)SendMessage(g_hwndMappingList, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)g_mappings.size()) {
        g_mappings.erase(g_mappings.begin() + sel);
        AddLog("Mapping removed.");
        UpdateMappingList();
        SetStatus(L"Mapping removed.");
    }
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
                AddLog("Switched mappings for app: " + std::string(exe.begin(), exe.end()));
                SetStatus((std::wstring(L"Mappings auto-loaded for ") + exe).c_str());
                g_lastProfileLoadedForApp = exe;
            }
        }
        CloseHandle(hProc);
    }
}

// Settings dialog procedure
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
                    AddLog("App profile binding stored: " + std::string(exeName.begin(), exeName.end()));
                }
                CloseHandle(hProc);
            }
            break;
        }
        case IDC_BTN_CLEARLOG:
            g_log.clear();
            AddLog("Log cleared.");
            break;
        case IDC_BTN_CLEARMAPS:
            g_mappings.clear();
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int x = 20;
        int btn_w = 120, btn_h = 34, gap = 16;
        g_hwndPorts = CreateWindow(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            x, 24, 220, btn_h, hwnd, (HMENU)IDC_CB_PORTS, g_hInst, nullptr);
        SetUIFont(g_hwndPorts); x += 230;
        g_hwndButton = CreateWindow(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            x, 24, btn_w, btn_h, hwnd, (HMENU)IDC_BTN_CONNECT, g_hInst, nullptr);
        SetUIFont(g_hwndButton); x += btn_w + gap;
        g_hwndLearn = CreateWindow(L"BUTTON", L"Learn Mapping", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            x, 24, btn_w + 30, btn_h, hwnd, (HMENU)IDC_BTN_LEARN, g_hInst, nullptr);
        SetUIFont(g_hwndLearn); x += btn_w + 30 + gap;
        g_hwndSettings = CreateWindow(L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            x, 24, btn_w, btn_h, hwnd, (HMENU)IDC_BTN_SETTINGS, g_hInst, nullptr);
        SetUIFont(g_hwndSettings);

        g_hwndLog = CreateWindow(L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            20, 70, 480, 340, hwnd, (HMENU)IDC_LOGBOX, g_hInst, nullptr);
        SetUIFont(g_hwndLog);

        g_hwndMappingList = CreateWindow(L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_EXTENDEDSEL | WS_BORDER,
            520, 70, 540, 340, hwnd, (HMENU)IDC_LIST_MAPPINGS, g_hInst, nullptr);
        SetUIFont(g_hwndMappingList);

        g_hwndStatus = CreateWindow(STATUSCLASSNAME, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, g_hInst, nullptr);
        ScanMidiPorts(hwnd);
        AddLog("Ready.");
        SetStatus(L"Ready.");
        UpdateMappingList();

        SetTimer(hwnd, APP_PROFILE_TIMER, 1000, NULL);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_CONNECT) {
            if (!g_connected) ConnectMidi(hwnd);
            else DisconnectMidi(hwnd);
        }
        else if (LOWORD(wParam) == IDC_BTN_LEARN) {
            g_learning = true;
            g_learn_pending = { -1, -1, -1 };
            AddLog("Learning: Press a MIDI note (press key, not release) or CC...");
            SetStatus(L"Waiting for MIDI input...");
            SetFocus(hwnd);
        }
        else if (LOWORD(wParam) == IDC_BTN_SETTINGS) {
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_SETTINGS), hwnd, SettingsDlgProc);
        }
        break;
    case WM_KEYDOWN:
        if (g_learning && g_learn_pending.midi_type != -1) {
            g_learn_pending.key_vk = (int)wParam;
            g_mappings.push_back(g_learn_pending);
            std::stringstream ss;
            ss << "Mapped MIDI " << (g_learn_pending.midi_type == 0 ? "Note" : "CC")
                << " " << g_learn_pending.midi_num
                << " to key VK_" << g_learn_pending.key_vk;
            AddLog(ss.str());
            UpdateMappingList();
            SetStatus(L"Mapped and saved.");
            g_learning = false;
        }
        if (GetFocus() == g_hwndMappingList && wParam == VK_DELETE) {
            RemoveSelectedMapping();
        }
        break;
    case WM_TIMER:
        if (wParam == APP_PROFILE_TIMER) PerAppSwitchMapping();
        break;
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlType == ODT_BUTTON) {
            // Feedback for click: use lighter accent if pushed/selected/hovered
            BOOL pressed = (pDIS->itemState & ODS_SELECTED);
            HBRUSH hBrush = CreateSolidBrush(
                pressed ? ACCENT_COLOR_HOVER : ACCENT_COLOR
            );
            FillRect(pDIS->hDC, &pDIS->rcItem, hBrush);
            SetBkMode(pDIS->hDC, TRANSPARENT);
            SetTextColor(pDIS->hDC, BUTTON_TEXT);
            wchar_t btnText[64];
            GetWindowText(pDIS->hwndItem, btnText, 63);
            DrawText(pDIS->hDC, btnText, -1, &pDIS->rcItem,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(hBrush);
            return TRUE;
        }
        break;
    }
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH hBg = CreateSolidBrush(BACKGROUND_COLOR);
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);
        return 1;
    }
    case WM_SIZE:
        SendMessage(g_hwndStatus, WM_SIZE, 0, 0);
        break;
    case WM_CLOSE:
        if (g_hModernFont) DeleteObject(g_hModernFont);
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        if (g_midiIn) delete g_midiIn;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInstance;
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"MIDITypist";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"MIDITypist",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1140, 500,
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
