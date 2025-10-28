# MIDITypist

**Modern Windows MIDI Mapper to Keyboard**

MIDITypist lets you map any MIDI note or control change to a keyboard key, so you can type, automate, or control applications instantly using your MIDI controller. Features a modern, intuitive GUI and live mapping management.

---

## Features

- Map MIDI notes and CC events to keyboard keys
- Fast typing and control for creative workflow, accessibility, and automation
- Clear, modern Windows interface with interactive mapping list
- Live log display of MIDI events and mappings
- Double-click or use Delete key to remove mappings instantly
- Save/load mappings and future installer support

---

## How to Build

1. Clone the repository:

    ```
    git clone https://github.com/yourusername/miditypist.git
    ```

2. Open `miditypist.sln` in Visual Studio (Community or higher).
3. (Optional) Make sure RtMidi is provided in `/libs` or linked in your project.
4. Build the project (`Release` or `Debug`).
5. Run the resulting executable from the `Release` or `Debug` folder.

---

## How to Use

- Connect a MIDI keyboard/controller.
- Select your MIDI device.
- Click **"Learn Mapping"** > play a MIDI key > press your desired keyboard key.
- Repeat to build your mappings.  
- Double-click mappings to remove.  
- See live MIDI event log and statuses.

---

## Contributing

Pull requests and issues are welcome. 
For major changes, open an issue first to discuss what you’d like to change.

---

## License

This project is licensed under the MIT License.

---

## Credits

- Built by SamuelJoseph23
- Uses [RtMidi](https://github.com/thestk/rtmidi) for cross-platform MIDI input.