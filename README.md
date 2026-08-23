# Parfait

A local-first, open-source meeting notetaker for [Omarchy](https://omarchy.org) (Arch + Hyprland).
Record any meeting, transcribe it on-device, type rough cues while you listen, and let an LLM
merge the two into a clean note.

Qt 6 / QML / C++20. Audio via PipeWire, transcription via `whisper.cpp`, notes as plain Markdown
on disk.

> **Status — early alpha (v0.1).** The core loop works end to end and is exercised by a headless
> Docker test rig, but it has had far more hours in CI than on real hardware in real meetings.
> Expect rough edges: device selection, model quality trade-offs, and long-recording behaviour are
> all under-tested. File issues.

<!-- screenshot: main window -->

## What it is

Parfait copies the idea that makes Granola useful: **you should not be taking notes during a
meeting.** You jot fragments — a name, a number, "follow up re: pricing" — and afterwards an LLM
reconstructs the real note by reading your fragments against the full transcript. Your cues tell
the model *what mattered*; the transcript supplies the detail you did not write down.

The other half is capture. Parfait has **no meeting bot** and joins nothing. It taps two PipeWire
streams directly:

- **Mic** — your default source. Everything here is attributed to **Me**.
- **System audio** — the default sink's monitor, i.e. whatever the meeting app is playing.
  Everything here is attributed to **Them**.

That means it works with Zoom, Google Meet, Teams, Discord, a phone on speaker, a recorded talk in
a browser tab — anything that makes sound. No plugin, no calendar-bot invite, no vendor account.
Speaker attribution is free: it falls out of *which stream* the audio came from, not out of a
diarization model.

The retained recording is a stereo Ogg Opus file with **L = mic, R = system**, so the attribution
survives if you ever re-transcribe it later.

## Features (v0.1)

- **Dual-stream recording** — mic + system audio captured simultaneously via libpipewire, mixed to
  stereo Ogg Opus (~1 MB/min) in `~/Meetings/`.
- **Local streaming transcription** — `whisper.cpp` on a worker thread, VAD-gated windows, partials
  update live and finals persist. GPU via Vulkan when the build finds it.
- **Speaker turns** — with a tinydiarize (`*-tdrz`) model, the Them stream is split into turn
  indices (`Them 1`, `Them 2`, …) when the voice changes. See the caveat in
  [Speaker turns](#speaker-turns) — these are turn boundaries, not identities.
- **Notepad** — a minimal Markdown pane you type cues into while the meeting runs.
- **LLM enhance** — cues + transcript → structured Markdown, streamed in via SSE. Any
  OpenAI-compatible `/chat/completions` endpoint. Three built-in templates: `general`,
  `one-on-one`, `standup`.
- **Calendar** — ICS URL / CalDAV polling, upcoming events in the sidebar with a Record button,
  new notes pre-filled with title and attendees.
- **Search** — SQLite with FTS5 over titles, notes and transcripts (degrades to `LIKE` if your
  SQLite lacks FTS5). `note.md` on disk is the source of truth; the database is an index.
- **Live Omarchy theming** — reads `~/.config/omarchy/current/theme/colors.toml` and retints with
  an animated transition, either from a filesystem watch or from the theme-set hook.
- **In-app model downloader** — pick and fetch whisper models from Settings; they land in
  `~/.local/share/parfait/models/`.
- **Docker e2e rig** — a headless container that plays synthetic meeting audio into virtual
  PipeWire devices and asserts on the resulting transcript.

### Not done yet

Listed so you do not go looking: chat-with-transcript, a custom template editor, per-person
(identity) diarization, share-to integrations (Slack/Notion), tray/daemon mode, and OS keyring
storage for the API key.

## Privacy

- **Audio never leaves the machine.** Transcription is local `whisper.cpp`. There is no upload path
  for audio in the codebase.
- **Transcript and note text go only to the LLM endpoint you configure**, and only when you press
  Enhance. Configure a local endpoint and nothing leaves at all:

  ```
  base URL: http://localhost:11434/v1
  model:    llama3.1:8b
  ```

- **Calendar** fetches only from the ICS/CalDAV URL you enter.
- **API key storage caveat.** The key is currently stored in plain `QSettings`
  (`~/.config/parfait/parfait.conf`), *not* in libsecret/kwallet. It is readable by anything running
  as your user. Keychain support is planned; until then, prefer a scoped/revocable key, or use a
  local model and leave the key blank.

## Install

### Arch / Omarchy, from source

```bash
sudo pacman -S --needed qt6-base qt6-declarative pipewire libopusenc sqlite \
                        cmake ninja git base-devel
# optional: GPU transcription
sudo pacman -S --needed vulkan-icd-loader
```

```bash
git clone --recurse-submodules https://github.com/teevans/parfait.git
cd parfait
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/parfait
```

If you cloned without `--recurse-submodules`:

```bash
git submodule update --init third_party/whisper.cpp
```

`third_party/whisper.cpp` is optional at configure time — without it the build succeeds but uses a
stub transcriber, which is only useful for UI work.

To install system-wide:

```bash
sudo cmake --install build --prefix /usr
```

### AUR

A `PKGBUILD` for `parfait-git` lives in [`packaging/`](packaging/PKGBUILD). It is **not published to
the AUR yet**. To build it locally:

```bash
cd packaging && makepkg -si
```

### Models

Easiest path: open **Settings → Models** and download one in-app. They land in
`~/.local/share/parfait/models/`.

Manually, if you prefer — `ggml-small.en-tdrz.bin` is the tinydiarize model, and the only one that
produces speaker turns:

```bash
mkdir -p ~/.local/share/parfait/models
curl -L -o ~/.local/share/parfait/models/ggml-small.en-tdrz.bin \
  https://huggingface.co/akashmjn/tinydiarize-whisper.cpp/resolve/main/ggml-small.en-tdrz.bin
```

A smaller, faster, no-turns alternative:

```bash
curl -L -o ~/.local/share/parfait/models/ggml-base.en.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin
```

The active model path is remembered in `QSettings` under `transcribe/modelPath` and can be switched
live from Settings.

## Configure

Settings live in `~/.config/parfait/parfait.conf` and are editable from the in-app Settings sheet.

### LLM endpoint

Any OpenAI-compatible `/chat/completions` server. The base URL should *not* include the
`/chat/completions` suffix.

| Provider | Base URL | Model |
|---|---|---|
| OpenAI | `https://api.openai.com/v1` | `gpt-4o-mini` |
| Groq | `https://api.groq.com/openai/v1` | `llama-3.3-70b-versatile` |
| Ollama (local) | `http://localhost:11434/v1` | `llama3.1:8b` |
| llama.cpp server | `http://localhost:8080/v1` | *(whatever is loaded)* |

`fastModel` is an optional cheaper model used for short jobs like title suggestions.

```ini
[llm]
baseUrl=http://localhost:11434/v1
apiKey=
model=llama3.1:8b
fastModel=llama3.1:8b
```

### Calendar

Paste an ICS URL (Google "secret address in iCal format", Fastmail, iCloud, or a local file URL):

```ini
[calendar]
icsUrl=https://calendar.google.com/calendar/ical/…/basic.ics
```

Events are polled periodically; ones with a video link or multiple attendees show a Record button.

### Omarchy theme hook

Parfait watches `~/.config/omarchy/current/theme/colors.toml` and retints on its own with zero
setup. For the well-behaved path — an explicit retint on `omarchy-theme-set` — install the hook:

```bash
./omarchy/install-hook.sh
```

That writes `~/.config/omarchy/hooks/theme-set-parfait`, which calls:

```bash
parfait --retint
```

`--retint` talks to a running instance over a local socket and returns immediately; it is a no-op if
Parfait is not running.

On non-Omarchy systems Parfait falls back to a built-in dark palette.

## Usage

The flow, in full:

1. Start a meeting — from the calendar rail, or hit record and name it later.
2. **Type cues, not notes.** Fragments. `pricing → Ana`, `!! ship date slipped`, `ask about SSO`.
   The live transcript runs beside the notepad; you can collapse it.
3. Stop recording.
4. Press **Enhance**. Your cues plus the transcript go to your configured endpoint and the merged
   note streams back in. Accept it and it is written to disk.

Keyboard: `Ctrl+R` record, `Ctrl+E` enhance, `/` search.

### Speaker turns

Transcript lines are labelled by capture stream:

- **Me** — the mic stream. Always you.
- **Them** — the system-audio stream. Everyone on the other end, mixed together.

With a tinydiarize model loaded, Them lines additionally carry a **turn index** — `Them 1`,
`Them 2`, `Them 3` — which increments when whisper emits a speaker-change token.

**These indices are not identities.** They mark *that the voice changed*, not *who is speaking*.
`Them 3` may well be the same person as `Them 1`. Indices also restart within a transcription
window, so do not treat them as stable across a whole meeting. Real per-person diarization is
roadmap, not shipped. In practice the turn indices are useful for readability — they break a wall
of Them text into exchanges — and not much more.

### Where files land

```
~/Meetings/2026-08-23 Blue Harbor standup/
├── audio.ogg     # stereo Opus, L = mic, R = system
└── note.md       # the enhanced note, plain Markdown
```

`note.md` is the source of truth. It is greppable, syncable, and editable in any editor. The
database at `~/.local/share/parfait/parfait.db` holds segments and the FTS5 search index, and can be
deleted and rebuilt without losing your notes.

## Testing

The end-to-end rig runs the real `AudioEngine` → `TranscribeEngine` path headless inside Docker,
against virtual PipeWire devices, with synthesized speech played into both streams. No GUI, no host
audio hardware.

```bash
./docker/run.sh                       # single-speaker "Them" + espeak "Me" lines
./docker/run.sh --multi               # three-voice panel + tinydiarize, asserts turn indices
./docker/run.sh --youtube <URL>       # use real audio from a video as the Them track
./docker/run.sh --increments 3        # how many chunks of the Them track to run
```

`--multi` is the interesting one: it fails the run unless at least one increment sees two or more
distinct speaker turns on Them, and unless zero Me lines ever get a turn suffix. Output lands in
`build/e2e-out/`.

A passing increment looks roughly like:

```
Them#0 [00:02.1 → 00:06.4]  Good morning everyone. Thanks for joining the Blue Harbor standup.
Them#1 [00:07.0 → 00:11.8]  Morning. The backfill finished overnight with no errors at all.
Me     [00:12.6 → 00:15.9]  Thanks Dana. Quick question about the backfill.
Them#2 [00:17.2 → 00:21.5]  We processed just over four million documents end to end.
```

## Architecture

Single process. Audio and transcription run on worker threads; the UI stays on the main thread.
There is no daemon — the app must be running to record.

| Module | Path | Does |
|---|---|---|
| AudioEngine | `src/audio/` | Two libpipewire capture streams, 16 kHz mono rings for transcription + stereo Opus encode |
| TranscribeEngine | `src/transcribe/` | `whisper.cpp` context, windowed streaming, tinydiarize turn tracking |
| EnhanceService | `src/llm/` | Streaming SSE client for OpenAI-compatible endpoints; prompts in `Prompts.h` |
| CalendarService | `src/calendar/` | ICS/CalDAV polling, meeting-link detection |
| Library | `src/store/` | SQLite + FTS5 index, `~/Meetings/` file writing |
| ThemeService | `src/theme/` | Omarchy `colors.toml` parsing, file watch, live retint |
| ModelDownloader | `src/models/` | Whisper model catalog, resumable-safe downloads, active-model switching |
| MeetingController | `src/` | Wires record → transcribe → store → enhance |
| UI | `qml/` | `Main`, `Sidebar`, `MeetingView`, `NotePad`, `TranscriptView`, `EnhanceBar`, `EnhancedNote`, `SettingsView`, `VuMeter`, `EmptyState` |

Design notes, rationale, and the roadmap are in [DESIGN.md](DESIGN.md).

## Contributing

Issues and PRs welcome. A few things worth knowing before you start:

- Run `./docker/run.sh --multi` before opening a PR that touches audio or transcription — it is the
  only thing that catches stream-attribution and turn-index regressions.
- Keep `note.md` authoritative. Anything that makes the database the source of truth for note text
  is a design regression.
- QML files are added by glob; no manifest to update.
- The roadmap in DESIGN.md lists the obvious next pieces — chat-with-transcript, a template editor,
  identity diarization, keychain storage — if you are looking for somewhere to start.

## License

MIT — see [LICENSE](LICENSE).
