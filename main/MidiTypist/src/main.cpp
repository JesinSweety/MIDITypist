#include <windows.h>
#include <CommCtrl.h>
#include <vector>
#include <string>
#include <sstream>
#include "RtMidi.h"

#pragma comment(lib, "Comctl32.lib")

#define IDC_CB_PORTS 101
#define IDC_BTN_CONNECT 102
#define IDC_LOGBOX 103
#define IDC_BTN_LEARN 104
#define IDC_BTN_CLEARLOG 105
#define IDC_LIST_MAPPINGS 106
#define IDC_BTN_CLEARMAPS 107
#define IDC_STATUSBAR 108
#define IDC_ABOUT 109

HINSTANCE g_hInst;
HFONT g_uiFont = nullptr;
HWND g_hwndPorts, g_hwndButton, g_hwndLog, g_hwndLearn, g_hwndMappingList,
g_hwndStatus, g_hwndClearLog, g_hwndClearMaps, g_hwndAbout;
RtMidiIn* g_midiIn = nullptr;
std::vector<std::string> g_ports;
std::vector<std::string> g_log;
bool g_connected = false;

struct Mapping {
    int midi_type;   // 0=NoteOn, 1=CC
    int midi_num;
    int key_vk;
};
std::vector<Mapping> g_mappings;
bool g_learning = false;
Mapping g_learn_pending = { -1, -1, -1 };

void SetUIFont(HWND hwnd) {
    if (!g_uiFont) {
        LOGFONT lf = { 0 };
        NONCLIENTMETRICS ncm = { sizeof(NONCLIENTMETRICS) };
        SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        lf = ncm.lfMessageFont;
        lf.lfHeight = 16;
        g_uiFont = CreateFontIndirect(&lf);
    }
    SendMessage(hwnd, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
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
    MessageBox(hwnd, L"Modern MIDI Mapper\nby You\n\nMap MIDI keys to keyboard keys.\nDouble-click mappings to remove.\n(C) 2025", L"About", MB_OK | MB_ICONINFORMATION);
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);

        g_hwndPorts = CreateWindow(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            20, 20, 240, 28, hwnd, (HMENU)IDC_CB_PORTS, g_hInst, nullptr);
        SetUIFont(g_hwndPorts);

        g_hwndButton = CreateWindow(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE,
            270, 20, 90, 28, hwnd, (HMENU)IDC_BTN_CONNECT, g_hInst, nullptr);
        SetUIFont(g_hwndButton);

        g_hwndLearn = CreateWindow(L"BUTTON", L"Learn Mapping", WS_CHILD | WS_VISIBLE,
            370, 20, 130, 28, hwnd, (HMENU)IDC_BTN_LEARN, g_hInst, nullptr);
        SetUIFont(g_hwndLearn);

        g_hwndClearLog = CreateWindow(L"BUTTON", L"Clear Log", WS_CHILD | WS_VISIBLE,
            510, 20, 90, 28, hwnd, (HMENU)IDC_BTN_CLEARLOG, g_hInst, nullptr);
        SetUIFont(g_hwndClearLog);

        g_hwndClearMaps = CreateWindow(L"BUTTON", L"Clear Mappings", WS_CHILD | WS_VISIBLE,
            610, 20, 130, 28, hwnd, (HMENU)IDC_BTN_CLEARMAPS, g_hInst, nullptr);
        SetUIFont(g_hwndClearMaps);

        g_hwndAbout = CreateWindow(L"BUTTON", L"About", WS_CHILD | WS_VISIBLE,
            750, 20, 60, 28, hwnd, (HMENU)IDC_ABOUT, g_hInst, nullptr);
        SetUIFont(g_hwndAbout);

        g_hwndLog = CreateWindow(L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            20, 60, 480, 320, hwnd, (HMENU)IDC_LOGBOX, g_hInst, nullptr);
        SetUIFont(g_hwndLog);

        g_hwndMappingList = CreateWindow(L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_EXTENDEDSEL | WS_BORDER,
            520, 60, 420, 320, hwnd, (HMENU)IDC_LIST_MAPPINGS, g_hInst, nullptr);
        SetUIFont(g_hwndMappingList);

        g_hwndStatus = CreateWindow(STATUSCLASSNAME, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, g_hInst, nullptr);

        ScanMidiPorts(hwnd);
        AddLog("Ready.");
        SetStatus(L"Ready.");
        UpdateMappingList();
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
        else if (LOWORD(wParam) == IDC_BTN_CLEARLOG) {
            g_log.clear();
            AddLog("Log cleared.");
        }
        else if (LOWORD(wParam) == IDC_BTN_CLEARMAPS) {
            g_mappings.clear();
            UpdateMappingList();
            AddLog("All mappings cleared.");
            SetStatus(L"All mappings cleared.");
        }
        else if (LOWORD(wParam) == IDC_ABOUT) {
            ShowAbout(hwnd);
        }
        else if (LOWORD(wParam) == IDC_LIST_MAPPINGS && HIWORD(wParam) == LBN_DBLCLK) {
            RemoveSelectedMapping();
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
        // Optionally: handle Delete key press when mapping list is focused to delete mapping
        if (GetFocus() == g_hwndMappingList && wParam == VK_DELETE) {
            RemoveSelectedMapping();
        }
        break;
    case WM_SIZE:
        SendMessage(g_hwndStatus, WM_SIZE, 0, 0);
        break;
    case WM_CLOSE:
        if (g_uiFont) DeleteObject(g_uiFont);
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        if (g_midiIn) delete g_midiIn;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInstance;
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"MIDI Mapper Modern";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"MIDI Mapper Modern",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 470,
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
