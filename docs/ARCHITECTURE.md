# Architecture

## Layers

1. **engine**: lock-free real-time core. `AudioBuffer`, `Processor`, `Graph`
   (topological order, no allocation in `process`).
2. **session**: tracks, clips, regions, tempo map, versioned serialization.
3. **io**: WAV/BWF now; AAF, OMF, MIDI, MusicXML later.
4. **plugins**: format-agnostic host interfaces, bridge protocol.
5. **ui**: Qt 6, separate target, talks to the engine through a command queue.

## Real-time rules

No heap allocation, locks, or I/O on the audio thread. UI to engine communication goes
through SPSC queues. Graph edits build a new plan on the UI thread and swap atomically.

## Interchange (cross-version compatibility)

- Native session: versioned JSON/CBOR container, with a migration function per version
  step so old sessions always open in newer builds, and export-down for older builds.
- Audio: WAV/BWF (with timestamps), AIFF, FLAC.
- Timelines: AAF and OMF (via an open implementation), MIDI files, MusicXML.
- Stems/mixdown export for hand-off to other DAWs.
