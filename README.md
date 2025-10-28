# MIDITypist

**A Modern Windows MIDI-to-Keyboard Mapper with Advanced Features and Clean UI**

MIDITypist lets you map MIDI notes or controllers from any device to regular PC keyboard keys, enabling creative typing, automation, and accessibility workflows. With the latest update, MIDITypist brings a modern, clean Win32 interface and extensive new controls for professional and fun MIDI mapping.

---

## **Features**

- **Clean, Modern UI:**  
  Flat accent-colored buttons, wide spacing, custom Segoe UI font, and grouped dialogs for all settings.
- **Live MIDI Mapping:**  
  Instantly bind any MIDI note or controller (CC) to a keyboard key.
- **Profile Management (Save/Load):**  
  Easily save, load, and switch between different mapping sets.
- **Per-Application Profiles:**  
  Automatically activate the right mapping profile based on the focused application (perfect for DAWs, games, or custom workflows).
- **Key Mapping Editor Enhancements:**  
  Remove or reprioritize mappings with double-click or delete.
- **Grouped "Settings" Dialog:**  
  Access profile functions, clearing, and about info from a focused modal window, keeping the main workspace uncluttered.
- **Visual Button Feedback:**  
  All toolbar buttons change color clearly on click or press.
- **Live Log & Status Bar:**  
  See MIDI events, status changes, and mapping edits in real time.
- **Portable:**  
  No installation required; just open the EXE.

---

## **How to Use**

1. **Connect** your MIDI controller/device.
2. **Select** your MIDI port, click "Connect".
3. Use "Learn Mapping" to assign keys:  
   - Press desired MIDI key or control  
   - Press target keyboard key  
   - Mapping is live!
4. Use the "Settings" button for advanced functions:  
   - Save/Load mapping profiles  
   - Bind mappings to specific applications  
   - Clear mappings/log  
   - About dialog.

> **Tip:** Double-click a mapping or press Delete to remove.

---

## **Building**

MIDITypist uses:
- Standard Win32 C++ (C++17 or C++20 recommended)
- [RtMidi](https://github.com/thestk/rtmidi) for MIDI input
- [nlohmann/json](https://github.com/nlohmann/json) for profile save/load (include `json.hpp`)
- Dialog and window resources in `.rc` files

**Steps:**
1. Clone the repository.
2. Open the solution/project in Visual Studio.
3. Build (`Release` recommended).
4. Run `MIDITypist.exe`.

---

## **License**

Licensed under the MIT License.

---

## **Contributing**

Pull requests, bug reports, and feature ideas are welcome.
