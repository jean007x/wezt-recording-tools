# Roadmap

## Logic-style features to add (gap list)

Compiled from general knowledge of Logic Pro; web results this session were stale, so
verify each against the current release before committing to scope.

| Feature | Wezt Recording Tools module | Phase |
|---|---|---|
| Live Loops (clip-launch grid) | `session/clips` | 3 |
| Drummer / Session Players (generative backing) | `creative/players` | 4 |
| Flex Time and Flex Pitch (non-destructive warp, pitch edit) | `dsp/warp` | 3 |
| Smart Tempo (tempo detection and following) | `dsp/tempo` | 3 |
| Step Sequencer and pattern regions | `session/pattern` | 3 |
| Stem splitting (ML source separation) | `ml/stems` | 5 |
| Built-in synths and sampler (wavetable, quick sampler) | `instruments/` | 4 |
| Saturation and amp/pedalboard effects | `dsp/fx` | 4 |
| Score editor | `ui/score` | 5 |
| Track stacks / summing stacks | `session/stacks` | 2 |
| Ableton Link sync | `sync/link` | 3 |

## Phases (updated)

1. **Engine**: graph, session model, WAV/BWF, offline render (done in scaffold)
2. **Real-time**: audio I/O (WASAPI/ASIO, CoreAudio), transport, recording, stacks
3. **Editing and interchange**: waveform editing, warp, AAF/OMF/MIDI import-export, clips
4. **Plugins and instruments**: VST3, CLAP, AU hosting, 32-bit bridge, built-ins
5. **ML and notation**: stems, score editor
6. **UI** (Dear ImGui, replacing the earlier Qt plan) in parallel from phase 2
