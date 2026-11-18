#pragma once
// A MIDI event as it travels to an instrument inside one audio block. `offset` is the sample
// position within the block, so notes start exactly where the sequencer put them. Plain data:
// no allocation, safe to copy on the audio thread.
#include <cstdint>

namespace wezt {

struct MidiEvent {
  enum Type : std::uint8_t { NoteOn, NoteOff, Controller, PitchBend, AllNotesOff };
  std::uint32_t offset = 0;  // sample within the block
  Type type = NoteOn;
  std::uint8_t channel = 0;  // 0-based
  std::int16_t a = 0;        // NoteOn/NoteOff: key. Controller: number. PitchBend: 0..16383 (8192 centre)
  std::int16_t b = 0;        // NoteOn/NoteOff: velocity (0..127). Controller: value
};

// Most events a single block can carry to one instrument; extras are dropped and counted.
constexpr std::size_t kMaxMidiPerBlock = 512;

}  // namespace wezt
