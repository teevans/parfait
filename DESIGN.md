# Gromarch — an open-source Granola for Omarchy

A local-first meeting notepad for Linux/Hyprland: capture any meeting's audio, transcribe it
on-device, type rough cues during the call, and let an LLM merge your cues with the transcript
into polished notes. Qt Quick UI that natively follows the active Omarchy theme.

**Stack decisions (locked):**
- C++20 / Qt 6 (Qt Quick + QML), CMake
- Transcription: local, streaming, via `whisper.cpp` (linked as a library)
- AI features: any OpenAI-compatible endpoint (base URL + key + model) — covers OpenAI, Groq,
  OpenRouter, Ollama/llama.cpp, and Claude via compatible proxies
- v1 scope: record + live transcribe + notepad + enhance, calendar integration, retained audio

---

## 1. Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ QML UI (Qt Quick)                                           │
│  MeetingList · NotePad · LiveTranscript · Player · Settings │
├─────────────────────────────────────────────────────────────┤
│ C++ Application core (QObject models exposed to QML)        │
│  MeetingController · Library (SQLite) · ThemeService        │
│  CalendarService · EnhanceService (LLM client)              │
├──────────────┬──────────────────────┬───────────────────────┤
│ AudioEngine  │ TranscribeEngine     │ Storage               │
│ (PipeWire)   │ (whisper.cpp, own    │ SQLite (FTS5) +       │
│ mic + system │ thread, streaming)   │ ~/Meetings/ files     │
│ audio taps   │                      │ (ogg + md)            │
└──────────────┴──────────────────────┴───────────────────────┘
```

Single process. Audio and transcription run on worker threads; UI stays on the main thread.
No daemon in v1 — the app must be running to record (a background/tray mode can come later).

### AudioEngine (PipeWire)
- Two capture streams via libpipewire:
  1. **Mic** — default source (respects WirePlumber default; selectable in settings)
  2. **System audio** — capture sink monitor (what the meeting app plays = the other
     participants). This works with Zoom, Meet-in-browser, Teams, Discord, anything.
- Each stream: 16 kHz mono f32 ring buffer → transcription; both streams also mixed/encoded
  to **Ogg Opus** on disk (retained recording, ~1 MB/min). Stereo file: L = mic, R = system,
  so re-transcription later keeps speaker attribution.
- VU levels pushed to UI at ~15 Hz.

### TranscribeEngine (whisper.cpp)
- One whisper context, two logical streams processed in windowed chunks (VAD-gated,
  ~5–8 s windows with overlap trim — the proven whisper.cpp streaming pattern).
- **Speaker attribution for free:** text from the mic stream = "Me", from the system stream
  = "Them". No diarization model needed in v1. (Per-participant diarization is a v2 idea.)
- Default model `ggml-small.en` (or `base` for low-end); GGUF models downloaded on first run
  into `~/.local/share/gromarch/models/` with an in-app picker. GPU via Vulkan when available.
- Emits `TranscriptSegment { stream, t0, t1, text, final }` — partials update the live view,
  finals are persisted.

### EnhanceService (LLM)
- Plain HTTPS via QNetworkAccessManager to `{base_url}/chat/completions`, streaming SSE.
- Jobs:
  - **Enhance notes** — system prompt + template + user's raw cues + transcript → structured
    Markdown. User's cues are explicitly weighted as "what mattered".
  - **Auto summary / action items** — runs on meeting end if no cues were typed.
  - (v1.x) **Chat with transcript** — same client, conversation panel.
- Config: base URL, API key (stored in `libsecret`/kwallet via QtKeychain, fallback to
  config file with a warning), model name, optional second "fast model" for titles.
- Nothing but transcript/notes text ever leaves the machine; audio never does.

### CalendarService
- v1: **CalDAV + ICS** (covers Google/Fastmail/iCloud via app passwords, and local khal
  setups). Poll upcoming events every 5 min.
- Meeting detection: event within ±10 min that has a video link (regex for
  zoom.us / meet.google.com / teams.microsoft.com / around / jitsi) or ≥2 attendees.
- Behavior: desktop notification "Standup starts in 2 min — record?" (via mako, so it's
  themed too) with an action button; new notes are pre-filled with title + attendees + event
  link. Optional auto-record toggle.

### Storage
- `~/.local/share/gromarch/gromarch.db` (SQLite, WAL):
  - `meetings(id, title, started_at, ended_at, calendar_uid, attendees_json, audio_path,
    notes_md, enhanced_md, template_id, state)`
  - `segments(meeting_id, stream, t0, t1, text)` + **FTS5** index over segments and notes
    → instant full-text search across all meetings
- Files under `~/Meetings/<yyyy-mm-dd> <title>/`: `audio.ogg`, `note.md` (enhanced note,
  written on save — your notes are always plain Markdown on disk, greppable and syncable,
  DB is the index not the source of truth for notes).

---

## 2. Omarchy theming

Goal: Gromarch looks like it shipped with the OS and retints live on `omarchy-theme-set`.

- **ThemeService (C++)** reads `~/.config/omarchy/current/theme/colors.toml` (semantic keys:
  `background`, `dark_background`, `darker_background`, `lighter_background`, `foreground`
  variants, `accent`, `selection`, `muted`, named colors; legacy `bg`/`fg` fallbacks).
- Exposed to QML as a `Theme` singleton (`Theme.background`, `Theme.accent`, …) with
  `Behavior on color` transitions — theme switches animate instead of flashing.
- **Live retint, two mechanisms (both):**
  1. `QFileSystemWatcher` on the `current/theme` symlink + colors.toml — works with zero setup.
  2. Ship `omarchy/gromarch.tpl` + an install script that registers a retint command
     (`gromarch --retint`, delivered over the app's local socket) in the theme-set hook /
     `post_theme_commands` — the "proper" Omarchy citizen path.
- Fallback palette (for non-Omarchy Linux): built-in Tokyo Night-ish default, or point
  `GROMARCH_THEME` at any colors.toml.
- Typography/spacing follow Omarchy conventions: mono accents (CaskaydiaMono) for
  timestamps/metadata, the UI font for prose, flat surfaces, 1px `lighter_background`
  borders, `accent` used sparingly (record indicator, links, active states).

---

## 3. UI (three surfaces, keyboard-first)

**Library** — left sidebar list of meetings (grouped Today/Yesterday/…), global fuzzy
search (`/`), calendar rail showing today's upcoming events each with a `Record` button.

**Meeting view** — the core screen, two panes:
- **Left: Notepad.** A minimal Markdown editor you type cues into during the call.
  This is the Granola trick: you write *fragments*, not notes.
- **Right: Live transcript.** Streaming segments, "Me"/"Them" colored with `accent` vs
  `muted`, auto-follow with scroll-lock. Collapsible.
- Top bar: title (editable, AI-suggested after the call), timer, VU meters, big record
  toggle. Bottom: after the call ends → **`Enhance` button** (streams the merged note in,
  diff-style, with one-key accept). Playback mode: click any transcript segment to seek
  the retained audio; audio position highlights the current segment.

**Settings** — audio devices, model picker/downloader, LLM endpoint, calendar accounts,
retention policy (keep audio forever / 30 days / delete after transcription).

Keyboard: `Super`-friendly, vim-ish where sane (`Ctrl+R` record, `Ctrl+E` enhance,
`/` search, `j/k` list nav). Also a CLI: `gromarch record --title "Standup"` for
scripting/Waybar integration, and a tiny Waybar module showing a red dot while recording.

---

## 4. Packaging

- `PKGBUILD` → AUR (`gromarch`, `gromarch-git`). Deps: qt6-base, qt6-declarative,
  pipewire, libsecret; whisper.cpp vendored as a submodule (built with Vulkan when present).
- License: MIT. Repo layout: `src/{audio,transcribe,llm,calendar,store,ui}/`, `qml/`,
  `omarchy/` (tpl + hook installer), `packaging/`.

## 5. Roadmap

| Milestone | Contents |
|---|---|
| **0.1 recorder** | PipeWire dual capture, Opus file, live whisper transcript, SQLite, minimal QML shell with Omarchy colors |
| **0.2 notepad** | Meeting view, notepad + live transcript panes, enhance via LLM endpoint, templates hardcoded (general/1-1/standup) |
| **0.3 the citizen** | Full theming polish + retint hook, calendar (CalDAV) + notifications, search (FTS5), playback-synced transcript, Waybar module |
| **1.0** | Settings UI, model downloader, keychain, retention policies, AUR release |
| **v2 ideas** | Chat with transcript(s), custom template editor, per-speaker diarization, share-to (Slack/Notion via webhooks), tray/daemon mode, transcript translation |

## 6. Open questions (non-blocking)

- Whisper model default: `small.en` quality vs `base.en` latency — decide after benchmarking on your hardware.
- Enhance prompt design: single-shot merge vs two-pass (outline from cues → fill from transcript). Start single-shot.
- Whether `note.md` should embed the transcript as a collapsible section for grep-ability.
