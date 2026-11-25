# Coverage of the reference feature document

"Done" means implemented and covered by an automated test. "Engine only" means the logic exists
and is tested but no window or shortcut uses it yet. CI (GitHub Actions) builds and tests it on real Windows and macOS runners: all 27 tests pass on Linux, Windows and macOS, the
full app compiles and its UI test passes on all three, and real VST3/CLAP plug-ins are hosted on all three. Still never run:
the window on a real display, real audio hardware, a real MIDI keyboard, commercial plug-ins, a real Claude Desktop session.

## Engine safety
- **Fault containment (built, tested):** NaN or infinity in any node's output (a misbehaving plug-in) is replaced by 0 before the
  next node sees it; a node that does it in 4 blocks in a row is muted until reset. The app names the muted plug-in in its status
  line (`Session::pluginFaults`, `reenablePlugin`; tested in `test_session`). The Plugin Parameters window shows a "Re-enable this plugin" button for a muted plugin (compiled, not clicked on a real screen). Not built: catching a plug-in that crashes or hangs (that is what the 32-bit bridge's process separation is for;
  the 64-bit hosts run plug-ins in-process).

- **Plug-in latency changes (built, tested):** a VST3 plug-in's restartComponent(kLatencyChanged) and a CLAP plug-in's host latency
  `changed()` are noticed; the app stops the audio briefly and re-computes the delay compensation. Tested with the CLAP test plug-in
  and a probe plug-in; the real VST3 sample plug-ins never change their latency, so that path is exercised only up to registering the handler.
- **Device loss (built, tested against the simulators):** WASAPI (invalidated device, no event for 0.5 s) and CoreAudio (the DeviceIsAlive
  listener) report a stream that died; the app shows a message, stops the transport and tries to reopen the system default device every 2 s.
  Real unplugging on real hardware has not been tried. When the system default device changes while the stream still works, the app checks every 2 s and reopens on the new default once the transport is stopped (compiles in CI, but never tried on real devices; there is no automated test of it).

## Edit window

| Feature | State |
|---|---|
| Edit modes Shuffle / Slip / Grid; Spot | Shuffle (`shuffleClose`), Slip, Grid (`snapToGrid`) done; Spot dialog done (click a clip in Spot mode, or Shift+F3: type an exact start time) |
| Tools: Trim, Selector, Grabber, Smart, Zoomer | in the UI; Scrubber, Pencil, Time/Loop trim variants not built |
| Counter, sub counter, selection start/end/length, rulers, overview, track/group lists, markers ruler, track headers | in the UI |
| Zoom presets 1-5, nudge and grid value boxes | in the UI |
| Tab to Transients | engine only (`tabToNextTransient`, `tabToPreviousTransient`) |
| Strip Silence (threshold, minimum silence, minimum clip, pads, fades) | done, with command and undo |
| Clip gain: gain, gain **envelope** (breakpoints, before inserts and fader), normalise | done; drawn as a dB tag, envelope not editable on screen yet |
| Separate, consolidate (`renderClip`), clip mute / lock / name | done (engine); lock and mute saved in the session |
| Undo / redo, merged drags, history list | done (`edit/undo.h`) and wired to the app |
| Memory Locations (numbered, ranges, comments, zoom preset, track visibility, pre/post-roll), saved | done; window in the UI |
| Clips list window, playlists (alternate takes) and comping | done (see "v0.7 additions") |
| Special Paste (Ctrl+Shift+V: copies, gap, merge) | done |
| Capture Clip, time operations (insert/cut time, tempo operations) | not built |
| Elastic Audio (polyphonic, rhythmic, monophonic, varispeed), Beat Detective | done (see "v0.7 additions") |
| Melodyne-style note-level pitch editing, ARA | not built |
| MIDI Editor, Score Editor | built as the piano roll and score windows (see the rows below) |
| Real-Time Properties, MPE | not built |

## Mix window

| Feature | State |
|---|---|
| Channel strips: inserts, sends (pre/post, mute), I/O, pan, fader, meters, solo/mute/arm, groups | done |
| Automation modes Off / Read / Touch / Latch / Touch-Latch / Write | done (`AutomationWriter`): written off the audio thread, merged on stop; Touch rejoins old automation |
| Thin Automation | done (`AutomationLane::thin`) |
| VCA masters | done (summed dB, saved/loaded); no strip in the Mix window yet |
| Aux inputs | covered by buses; Routing Folders, Master Fader, Video tracks as separate track types not built |
| Commit / Freeze | offline bounce exists; per-track commit not built |
| Track Pin, Playlist colour coding | Track Pin not built |
| LUFS, peak meters | LUFS in analysis and immersive meter; VU / K-System not built |

## Other windows and systems

| Feature | State |
|---|---|
| Transport: play, stop, record, loop, locate | done. Also loop record (every pass its own take), play at half speed (Shift+Space), and a recording take now ends when the playhead jumps. Pre-roll (toolbar "Pre": off, 2, 4, 8 s) rolls the song in from before the record point and switches recording on exactly there; it applies to recording from the playhead, not to loop or punch ranges. Record Selection (Ctrl+Shift+R) rolls in, records exactly the selected range, punches out and stops after a post-roll as long as the pre-roll. QuickPunch (Num6) switches recording on or off while the song keeps playing. Recorded takes become clips when the transport stops (one undo step). Metronome (a click on every beat, louder on the bar's first beat, mixed after the graph so it is heard but never recorded or bounced) and count-off before recording (off / 1 / 2 / 4 bars; the song waits while only the clicks play) are built and unit-tested in `test_engine` (frame-exact beats, the song starts at the end of the count, cancelling). The beats per bar can be set (1-12, quarter-note beats: the "n/4" box next to the tempo; it drives the ruler, the counter, the accent and the count-off length); other note values (6/8) are not modelled, and the click sound is not configurable. The tempo and the beats per bar are saved with the session (header line; older sessions load as 120 bpm, 4 beats). Pre/post-roll and QuickPunch not built |
| Setup: Playback Engine (device, buffer), exclusive mode | done (simulated on Linux) |
| VST3, CLAP hosting, 32-bit bridge, plug-in scanner / installer | hosting library done and tested with real SDK plug-ins; **View > Plug-ins (VST3, CLAP)...** in the app scans this computer, lists what this build can host and adds one to the selected track (opened sessions rebuild their VST3/CLAP inserts through the same loader). The window itself is compiled on all three platforms but has never been used with a real plug-in on a real screen; the scanner reads headers only, but adding a plug-in runs its code in-process, so without safe mode a crashing plug-in still takes the app down (save first). **Safe mode**: a checkbox in the browser runs a VST3 *effect* in the separate `wezt-bridge` process (shipped next to the app), so its crash or hang silences that insert instead of taking Wezt down; the Plugin Parameters window shows the helper's state; the flag is saved with the session. Instruments work in safe mode too (notes cross the bridge with their sample offsets; tested with a real SDK instrument), and CLAP plug-ins can run in safe mode too (tested bit-identical to in-process with a real CLAP plug-in), but a 32-bit helper is not shipped, and the in-app path is compiled but not exercised with a real plug-in (the bridge library itself is tested with real SDK plug-ins and a killed helper). **Plug-in windows (VST3 and CLAP, Windows and macOS only):** the Plugin Parameters window has an "Open plug-in window" button that puts the plug-in's own editor in a plain window (GLFW, native HWND / NSView), follows the plug-in's resize requests, forwards the plug-in's own control moves to the audio side, and closes the window when the insert is removed. The app-side logic is unit-tested with a fake window host; the VST3/CLAP view code and the native window glue are compiled in CI but have never run with a real plug-in editor (and not at all on Linux, where plug-in windows are not supported yet; CLAP windows use the clap.gui extension (embedded, resize requests followed) and are built the same way; safe-mode plug-ins have no window); AAX, RTAS, TDM not hostable (licensing, DSP hardware) |
| Immersive: layouts to 9.1.6, VBAP, binaural monitor, ADM BWF export | done; not routed into live audio |
| AudioSuite-style offline processing | the mastering chain processes files offline; no generic window |
| Stem Separator | built-in DSP splitter (rough, about -40 dB reconstruction error); an optional Demucs hook exists but is untested; no trained model of our own |
| Extensions API | not built |
| Speech-to-Text | not built |
| Project Browser, Workspace, Video | not built |
| AAF / OMF | not built |

## Where we went further than the document

- A local assistant that measures, learns your style and masters (`docs/AI.md`), with no cloud
  dependency and a loopback-only guard on the optional language model.
- Clip gain **envelopes** that are part of the clip and survive separate and strip silence.
- Automation is recorded without touching data the audio thread is reading.
- Open interchange (ADM BWF with channel and object metadata) rather than a proprietary renderer.


## v0.7 additions
| Feature | Status |
|---|---|
| Chord/melody generation by style and mood | Built: rule-based music theory (not trained on "thousands of compositions"); seeded, can learn degree transitions from your progressions |
| Audio-to-MIDI (hum, beatbox) | Built: YIN pitch tracking, onsets, spectral drum classes; writes .mid and renders with built-in synth |
| Session players (bass, keys, pad, drums) | Built: follow chords and tempo; rendered to audio with built-in sounds (they can also be played live through an instrument track: see "Piano roll window") |
| Stem splitting | Updated: built-in DSP splitter (rough, about -40 dB reconstruction error); optional Demucs hook on macOS/Linux (untested) |
| Text commands | Built in the Assistant panel: group tracks, sidechain, "find a dreamy pad" (generates one; no sound library search), clashes, song, chords, master, stems |
| In-app tutor | Built: 17 topics, local |
| Smart EQ / clash detection | Built: octave-band clash finder, proposed cuts, offline sidechain |
| One-click mastering | Already existed (v0.6), now reachable from text and the connector |
| Claude connector | Built: wezt-mcp (see CLAUDE_CONNECTOR.md); claude.ai in the browser cannot reach it |
| Voice model (clone) | Built from scratch: trained on recordings on this computer (cepstral-envelope codebook + harmonic/noise vocoder), saved to a file. Training is refused unless the caller states the voice is their own or the speaker agreed; the statement is stored in the model. Classical voice conversion: it captures timbre but sounds more synthetic than RVC-class neural systems, and needs a minute or two of clean solo voice. Not run on a real singer here (tested on synthetic voices) |
| Choir from your voice | Built: one take becomes stacked, detuned, time-spread copies moved to scale notes |
| Singing synthesis from lyrics | Built from scratch (no trained model), see the "Singing from lyrics" row. It is a formant synthesizer, so it can be made lifelike but not indistinguishable from a person |
| Elastic Audio | Built: polyphonic / rhythmic / monophonic / varispeed stretch and pitch shift (clips and tracks) |
| Beat Detective | Built: cuts at hits, moves slices to a grid with strength and swing, short fades; operates on the playlist |
| MIDI editing | Built as operations on .mid files (quantize, transpose, humanize, legato, velocity, crop, snap to scale, text score view) from Claude and code; the piano roll and score windows edit the same parts |
| Piano roll window | Built: draw/select, move, resize, delete, copy/paste/duplicate, transpose, quantize, legato, velocity lane, grid and zoom, undo/redo. MIDI parts are saved next to the session (`<session>.wezt.midi`); "Render to track" plays a part with the built-in instruments. Clicking a key on the keyboard column plays that note through the part's instrument (needs a running audio device). "Play live" binds a part to an instrument track so the transport plays it in real time, sample-accurately, through the built-in Wezt Synth or Sampler or a VST3/CLAP instrument. Tested with real VST3 instruments from the SDK's mda bundle (piano, DX10, JX10, EPiano) and a CLAP test instrument. Hardware MIDI keyboard: `src/midi/` (byte parser with running status, SPSC queue, Linux raw-MIDI device thread) is tested, including a fake keyboard through a FIFO; the WinMM and CoreMIDI backends and the App hookup (first keyboard found plays the selected part's instrument) are written but not compiled or run here. No device chooser UI, no sustain pedal or pitch bend routing yet |
| Score (notation) window | Built: clef (auto/treble/bass), key signature, bars, rests, ties, dotted values, accidentals, chords, stems and flags, ledger lines. One voice per staff, sixteenth-note rounding for printing, no beams, no editing on the staff (edit in the piano roll) |
| Clips list window | Built: every clip of every track, timeline order; select/locate, rename, gain, mute, delete (locked clips protected) |
| Playlists (alternate takes) | Built: per-track named playlists, new (copy/empty), switch, rename, delete, comp a selected range from another playlist; saved with the session |
| Singing from lyrics | Built from scratch, no trained model: rule-based syllables, formant vowels, consonant noise/nasal sounds, vibrato, bass/tenor/alto/soprano, choir of up to 8. Humanized: per-cycle jitter and shimmer, pulsed breath noise, onset scoop and overshoot, drift and end-of-note sag, vibrato that starts late and wanders (about 5-6 Hz, 50-65 cents peak to peak by default), loudness that follows velocity and brightness that follows loudness, a singer's formant near 3 kHz, human timing, a different "person" per choir voice, and a small room (controls: humanize, room). Voice characters (`character`): warm_alto, bright_pop, breathy_pop, gritty_rock, operatic_soprano, deep_bass, tenor_crooner, boyish, child, airy_whisper, choirboy, folk_storyteller: each sets the voice type, vocal-tract size, brightness, roughness, vocal fry, effort, breathiness and vibrato. Your own voice (`voice_model`): the sung line is passed through a voice model trained on your recordings (consent-gated), which gives it your timbre by classical conversion. Checked against published ranges for sung vowels with `tests/test_sing_human.cpp` (tenor: vibrato 5.9 Hz, jitter 0.7 %, shimmer 1.4 %, HNR 22 dB). **Not validated against recordings of real singers**: no singing audio could be fetched here, so the targets are published figures, not a comparison with actual voices. It still sounds synthetic next to a real singer; for a human timbre train a voice model on your own voice and convert the sung track. English spelling rules are approximate (use [ah] [oo] ... to force a vowel). Not a clone of any voice |
| VST3 | Already hosted |
| Use your own voice on any song (fit_vocal) | Built: your own recording is retuned and retimed to a melody (generated or a .mid) with a diatonic harmony track. DSP, not a voice model; the timbre stays yours and big pitch moves sound less natural. A true neural clone (e.g. RVC trained on your recordings) is not included |
| Universal plugin control for AI agents | Built: parameters with ranges/units/meanings on every plugin, intent-based tuning, optional CLAP extension `wezt.agent/1` for plugin-provided AI tools, 12 MCP tools, assistant commands ("make the bass darker"), undo. Built-in EQ, compressor, limiter and stereo width are now plugins with this surface. See AGENT_INTERFACE.md for the limits (guessed semantics for third-party plugins, VST3 has no AI-tool extension, only a test plugin implements the CLAP extension) |
| Sing with your own recorded vowels (voice bank) | Built: `build_voice_bank` from held-vowel recordings of your own voice, then `sing_lyrics voice_bank=...`: vowels are your real audio re-pitched with PSOLA, driven by the humanized pitch, vibrato and timing. Consonants can be recorded too ("sah", "tah": the part before the vowel is used at its real loudness), otherwise synthesized; keep notes within ~5 semitones of each recording. Mechanics tested on held vowels from the built-in singer, not on a real person. See VOICE_BANK.md |
| Plugin parameter automation | Built: lanes per plugin parameter, played on the audio thread in step with the transport (built-ins including the Sampler, VST3, CLAP, bridged), saved with the session, recorded by moving a control while playing (Plugin Parameters window) or set by an AI agent (`automate_plugin_param`). Effect plug-ins are processed in pieces: each block is cut at every breakpoint (a jump lands on its exact frame) and at most every 32 frames while a lane moves, each piece getting the value of its own middle (`AutomatedEffect`, tested with a probe plug-in that records every piece). Instruments (plug-ins that take MIDI) are run in pieces too: the track's MIDI for the block is cut up with it, so each note still starts on its own frame (tested with a probe instrument: pieces, one delivery per note, frame-exact offsets). This covers the built-ins; VST3/CLAP instruments take the same path but are not tested here with a real plug-in. Not per-sample ramps: 32-frame steps (0.7 ms at 48 kHz). Lanes can be drawn in the Plugin Parameters window (click to add a point, drag to move it, right-click to delete; all undoable). Recording in the window, its tidy-up and clearing a lane are on the undo stack (one pass is one step); agent edits are too. See AGENT_INTERFACE.md |
| Start-up intro and Settings | Built: cinematic GPU intro (logo as a ray-marched 3D metal slab in a studio, reflections, floor, light beams, bloom; small -> big -> small, then the studio opens; falls back to a plain 2D intro on old GPUs), synthesized cinematic sound ending in a metal clash, skippable; File > Settings turns it off or uses your own WAV/MP3 (in-window file browser). Tested headless (settings file, WAV/MP3 loading, sound shape, window logic, rendered frames). GL frames checked only through Mesa software rendering; not tested on a real GPU, display or speakers. See UI.md |


## Not built (the honest list)

- **Hardware MIDI keyboard.** Never tried with a real keyboard. The WinMM and CoreMIDI backends compile on real Windows and macOS runners but have never received a message from a device. Tested: parser, queue, the Linux raw-MIDI backend via a fake device, and the sustain pedal, pitch bend (±2 semitones) and mod wheel (vibrato) in the built-in synth. The sampler and third-party instruments get the same events; I did not test those. The device chooser is in the Piano Roll toolbar; compiled, never seen on screen.
- **Automation is applied in 32-frame pieces, not per sample** (effects and instruments; see the automation row).
- **Automation lanes on the main timeline: volume, pan and mute.** The "auto" button on a track header cycles off, volume, pan, mute, and draws that lane over the clips (click to add a point, drag to move, right-click to delete; snaps to the grid; undoable; only while the song is stopped, because the audio thread reads the lane while playing). Compiled and unit-tested (`testStripLaneEdit`) but not clicked on a real screen.
- **Real-world checks still missing:** the window on a real display, real audio hardware, a real MIDI keyboard, commercial (third-party) plug-ins, a real Claude Desktop session, and a real 32-bit helper.
- **Packaging:** unsigned archives are built by `release.yml`; there are no installers, and no code signing or macOS notarization (those need certificates).
- **From the reference document:** Real-Time Properties, MPE, Routing Folders, per-track Commit/Freeze,
  Track Pin, Extensions API, Speech-to-Text, Project Browser, Workspace, Video, AAF/OMF,
  AU and VST2 loading, AAX/RTAS/TDM (not hostable).
