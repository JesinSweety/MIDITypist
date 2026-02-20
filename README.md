# MIDITypist: Professional MIDI-to-Desktop Automation Engine (v1.0)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/SamuelJoseph23/MIDITypist)](https://github.com/SamuelJoseph23/MIDITypist/releases)

MIDITypist is a high-performance Windows automation utility that translates musical MIDI data into system-level keyboard and mouse commands. By leveraging a hybrid C++/WebView2 architecture, it provides the low-latency response of a native MIDI engine with the high-fidelity design of a modern web frontend.

## 1. Installation & Requirements
*   **OS**: Windows 10 or Windows 11 (x64)
*   **Dependencies**: [WebView2 Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) (usually pre-installed on Windows 11).
*   **Hardware**: Any MIDI-compliant controller (USB or via Interface).

**Quick Start**:
1.  Download the [latest release](https://github.com/SamuelJoseph23/MIDITypist/releases).
2.  Extract the zip folder.
3.  Run `MIDITypist.exe`.
4.  Select your MIDI device and click **Connect**.

## 2. Core Features

### 2.1 Native MIDI Engine
*   **Low-Latency C++ Processing**: Built on a high-performance C++ backend for millisecond-accurate MIDI-to-input translation.
*   **Hardware Auto-Reconnect**: Intelligent background monitoring that automatically restores your device connection if it's transiently lost or replugged.
*   **Universal MIDI Support**: Works with any standard MIDI controller (Keyboards, Launchpads, Mixers).

### 2.2 Intelligent Automation
*   **Context-Aware Engine**: Automatically swaps MIDI profiles based on your active foreground application (e.g., Photoshop, Ableton, Chrome).
*   **Profile Management**: Save and share your mapping sets as portable `.json` files. Use the **Export/Import** buttons to backup your workflows or share them with the community.
*   **Native Learning Mode**: Uses a Low-Level Keyboard Hook (`WH_KEYBOARD_LL`) to capture mapping keys instantly without needing to focus the MIDITypist window.

### 2.3 Gestures & Chords
*   **Multi-Gesture Triggers**: Every MIDI note can perform 3 distinct actions: **Single Tap**, **Double Tap**, and **Long Hold**.
*   **Chord Mapping**: Group rapid note strikes into a single automation trigger (perfect for power chords or DAW shortcuts).
*   **Velocity Sensitivity**: Map different actions to light vs. hard strikes for dynamic control.

### 2.4 AI Assistant (Powered by Gemini)
*   **AI Desktop Automation**: Integrated **Google Gemini** support allows you to trigger intelligent prompts directly from your MIDI controller to perform complex text or system tasks.
*   **Smart HUD Overlay**: Visualize active mapping palettes and AI responses with a high-contrast HUD.

### 2.5 System Integration
*   **Tray Operation**: Runs silently in the system tray with real-time status dots (Green = Connected, Red = Needs Attention).
*   **Real-time Activity Log**: Monitor every MIDI signal and system event with a high-fidelity diagnostic log.
*   **Zero Admin Required**: Injects input at the user level, requiring no administrative privileges for standard operation.

## 3. Technical Architecture

The application utilizes a dual-layer hybrid architecture:

### 3.1 Backend (C++/Win32)
*   **MIDI Engine**: Low-latency real-time capture via the RtMidi library.
*   **Input Driver**: Uses the Win32 `SendInput` API for system-level automation.
*   **Capture Engine**: Utilizes Low-Level Keyboard Hooks (`WH_KEYBOARD_LL`) for reliable "Learning Mode" capture regardless of window focus.

### 3.2 Frontend (WebView2 / Chromium)
*   **Modern Interface**: A glassmorphic UI designed for high performance and visual clarity.
*   **Real-time Monitoring**: System logs and signal visualizers provide instant feedback on MIDI and automation activity.

## 4. Build Requirements (For Developers)

*   **Compiler**: Visual Studio 2022.
*   **SDKs**: Windows 10/11 SDK, Microsoft WebView2 SDK, and WIL (available via NuGet).
*   **Dependencies**: RtMidi (included in source).

## 5. Security and Permissions

MIDITypist requires standard user permissions to inject input. It does not require Administrative privileges unless it needs to interact with other elevated applications.

---
© 2026 Samuel Joseph S. Professional MIDI Automation.

## 6. Contributing

Contributions are what make the open-source community such an amazing place to learn, inspire, and create. Any contributions you make are **greatly appreciated**.

Please see `CONTRIBUTING.md` for details on our code of conduct and the process for submitting pull requests to us.

## 7. License

Distributed under the MIT License. See `LICENSE` for more information.

## 8. Support

If you find this project helpful, please give it a ⭐ on GitHub! For bugs and feature requests, please use the [Issue Tracker](https://github.com/SamuelJoseph23/MIDITypist/issues).