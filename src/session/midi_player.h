#pragma once
// The sequencer side of live MIDI. A MidiSequence is a MIDI clip turned into a time-ordered list of
// events at absolute timeline frames; a track plays it to its instrument inserts in step with the
// transport (see TrackSource::setMidiSequence). Build it once when the part or the tempo changes;
// the audio thread only reads it.
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/midi_event.h"
#include "music/midi.h"

namespace wezt::session {

struct SeqEvent {
  std::int64_t frame = 0;   // timeline frame
  MidiEvent ev;             // ev.offset is filled in per block
};

struct MidiSequence {
  std::vector<SeqEvent> events;     // sorted by frame (note-offs before note-ons at the same frame)
  std::int64_t lengthFrames = 0;
};

// Notes are in beats from the clip start; the clip starts at `startBeat` on the timeline, at constant `bpm`.
std::shared_ptr<const MidiSequence> buildMidiSequence(const music::MidiClip& clip, double bpm, double sampleRate, double startBeat = 0.0);

// Events with frame in [from, to), written into `out` with offsets relative to `from`. Returns how many
// were written (at most `cap`); events past `cap` are dropped and counted in `dropped` when given.
std::size_t collectMidi(const MidiSequence& seq, std::int64_t from, std::int64_t to, MidiEvent* out, std::size_t cap, unsigned* dropped = nullptr);

}  // namespace wezt::session
