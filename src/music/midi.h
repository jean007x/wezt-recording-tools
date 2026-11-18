#pragma once
// Notes and clips in musical time (beats), plus Standard MIDI File export and import so that what
// Wezt writes opens in any other DAW.
#include <cstdint>
#include <string>
#include <vector>

namespace wezt::music {

struct Note {
  int pitch = 60;         // MIDI note number
  double start = 0;       // beats from the clip start
  double length = 1;      // beats
  int velocity = 90;      // 1..127
};

struct MidiClip {
  std::string name;
  int channel = 0;        // 0-based; drums use 9
  std::vector<Note> notes;
  double lengthBeats = 0; // 0 = until the last note ends
  double end() const {
    double e = lengthBeats;
    for (auto& n : notes) if (n.start + n.length > e) e = n.start + n.length;
    return e;
  }
};

// Format 1 file: a tempo track and one track per clip. Notes are placed at `offsetBeats[i]`
// (empty = all at 0). 480 ticks per beat.
std::vector<std::uint8_t> writeMidiFile(const std::vector<MidiClip>& clips, double bpm, int beatsPerBar = 4,
                                        const std::vector<double>& offsetBeats = {});
bool saveMidiFile(const std::string& path, const std::vector<MidiClip>& clips, double bpm, int beatsPerBar = 4,
                  const std::vector<double>& offsetBeats = {});

struct MidiFile {
  double bpm = 120;
  std::vector<MidiClip> clips;  // one per track that holds notes
};
bool readMidiFile(const std::vector<std::uint8_t>& bytes, MidiFile& out, std::string* error = nullptr);
bool loadMidiFile(const std::string& path, MidiFile& out, std::string* error = nullptr);

}  // namespace wezt::music
