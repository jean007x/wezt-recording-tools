#include "session/midi_player.h"

#include <algorithm>
#include <cmath>

namespace wezt::session {

std::shared_ptr<const MidiSequence> buildMidiSequence(const music::MidiClip& clip, double bpm, double sr, double startBeat) {
  auto seq = std::make_shared<MidiSequence>();
  if (!(bpm > 0) || !(sr > 0)) return seq;
  const double framesPerBeat = sr * 60.0 / bpm;
  auto at = [&](double beat) { return static_cast<std::int64_t>(std::llround((startBeat + beat) * framesPerBeat)); };
  for (const auto& n : clip.notes) {
    if (n.length <= 0 || n.pitch < 0 || n.pitch > 127 || !std::isfinite(n.start) || !std::isfinite(n.length)) continue;
    SeqEvent on, off;
    on.frame = at(n.start);
    off.frame = std::max(on.frame + 1, at(n.start + n.length));
    on.ev.type = MidiEvent::NoteOn;  on.ev.channel = static_cast<std::uint8_t>(std::clamp(clip.channel, 0, 15)); on.ev.a = static_cast<std::int16_t>(n.pitch); on.ev.b = static_cast<std::int16_t>(std::clamp(n.velocity, 1, 127));
    off.ev = on.ev; off.ev.type = MidiEvent::NoteOff; off.ev.b = 0;
    seq->events.push_back(on);
    seq->events.push_back(off);
    seq->lengthFrames = std::max(seq->lengthFrames, off.frame);
  }
  // a note that ends exactly where the next one (same key) starts must release first
  std::stable_sort(seq->events.begin(), seq->events.end(), [](const SeqEvent& a, const SeqEvent& b) {
    if (a.frame != b.frame) return a.frame < b.frame;
    return a.ev.type == MidiEvent::NoteOff && b.ev.type != MidiEvent::NoteOff;
  });
  return seq;
}

std::size_t collectMidi(const MidiSequence& seq, std::int64_t from, std::int64_t to, MidiEvent* out, std::size_t cap, unsigned* dropped) {
  const auto& ev = seq.events;
  auto it = std::lower_bound(ev.begin(), ev.end(), from, [](const SeqEvent& e, std::int64_t f) { return e.frame < f; });
  std::size_t n = 0;
  for (; it != ev.end() && it->frame < to; ++it) {
    if (n >= cap) { if (dropped) ++*dropped; continue; }
    out[n] = it->ev;
    out[n].offset = static_cast<std::uint32_t>(it->frame - from);
    ++n;
  }
  return n;
}

}  // namespace wezt::session
