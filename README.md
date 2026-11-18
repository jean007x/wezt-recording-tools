# Wezt Recording Tools

An open-source, clean-room digital audio workstation for Windows and macOS, aiming at the
multitrack recording, editing and mixing workflow of professional DAWs plus the creative
tooling Logic Pro users expect.

Wezt Recording Tools is an independent project. It contains no Avid or Apple code, assets,
names, icons or UI text and is not affiliated with either company. See
`docs/CLEAN_ROOM_POLICY.md`.

## Status (v0.7)

Developed on Linux; GitHub Actions now builds and tests it on real Windows (MSVC) and macOS (Apple clang) runners too.
Verified there: the whole library and every test (27) on Linux, Windows and macOS; the full app with Dear ImGui and GLFW
compiles and its UI test passes on all three; real VST3 (the Steinberg SDK's sample plug-ins) and CLAP plug-ins are hosted
on all three; Linux also passes clang, AddressSanitizer + UBSan and ThreadSanitizer builds. Not verified: the window on a real
display, sound on real audio hardware (WASAPI/CoreAudio against a real device; the runners have none), a real MIDI keyboard,
commercial plug-ins, a real Claude Desktop session. The per-feature list, including everything that is
not built, is `docs/FEATURE_COVERAGE.md`.

| Area | State |
|---|---|
| Engine, graph, transport, recording to BWF/WAV; session, mixer, undo/redo, automation, VCA, memory locations | works, tested (headless) |
| Audio I/O: WASAPI (exclusive and shared), CoreAudio (hog mode) | written; **simulated** on Linux |
| VST3 and CLAP hosting, 32-bit bridge, plugin scanner/installer | works, tested with real SDK plugins; a real 32-bit helper is untested |
| AAX, RTAS, TDM | not hostable (licensing, DSP hardware); matched to VST3/CLAP/AU twins |
| AU, VST2 | scanned, no loader yet |
| Editing: Elastic Audio, Beat Detective, strip silence, clip gain, clips list, playlists/comping | works, tested |
| MIDI: piano roll, score window, MIDI editing, live play through instruments | works, tested; hardware MIDI keyboard input (device chooser in the Piano Roll, sustain, pitch bend, mod wheel): core tested with a fake device; WinMM/CoreMIDI backends never compiled; never tried with a real keyboard |
| Instruments: Sampler, Wezt Synth | works, tested |
| Plugin parameter automation (record, play, undo) and the universal AI-agent plugin interface (CLAP `wezt.agent/1`) | works, tested; effects are automated in pieces of at most 32 frames and exactly at breakpoints, instruments once per block; lanes can be drawn in the Plugin Parameters window |
| Local assistant, mix assistant, mastering, tutor, music generation, audio-to-MIDI, stem splitting | works, tested; no cloud |
| Claude connector (MCP, `wezt-mcp`): build a whole song, sing, voice banks, plugin control | works, tested over stdio; a real Claude Desktop session is untested |
| Singing from lyrics, voice model, voice bank from your own recordings (`wezt_voice_takes`) | works, tested; judged by measurement, and by the person it was built for |
| Immersive audio: layouts, VBAP, binaural, loudness, ADM BWF | works, tested; not routed into live audio |
| User interface (Dear ImGui, C++) | logic and layout tested headless (CPU snapshots); the whole UI library compiles against Dear ImGui 1.91.9; the GLFW window has never been run here |
| AAF/OMF, Extensions API, Speech-to-Text, Video, installers | not built |

### Exclusive mode

- **Windows:** Wezt opens the device in WASAPI exclusive mode when asked, and falls back to
  shared mode with a notice if it cannot. The two Sound-settings switches, "Allow applications
  to take exclusive control of this device" and "Give exclusive mode applications priority", are
  per-device user settings. Wezt reads and reports them but never changes them; if exclusive mode
  is blocked, the error says which switch to turn on.
- **macOS:** exclusive access is CoreAudio "hog mode". Wezt never takes a device another process
  already owns, and releases it on stop.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++20 compiler (MSVC 2022, Clang 15+, GCC 12+) and CMake 3.20+. VST3, CLAP and the
bridge are optional and need their SDKs (VST3 needs CMake 3.25+); see `docs/PLUGIN_FORMATS.md`.

## Using the engine

```cpp
Graph g;
auto src = g.add(std::make_unique<Sine>(440.0));
g.setOutput(src);
Engine engine(g, /*block*/256, /*channels*/2, /*rate*/48000.0);

auto dev = audio::createPlatformDevice();   // WASAPI or CoreAudio
audio::AudioConfig cfg; std::string err;
dev->open(cfg, &engine, &err);
dev->start(&err);
engine.transport().play();
```

## Layout

```
docs/        architecture, plugin formats, UI, immersive audio, roadmap, clean-room policy
src/engine/  graph, transport, engine driver (no third-party deps)
src/audio/   device interface; WASAPI, CoreAudio, null backends, sample-format conversion
src/io/      WAV/BWF/RF64 reader and writer
src/session/ mixer strips, clips, session save/load, bounce
src/instruments/ sampler
src/immersive/ layouts, VBAP renderer, binaural, loudness, ADM export
src/ui/      commands and key map, the Dear ImGui application
tools/       CPU screenshot tool for the UI
src/plugins/ VST3 + CLAP hosts, plugin library (scan/install), 32-bit bridge, AAX twin finder
tests/       unit tests; Windows/macOS simulators (tests/sim); plugin host tests
```

Plugin control for AI agents (parameters, intent tuning, CLAP `wezt.agent/1`): docs/AGENT_INTERFACE.md

## Build presets and CI

`CMakePresets.json` has `release`, `debug`, `clang`, `asan` (AddressSanitizer + UBSan) and `tsan`:

```
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

`.github/workflows/ci.yml` runs those four presets on Linux, builds and tests on Windows (MSVC) and macOS, builds the full
app on all three, and hosts real VST3/CLAP plug-ins on all three. `.github/workflows/release.yml` packs unsigned
archives (app, `wezt-mcp`, `wezt_voice_takes`, docs) for the three systems: run it by hand or push a tag like `v0.8.0` to
publish a release. Unsigned means Windows SmartScreen and macOS Gatekeeper will warn. MSVC gets `_USE_MATH_DEFINES`,
`NOMINMAX` and `/utf-8`.
