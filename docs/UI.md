# User interface

Wezt's UI is written in C++ with [Dear ImGui](https://github.com/ocornut/imgui) (MIT). It replaces the
earlier plan of using Qt.

Why: ImGui is immediate-mode. It keeps no widget tree in memory, draws straight from the session data,
and uses no UI framework runtime. The desktop app sleeps until an input event arrives unless something
is moving (playback, meters, a drag), so an idle session costs almost no CPU. The UI is in the same
language as the engine, so meters, waveforms and the engine share data with no bridge.

This is **not** a copy of the Pro Tools interface. The workflow is the same kind (Edit window, Mix
window, transport, tools, edit modes) and the default keys match the published Pro Tools shortcut
table, but the layout, drawing, colours and names are our own (see `CLEAN_ROOM_POLICY.md`).

## Windows

| Window | What it does |
|---|---|
| Transport bar | counter (time or bars/beats), play, record, loop, rewind/forward, edit mode, tool, **+ / - / Dup** track buttons, tempo |
| Edit | track list and timeline with waveforms, selection, playhead, fades, grid; tools: Zoom, Trim, Select, Grab, Smart |
| Mix | one strip per track: inserts, sends, pan, fader, meter, output; aux buses and the master |
| Sampler | CLASSIC / ONE SHOT / SLICE tabs, stereo waveform with draggable flags, key tags, slice strip, LFO 1/2 + mod matrix, Pitch, Filter and Amp panels, keyboard (see below) |
| Immersive panner | speaker layout, objects in top and side view, estimated speaker levels, ADM BWF export, loudness measurement |

## Keyboard

Everything in `src/ui/commands.cpp`. Open View > Keyboard shortcuts for the live table. Commands that are
not implemented yet appear dimmed and say so in the status bar instead of doing nothing. Not
implemented: undo/redo, cut/copy/paste, clip mute, strip silence, consolidate, scrub, pencil, metronome,
count-off, QuickPunch, grouping, commit/bounce track.

## Building

```
cmake -S . -B build -DWEZT_BUILD_UI=ON -DWEZT_IMGUI_DIR=/path/to/imgui -DWEZT_GLFW_DIR=/path/to/glfw
cmake --build build
./build/wezt-studio --demo
```

Without GLFW, `wezt_ui_snapshot` still builds: it draws one frame on the CPU into an image, which is
how the layout is checked on machines with no display. `wezt_ui_test` runs the UI logic headless.

## Status

Checked here: all of the UI logic (commands, keys, clip editing, track add/remove/duplicate, every window
drawing for several frames) under ASan and UBSan, and the layout through CPU-rendered screenshots.
**Not checked here:** the GLFW + OpenGL window (`wezt-studio`) has only been syntax-checked (this machine
has no GLFW or OpenGL headers), and the UI has never run on Windows or macOS. Expect small fixes.

Known limits: structural edits (add/remove/duplicate track, inserts, sends) stop the audio device for
a moment, because the graph may only change while it is stopped. File paths are typed in a text box
(no native file dialog yet). The sampler is played from its on-screen keyboard; MIDI input and
MIDI clips are not built yet.

## Look

Graphite panels, one azure accent, a track colour per track (header band, clip body, mixer strip label),
glossy transport buttons with drawn icons, an LCD-style counter, segmented LED meters and a fader with a
dB scale. Everything is drawn with ImGui primitives in `src/ui/style.h`; the colours, shapes and layout
are our own and not copied from any other product, so the look is in the same *style* as professional
DAWs but is not a reproduction of any one of them.

Font: Roboto Medium (Apache License 2.0, shipped with Dear ImGui), embedded in `src/ui/font_roboto.inc`.

## Sampler window

Layout: mode tabs, file name, snap and zoom, a menu (load sample, patches, multi-sample zones); a stereo
waveform with start / end / loop flags (Classic, One Shot) or slice flags with numbers (Slice); key tags
under each slice; a control strip for the mode; then four panels: LFO (LFO 1, LFO 2, mod matrix), Pitch,
Filter and Amp, with knobs and AHDSR envelope graphs; and an on-screen keyboard.

- **Classic** pitches one sample across the keyboard (optional loop with crossfade, reverse, root key).
- **One Shot** is the same, but the sample plays through and ignores key release.
- **Slice** cuts the sample at detected transients (or into equal parts) and puts the slices on keys from a start
  key, chromatic or white keys only. Drag a flag to move a cut; Snap pulls it to a transient or the beat grid.
  Gate plays a slice while the key is held, Play to End lets it finish.
- **LFOs:** shape, rate or tempo sync, fade-in, phase, bipolar/unipolar, poly/mono, key trigger; each can
  drive pitch, filter cutoff and level, scaled by mod wheel or velocity.
- **Pitch:** coarse, fine, glide, pitch envelope, key tracking switch, bend range. **Filter:** LP/HP/BP 12 dB,
  cutoff, resonance, drive, envelope depth, keyscale, envelope and velocity. **Amp:** pan, polyphony,
  mono/legato, volume, envelope, velocity.

Not built (shown dimmed with a tooltip): **Recorder** (record into the sampler), **Follow Tempo** and **Speed**
(they need time-stretching). The filter types are 12 dB LP/HP/BP only; there are no other filter models.

## Main window layout

Top to bottom: title strip ("Edit: <session>"); toolbar; overview strip; the work area; a bottom tab bar.

- **Toolbar, row 1:** edit modes in a dark green box (Shuffle, Spot, Slip, Grid; the active one is lit), five zoom presets
  (click recalls, Shift+click stores), zoom out/in, waveform height, track height, tools (smart, zoom, trim, selector, grabber;
  scrub and pencil are shown dimmed). **Row 2:** transport, scroll-with-playhead, **+ / - / Dup track buttons**, add marker,
  Edit / Mix / Float mix, tempo. **Right:** main counter in green, sub counter, start / end / length, MIDI in/out lights (MIDI is
  not built yet), grid value and nudge value with nudge buttons.
- **Overview strip:** the whole session as coloured lines per track; the lighter box is what the Edit window shows; drag in it to scroll.
- **Left list:** TRACKS (click to select, dot to hide or show) and GROUPS. A group links mute, solo, record-arm and fader moves
  across its members; `<ALL>` is a switch for all tracks. Groups, markers and hidden tracks are kept while the app runs; they are not
  in the session file yet.
- **Rulers:** minutes:seconds, bars|beats, and markers (double-click the marker ruler or use the flag button to add one).
- **Track headers:** colour bar, name, record-arm, input monitor (dimmed, not built), solo, mute, waveform toggle, automation
  read/off, input and output boxes, vol and pan fields. Clips show a name bar, a clip-gain tag and fades.
- **Mix:** a full window, or **Float mix** to put it over the Edit window; strips have a colour band, inserts, sends, I/O, automation
  mode, pan knob, fader with LED meters, and the name.
- **Bottom tabs:** MIDI Editor and Clip Effects (dimmed, not built), **Sampler** and **Immersive** (ours).

## Drawing plug-in automation

In Plugin Parameters, click a parameter's dot to show its graph. Click an empty spot to add a point, drag a point to move
it (it stays between its neighbours), right-click a point to delete it. Dragging is one undo step. Plug-ins without
automation support show a note instead.

## MIDI keyboard

The Piano Roll toolbar has a MIDI keyboard chooser. The first keyboard found is opened by itself; choose "none" to
release it. Notes play the selected part's instrument; the sustain pedal, pitch bend and mod wheel go to it too.

## Undo for plug-in automation

Recording automation in the Plugin Parameters window, thinning what was recorded when the song stops, and clearing a lane
are all undo steps. One recording pass is one step (they share a merge key), so a single Undo removes the whole pass.
