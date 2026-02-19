# MIDITypist: Professional MIDI-to-Desktop Automation Engine

MIDITypist is a high-performance Windows automation utility that translates musical MIDI data into system-level keyboard and mouse commands. By leveraging a hybrid C++/WebView2 architecture, it provides the low-latency response of a native MIDI engine with the high-fidelity design of a modern web frontend.

## 1. Technical Architecture

The application utilizes a dual-layer hybrid architecture:

### 1.1 Backend (C++/Win32)
*   **MIDI Engine**: Built on the RtMidi library for real-time, low-latency MIDI message capture.
*   **Automation Driver**: Utilizes the Win32 `SendInput` API for system-level injection of keystrokes and mouse events, bypassing application-level sandbox restrictions.
*   **Message Bridge**: Implements a bidirectional JSON-based communication layer via Microsoft WebView2 to synchronize engine state with the UI.
*   **Process Monitor**: Uses Win32 hooks (`SetWinEventHook`) to monitor foreground window changes for automatic profile switching.

### 1.2 Frontend (WebView2 / Chromium)
*   **Modern Interface**: A glassmorphic UI inspired by macOS Sonoma, utilizing advanced CSS3 techniques like `backdrop-filter: blur()`.
*   **Visualization**: Real-time SVG-based piano roll and system log for signal monitoring.
*   **HUD Layer**: A transient, borderless overlay mode for showing active mapping palettes during live workflows.

## 2. Core Features

### 2.1 Mapping Engine
*   **Note-to-Key**: Map individual MIDI notes to single keys or complex modifier combinations (Ctrl, Shift, Alt).
*   **CC-to-Mouse**: Map continuous controllers (knobs/sliders) to mouse cursor movement, vertical scrolling, or proportional key-hold behaviors.
*   **Velocity Sensitivity**: Define "Soft" and "Hard" velocity zones to trigger different actions on a single note strike.

### 2.2 Advanced Gesture Engine (v4.0)
*   **Chord Detection**: A pulse-based collector groups rapid note strikes into "Chords" (e.g., C-E-G), allowing for significantly more mapping combinations.
*   **Layer Keys (HUD)**: Specific MIDI buttons can be designated as "Layer Keys." Holding a Layer Key reveals a high-contrast HUD on the desktop, displaying all active mappings in that layer.

### 2.3 Workflow Automation
*   **Per-App Auto-Switching**: Automatically detects the active application (e.g., Photoshop, VS Code, Ableton) and swaps the MIDI profile to match the user's current task.
*   **System Tray Integration**: Designed to run seamlessly in the background with auto-reconnect support for MIDI devices.

## 3. Project Structure

*   `/main/MIDI Mapper/src/main.cpp`: Core C++ engine and WebView2 hosting logic.
*   `/main/MIDI Mapper/src/ui/index.html`: Unified UI containing HTML, CSS, and JavaScript.
*   `/main/x64/Debug/`: Default output directory for build artifacts and runtime configuration.
*   `config.json`: Master configuration file for application settings and profile routing.

## 4. Build Requirements

*   **Compiler**: Visual Studio 2022 (v143 toolset recommended).
*   **SDKs**:
    *   Windows 10/11 SDK.
    *   Microsoft WebView2 SDK (via NuGet).
    *   Microsoft Windows Implementation Library (WIL, via NuGet).
*   **Runtime**: WebView2 Runtime (Included in modern Windows/Edge installations).

## 5. Configuration

Mappings are stored in JSON format with the following structure:
*   `midi_type`: 0 (Note), 1 (CC), 2 (Chord), 3 (LayerKey).
*   `midi_num`: MIDI note or CC number.
*   `midi_chord`: Array of notes defining a chord gesture.
*   `key_vk`: Virtual Key code for automation.
*   `modifiers`: Bitmask for Ctrl(1), Shift(2), Alt(4).

## 6. Security and Permissions

MIDITypist requires standard user permissions to inject input. It does not require Administrative privileges unless it needs to interact with other elevated applications.