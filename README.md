# MIDITypist: Professional MIDI-to-Desktop Automation Engine (v1.0)

MIDITypist is a high-performance Windows automation utility that translates musical MIDI data into system-level keyboard and mouse commands. By leveraging a hybrid C++/WebView2 architecture, it provides the low-latency response of a native MIDI engine with the high-fidelity design of a modern web frontend.

## 1. Installation

Getting started with MIDITypist is simple:

1.  **Download**: Head to the [Releases](https://github.com/SamuelJoseph23/MIDITypist/releases) page and download `MIDITypist_v1.0_Windows.zip`.
2.  **Extract**: Unzip the folder to a location of your choice.
3.  **Run**: Launch `MIDITypist.exe`.
4.  **Connect**: Select your MIDI device in the settings and click **Connect**.

*Note: Microsoft Edge WebView2 Runtime is required (standard on most Windows 10/11 systems).*

## 2. Core Features

### 2.1 Advanced Mapping Engine
*   **Note-to-Key**: Map individual MIDI notes to single keys or complex modifier combinations (Ctrl, Shift, Alt).
*   **CC-to-Mouse**: Map continuous controllers (knobs/sliders) to mouse cursor movement, vertical scrolling, or proportional key-hold behaviors.
*   **Velocity Consistency**: Use "Velocity Zones" (Soft/Hard) to trigger different actions based on how hard you strike a key.
*   **Macro Support**: Trigger long strings of text or sequence-based keyboard macros with a single MIDI event.

### 2.2 Gesture & Chord Engine
*   **Chord Detection**: Group rapid note strikes into "Chords" (e.g., C-E-G) to create hundreds of additional mapping combinations.
*   **Multi-Gestures**: Assign different actions to **Single Tap**, **Double Tap**, and **Long Hold** on the same MIDI note.
*   **HUD Overlay**: Dedicated "Layer Keys" reveal a high-contrast HUD on your desktop, showing your active mapping palettes in real-time.

### 2.3 AI Assistant
*   **AI Desktop Automation**: Integrated **Google Gemini** support allows you to trigger intelligent prompts directly from your MIDI controller to perform complex text or system tasks.

### 2.4 Workflow Automation
*   **Context Awareness**: Automatically detects the active application (e.g., Photoshop, Ableton, Chrome) and swaps your MIDI profile to match.
*   **System Tray Integration**: Runs quietly in the background with auto-reconnect support for your hardware.

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