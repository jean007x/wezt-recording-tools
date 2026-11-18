#include "music/midi_edit.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace wezt::music {
namespace {
int clampVel(double v) { return std::clamp(static_cast<int>(std::lround(v)), 1, 127); }
void sortNotes(MidiClip& c) { std::stable_sort(c.notes.begin(), c.notes.end(), [](const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); }); }
}  // namespace

std::string pitchName(int n) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(names[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
}

MidiClip quantizeMidi(const MidiClip& c, double grid, double strength, double swing, bool lengths) {
  MidiClip r = c;
  if (grid <= 0) return r;
  strength = std::clamp(strength, 0.0, 1.0);
  for (Note& n : r.notes) {
    const double line = std::round(n.start / grid);
    double target = line * grid;
    if (swing > 0 && (static_cast<long long>(line) & 1)) target += swing * 0.5 * grid;
    n.start += (target - n.start) * strength;
    if (lengths) n.length = std::max(grid, std::round(n.length / grid) * grid);
  }
  sortNotes(r);
  return r;
}

MidiClip snapToScale(const MidiClip& c, const Key& key) {
  MidiClip r = c;
  const auto iv = scaleIntervals(key.mode);
  for (Note& n : r.notes) {
    int best = n.pitch, bestD = 99;
    for (int d = -2; d <= 2; ++d) {
      const int cand = n.pitch + d;
      const int rel = ((cand - key.root) % 12 + 12) % 12;
      if (std::find(iv.begin(), iv.end(), rel) != iv.end() && std::abs(d) < bestD) { bestD = std::abs(d); best = cand; }
    }
    n.pitch = best;
  }
  return r;
}

MidiClip transposeMidi(const MidiClip& c, int semitones, bool toKey, const Key& key) {
  MidiClip r = c;
  for (Note& n : r.notes) n.pitch = std::clamp(n.pitch + semitones, 0, 127);
  return toKey ? snapToScale(r, key) : r;
}

MidiClip humanizeMidi(const MidiClip& c, double timing, int velRange, std::uint64_t seed) {
  MidiClip r = c;
  Rng rng(seed ? seed : 1);
  auto u = [&] { return rng.uniform() * 2.0 - 1.0; };
  for (Note& n : r.notes) {
    n.start = std::max(0.0, n.start + u() * timing);
    n.velocity = clampVel(n.velocity + u() * velRange);
  }
  sortNotes(r);
  return r;
}

MidiClip legatoMidi(const MidiClip& c, double gap) {
  MidiClip r = c;
  sortNotes(r);
  for (std::size_t i = 0; i < r.notes.size(); ++i) {
    double next = -1;
    for (std::size_t j = i + 1; j < r.notes.size(); ++j) if (r.notes[j].start > r.notes[i].start + 1e-9) { next = r.notes[j].start; break; }
    if (next > 0) r.notes[i].length = std::max(0.0625, next - r.notes[i].start - gap);
  }
  return r;
}

MidiClip scaleVelocity(const MidiClip& c, double f, int off) {
  MidiClip r = c;
  for (Note& n : r.notes) n.velocity = clampVel(n.velocity * f + off);
  return r;
}

MidiClip cropMidi(const MidiClip& c, double a, double b) {
  MidiClip r; r.name = c.name; r.channel = c.channel;
  for (const Note& n : c.notes) if (n.start >= a && n.start < b) { Note m = n; m.start -= a; m.length = std::min(m.length, b - n.start); r.notes.push_back(m); }
  r.lengthBeats = std::max(0.0, b - a);
  return r;
}

MidiClip removeShortNotes(const MidiClip& c, double minBeats) {
  MidiClip r = c;
  r.notes.erase(std::remove_if(r.notes.begin(), r.notes.end(), [&](const Note& n) { return n.length < minBeats; }), r.notes.end());
  return r;
}

std::string describeMidi(const MidiClip& c, int bpb) {
  MidiClip s = c; sortNotes(s);
  std::map<int, std::vector<const Note*>> bars;
  for (const Note& n : s.notes) bars[static_cast<int>(std::floor(n.start / bpb)) + 1].push_back(&n);
  std::ostringstream o;
  o << s.notes.size() << " notes, " << std::lround(std::ceil(s.end() / bpb)) << " bars\n";
  for (auto& [bar, ns] : bars) {
    o << "Bar " << bar << ":";
    for (const Note* n : ns) {
      const double beat = n->start - (bar - 1) * bpb + 1;
      o << " " << pitchName(n->pitch) << "@" << std::round(beat * 100) / 100 << "(" << std::round(n->length * 100) / 100 << ")";
    }
    o << "\n";
  }
  return o.str();
}

}  // namespace wezt::music
