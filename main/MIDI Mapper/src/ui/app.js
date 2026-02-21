/**
 * MIDITypist - Core Frontend Engine
 * Handles State, Bridge Communication, and UI Orchestration.
 */

// --- State ---
let mappings = [];
let currentView = 'mappings';
let learnPhase = 0; // 0: Idle, 1: Waiting for MIDI, 2: Waiting for Key

// --- Bridge: Send commands to C++ ---
function send(action, data = {}) {
    if (window.chrome?.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ action, ...data }));
    }
}

// --- Bridge: Receive messages from C++ ---
if (window.chrome?.webview) {
    window.chrome.webview.addEventListener('message', e => {
        const msg = e.data;
        switch (msg.type) {
            case 'mappings': updateMappings(msg.mappings); break;
            case 'midi_note': handleMidiEvent('Note', msg.note, msg.velocity); break;
            case 'midi_cc': handleMidiEvent('CC', msg.cc, msg.value); break;
            case 'status': setStatus(msg.text); break;
            case 'app_changed': updateContext(msg.app, msg.title); break;
            case 'learn_phase':
                learnPhase = msg.phase;
                const promptEl = document.getElementById('learnPrompt');
                if (promptEl) promptEl.textContent = msg.text;
                break;
            case 'learn_done':
                learnPhase = 0;
                const overlay = document.getElementById('learnOverlay');
                if (overlay) overlay.style.display = 'none';
                break;
            case 'log': addLog(msg.text, msg.category); break;
            case 'run_ai': handleAiRequest(msg.prompt); break;
            case 'ports': updatePorts(msg.ports, msg.selected); break;
            case 'config': syncConfig(msg.config); break;
        }
    });
}

// --- UI Logic ---
function setView(viewId) {
    document.querySelectorAll('.view-section').forEach(v => v.classList.remove('active'));
    document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));

    const targetView = document.getElementById('view-' + viewId);
    if (targetView) targetView.classList.add('active');

    // Update nav item state (find by text or id if we add them)
    document.querySelectorAll('.nav-item').forEach(n => {
        if (n.textContent.toLowerCase().includes(viewId)) n.classList.add('active');
    });

    currentView = viewId;
}

function toggleTheme() {
    const html = document.documentElement;
    const theme = html.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
    html.setAttribute('data-theme', theme);
    localStorage.setItem('miditypist-theme', theme);
}

function updateMappings(list) {
    mappings = list;
    const grid = document.getElementById('mapGrid');
    if (!grid) return;
    grid.innerHTML = '';

    mappings.forEach((m, i) => {
        const card = document.createElement('div');
        card.className = 'mapping-card';
        card.onclick = () => openEditor(i);

        let target = 'HUD';
        if (m.midi_type === 0) target = 'Key ' + m.key_vk;
        else if (m.midi_type === 4) target = 'Macro';
        else if (m.midi_type === 5) target = 'AI';
        else if (m.midi_type === 2) target = 'Chord Key ' + m.key_vk;

        let gesture = m.gesture_id === 1 ? 'DBL' : (m.gesture_id === 2 ? 'HLD' : 'TAP');
        let titleLine = m.midi_type === 2 ? `Chord [${(m.midi_chord || []).join(',')}]` : `${m.midi_type === 1 ? 'CC' : 'Note'} ${m.midi_num}`;

        card.innerHTML = `
      <div style="display:flex; justify-content:space-between; align-items:flex-start;">
        <span style="font-weight:700; font-size:14px;">${titleLine}</span>
        <div class="badge">${gesture}</div>
      </div>
      <div style="font-size:18px; font-weight:600; color:var(--accent);">${target}</div>
      <div class="mapping-footer" style="display:flex; justify-content:space-between; align-items:center; margin-top:8px;">
        <div class="badge-row">
           ${m.app_pattern ? `<span class="context-pill">${m.app_pattern}</span>` : ''}
        </div>
        <button class="btn" style="padding:4px; color:var(--error);" onclick="event.stopPropagation(); deleteMapping(${i})">
          <svg style="width:14px; height:14px;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>
        </button>
      </div>
    `;
        grid.appendChild(card);
    });
}

function handleMidiEvent(type, num, val) {
    if (val > 0) {
        addLog(`${type} ${num} (Val: ${val})`, 'midi-active');
        const key = document.getElementById(`key-${num}`);
        if (key) key.classList.add('active');
    } else {
        const key = document.getElementById(`key-${num}`);
        if (key) key.classList.remove('active');
    }
}

// --- Piano Widget ---
function initPiano() {
    const dashboard = document.getElementById('pianoDashboard');
    if (!dashboard) return;
    dashboard.innerHTML = '';

    for (let i = 0; i < 128; i++) {
        const key = document.createElement('div');
        key.className = 'piano-key';
        key.id = `key-${i}`;

        // Simple black key logic
        const octave = i % 12;
        if ([1, 3, 6, 8, 10].includes(octave)) {
            key.classList.add('black');
        }

        dashboard.appendChild(key);
    }
}

function clearMappings() {
    if (confirm("Are you sure you want to clear all mappings? This cannot be undone.")) {
        send('clear_mappings');
    }
}

function addLog(text, cat) {
    const body = document.getElementById('logBody');
    if (!body) return;
    const div = document.createElement('div');

    let color = 'var(--text-secondary)';
    if (cat === 'error') color = 'var(--error)';
    if (cat === 'midi-active' || cat === 'mapping') color = 'var(--accent)';

    div.style.color = color;
    div.textContent = `[${new Date().toLocaleTimeString()}] ${text}`;
    body.appendChild(div);
    body.scrollTop = body.scrollHeight;
    const countEl = document.getElementById('logCount');
    if (countEl) countEl.textContent = body.children.length;
}

function updateContext(app, title) {
    if (title === "Task Switching") return;
    if (app === "explorer.exe" && !title) return;

    const appEl = document.getElementById('contextApp');
    const titleEl = document.getElementById('contextTitle');
    if (appEl) appEl.textContent = app || 'Desktop';
    if (titleEl) titleEl.textContent = title || 'Untitled';

    addLog(`Context: ${app} | ${title}`, 'system');
}

function setStatus(text) {
    const label = document.getElementById('statusLabel');
    const dot = document.getElementById('statusDot');
    const btn = document.getElementById('btnConnect');

    if (label) label.textContent = text;
    if (dot) dot.style.background = (text.includes('Connected') || text.includes('Ready')) ? 'var(--success)' : 'var(--error)';
    if (btn) btn.textContent = text.includes('Connected') ? 'Disconnect' : 'Connect';
}

function updatePorts(ports, selectedIdx) {
    const sel = document.getElementById('selectMidiPort');
    if (!sel) return;
    sel.innerHTML = '';
    ports.forEach((p, i) => {
        const opt = document.createElement('option');
        opt.value = i;
        opt.textContent = p;
        if (i === selectedIdx) opt.selected = true;
        sel.appendChild(opt);
    });
}

function syncConfig(cfg) {
    const ids = {
        'checkReconnect': cfg.auto_reconnect,
        'checkAppSwitch': cfg.app_switching,
        'checkVelocity': cfg.velocity_zones,
        'checkTray': cfg.minimize_to_tray,
        'inputApiKey': cfg.ai_api_key || '',
        'inputAiGlobal': cfg.ai_global_prompt || ''
    };

    for (const [id, val] of Object.entries(ids)) {
        const el = document.getElementById(id);
        if (el) {
            if (el.type === 'checkbox') el.checked = val;
            else el.value = val;
        }
    }
}

// --- Editor Functions ---
let activeIdx = -1;
function openEditor(i) {
    activeIdx = i;
    const m = mappings[i];
    const modal = document.getElementById('modalEditor');
    if (!modal) return;
    modal.style.display = 'flex';

    document.getElementById('editMidiType').value = m.midi_type;
    document.getElementById('editKeyVk').value = m.key_vk;
    document.getElementById('editGestureId').value = m.gesture_id || 0;
    document.getElementById('editMacroText').value = m.macro_text || '';
    document.getElementById('editAiPrompt').value = m.ai_prompt || '';
    document.getElementById('editMidiChord').value = (m.midi_chord || []).join(', ');
    document.getElementById('editAppPattern').value = m.app_pattern || '';
    document.getElementById('editTitlePattern').value = m.title_pattern || '';
    toggleEditFields();
}

function closeEditor() {
    const modal = document.getElementById('modalEditor');
    if (modal) modal.style.display = 'none';
}

function toggleEditFields() {
    const type = document.getElementById('editMidiType').value;
    const fields = {
        'editFieldMacro': type == 4,
        'editFieldAi': type == 5,
        'editFieldKey': (type != 4 && type != 5),
        'editFieldChord': type == 2
    };

    for (const [id, visible] of Object.entries(fields)) {
        const el = document.getElementById(id);
        if (el) el.style.display = visible ? 'block' : 'none';
    }
}

function saveEdit() {
    const chordStr = document.getElementById('editMidiChord').value;
    const chordArr = chordStr.split(',').map(s => parseInt(s.trim())).filter(n => !isNaN(n));

    send('update_mapping', {
        index: activeIdx,
        midi_type: parseInt(document.getElementById('editMidiType').value),
        key_vk: parseInt(document.getElementById('editKeyVk').value),
        gesture_id: parseInt(document.getElementById('editGestureId').value),
        macro_text: document.getElementById('editMacroText').value,
        ai_prompt: document.getElementById('editAiPrompt').value,
        midi_chord: chordArr,
        app_pattern: document.getElementById('editAppPattern').value,
        title_pattern: document.getElementById('editTitlePattern').value
    });
    closeEditor();
}

// --- Actions ---
function startLearn() {
    learnPhase = 1;
    const prompt = document.getElementById('learnPrompt');
    if (prompt) prompt.textContent = "Waiting for MIDI...";
    const overlay = document.getElementById('learnOverlay');
    if (overlay) overlay.style.display = 'flex';
    send('start_learn');
}

function cancelLearn() {
    learnPhase = 0;
    send('cancel_learn');
    const overlay = document.getElementById('learnOverlay');
    if (overlay) overlay.style.display = 'none';
}

function addMapping() { send('add_mapping'); }
function deleteMapping(i) { send('delete_mapping', { index: i }); }
function loadProfile() { send('load_profile'); }
function saveProfile() { send('save_profile'); }
function clearLog() { const log = document.getElementById('logBody'); if (log) log.innerHTML = ''; }
function toggleConnect() {
    const portEl = document.getElementById('selectMidiPort');
    if (!portEl) return;
    const port = parseInt(portEl.value);
    send('toggle_connect', { port });
}

function updateSettings() {
    send('update_config', {
        auto_reconnect: document.getElementById('checkReconnect').checked,
        app_switching: document.getElementById('checkAppSwitch').checked,
        minimize_to_tray: document.getElementById('checkTray').checked,
        velocity_zones: document.getElementById('checkVelocity').checked,
        ai_api_key: document.getElementById('inputApiKey').value,
        ai_global_prompt: document.getElementById('inputAiGlobal').value
    });
}

async function handleAiRequest(prompt) {
    const key = document.getElementById('inputApiKey').value;
    const global = document.getElementById('inputAiGlobal').value;
    if (!key) { addLog("AI Error: No API Key", "error"); return; }

    addLog("AI Thinking (Gemini Flash 1.5)...", "system");

    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 15000);

    try {
        const fullPrompt = global.replace('{prompt}', prompt);
        const url = `https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key=${key}`;

        const resp = await fetch(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            signal: controller.signal,
            body: JSON.stringify({
                contents: [{
                    parts: [{ text: fullPrompt }]
                }]
            })
        });

        clearTimeout(timeoutId);

        if (!resp.ok) {
            const errorJson = await resp.json().catch(() => ({}));
            const errorMsg = errorJson.error?.message || `HTTP ${resp.status}`;
            throw new Error(errorMsg);
        }

        const json = await resp.json();
        const result = json.candidates?.[0]?.content?.parts?.[0]?.text || "";
        addLog("AI Action: " + result, "system");
        send('simulate_text', { text: result });
    } catch (err) {
        clearTimeout(timeoutId);
        if (err.name === 'AbortError') {
            addLog("AI Failed: Request timed out (15s)", "error");
        } else {
            addLog("AI Failed: " + err.message, "error");
        }
    }
}

// Initial Boot
document.addEventListener('DOMContentLoaded', () => {
    const savedTheme = localStorage.getItem('miditypist-theme');
    if (savedTheme) document.documentElement.setAttribute('data-theme', savedTheme);
    initPiano();
    send('init');
});
