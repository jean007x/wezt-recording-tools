#include "music/midi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

namespace wezt::music {
namespace {
constexpr int kPpq = 480;

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s)); }
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(static_cast<std::uint8_t>(x >> 8)); v.push_back(static_cast<std::uint8_t>(x)); }
void putVar(std::vector<std::uint8_t>& v, std::uint32_t x) {
  std::uint8_t buf[5]; int n = 0;
  buf[n++] = x & 0x7F;
  while ((x >>= 7) > 0) buf[n++] = static_cast<std::uint8_t>((x & 0x7F) | 0x80);
  while (n > 0) v.push_back(buf[--n]);
}
void chunk(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& body) {
  out.insert(out.end(), tag, tag + 4);
  put32(out, static_cast<std::uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}
}  // namespace

std::vector<std::uint8_t> writeMidiFile(const std::vector<MidiClip>& clips, double bpm, int beatsPerBar, const std::vector<double>& offsets) {
  std::vector<std::uint8_t> out;
  std::vector<std::uint8_t> hdr;
  put16(hdr, 1); put16(hdr, static_cast<std::uint16_t>(clips.size() + 1)); put16(hdr, kPpq);
  chunk(out, "MThd", hdr);
  {  // tempo and meter
    std::vector<std::uint8_t> t;
    const std::uint32_t us = static_cast<std::uint32_t>(std::lround(60000000.0 / std::max(1.0, bpm)));
    putVar(t, 0); t.insert(t.end(), {0xFF, 0x51, 0x03, static_cast<std::uint8_t>(us >> 16), static_cast<std::uint8_t>(us >> 8), static_cast<std::uint8_t>(us)});
    putVar(t, 0); t.insert(t.end(), {0xFF, 0x58, 0x04, static_cast<std::uint8_t>(beatsPerBar), 2, 24, 8});
    putVar(t, 0); t.insert(t.end(), {0xFF, 0x2F, 0x00});
    chunk(out, "MTrk", t);
  }
  for (std::size_t ci = 0; ci < clips.size(); ++ci) {
    const auto& c = clips[ci];
    const double off = ci < offsets.size() ? offsets[ci] : 0.0;
    struct Ev { std::int64_t tick; int order; std::uint8_t st, a, b; };
    std::vector<Ev> evs;
    for (auto& n : c.notes) {
      const std::int64_t on = std::llround((off + n.start) * kPpq);
      const std::int64_t offT = std::max<std::int64_t>(on + 1, std::llround((off + n.start + n.length) * kPpq));
      const auto ch = static_cast<std::uint8_t>(c.channel & 15);
      evs.push_back({on, 1, static_cast<std::uint8_t>(0x90 | ch), static_cast<std::uint8_t>(std::clamp(n.pitch, 0, 127)), static_cast<std::uint8_t>(std::clamp(n.velocity, 1, 127))});
      evs.push_back({offT, 0, static_cast<std::uint8_t>(0x80 | ch), static_cast<std::uint8_t>(std::clamp(n.pitch, 0, 127)), 0});
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.tick != y.tick ? x.tick < y.tick : x.order < y.order; });
    std::vector<std::uint8_t> t;
    putVar(t, 0); t.push_back(0xFF); t.push_back(0x03);
    putVar(t, static_cast<std::uint32_t>(c.name.size())); t.insert(t.end(), c.name.begin(), c.name.end());
    std::int64_t last = 0;
    for (auto& e : evs) { putVar(t, static_cast<std::uint32_t>(e.tick - last)); last = e.tick; t.push_back(e.st); t.push_back(e.a); t.push_back(e.b); }
    putVar(t, 0); t.insert(t.end(), {0xFF, 0x2F, 0x00});
    chunk(out, "MTrk", t);
  }
  return out;
}

bool saveMidiFile(const std::string& path, const std::vector<MidiClip>& clips, double bpm, int bpb, const std::vector<double>& offsets) {
  const auto b = writeMidiFile(clips, bpm, bpb, offsets);
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return static_cast<bool>(f);
}

bool readMidiFile(const std::vector<std::uint8_t>& b, MidiFile& out, std::string* err) {
  auto fail = [&](const char* m) { if (err) *err = m; return false; };
  std::size_t p = 0;
  auto r32 = [&](std::uint32_t& x) { if (p + 4 > b.size()) return false; x = (std::uint32_t{b[p]} << 24) | (b[p + 1] << 16) | (b[p + 2] << 8) | b[p + 3]; p += 4; return true; };
  auto r16 = [&](std::uint32_t& x) { if (p + 2 > b.size()) return false; x = static_cast<std::uint32_t>((b[p] << 8) | b[p + 1]); p += 2; return true; };
  if (b.size() < 14 || std::string(b.begin(), b.begin() + 4) != "MThd") return fail("not a MIDI file");
  p = 4;
  std::uint32_t len, fmt, ntr, div;
  if (!r32(len) || len < 6 || !r16(fmt) || !r16(ntr) || !r16(div)) return fail("bad header");
  if (div & 0x8000) return fail("SMPTE time division is not supported");
  if (div == 0) return fail("bad time division");
  p = 8 + len;
  out = MidiFile();
  bool tempoSet = false;
  for (std::uint32_t t = 0; t < ntr && p + 8 <= b.size(); ++t) {
    if (std::string(b.begin() + static_cast<long>(p), b.begin() + static_cast<long>(p) + 4) != "MTrk") return fail("missing track");
    p += 4;
    std::uint32_t tl;
    if (!r32(tl) || p + tl > b.size()) return fail("truncated track");
    const std::size_t end = p + tl;
    MidiClip clip;
    std::int64_t tick = 0;
    std::uint8_t running = 0;
    std::map<std::pair<int, int>, std::pair<std::int64_t, int>> open;  // (channel, pitch) -> start tick, velocity
    auto readVar = [&](std::uint32_t& v) { v = 0; for (int i = 0; i < 4; ++i) { if (p >= end) return false; const std::uint8_t c = b[p++]; v = (v << 7) | (c & 0x7F); if (!(c & 0x80)) return true; } return false; };
    while (p < end) {
      std::uint32_t dt;
      if (!readVar(dt)) return fail("bad delta time");
      tick += dt;
      if (p >= end) break;
      std::uint8_t st = b[p];
      if (st == 0xFF) {
        if (p + 2 > end) return fail("bad meta event");
        const std::uint8_t type = b[p + 1];
        p += 2;
        std::uint32_t ml;
        if (!readVar(ml) || p + ml > end) return fail("bad meta length");
        if (type == 0x51 && ml == 3 && !tempoSet) { const double us = (b[p] << 16) | (b[p + 1] << 8) | b[p + 2]; if (us > 0) out.bpm = 60000000.0 / us; tempoSet = true; }
        if (type == 0x03) clip.name.assign(b.begin() + static_cast<long>(p), b.begin() + static_cast<long>(p + ml));
        p += ml;
        running = 0;
        continue;
      }
      if (st == 0xF0 || st == 0xF7) {
        ++p; std::uint32_t sl;
        if (!readVar(sl) || p + sl > end) return fail("bad sysex");
        p += sl; running = 0;
        continue;
      }
      if (st & 0x80) { running = st; ++p; } else if (!running) return fail("data byte without status");
      st = running;
      const int ch = st & 15, kind = st >> 4;
      const int need = (kind == 0xC || kind == 0xD) ? 1 : 2;
      if (p + static_cast<std::size_t>(need) > end) return fail("truncated event");
      const int d1 = b[p], d2 = need == 2 ? b[p + 1] : 0;
      p += static_cast<std::size_t>(need);
      if (kind == 0x9 && d2 > 0) { open[{ch, d1}] = {tick, d2}; clip.channel = ch; }
      else if (kind == 0x8 || (kind == 0x9 && d2 == 0)) {
        auto it = open.find({ch, d1});
        if (it != open.end()) {
          Note n; n.pitch = d1; n.velocity = it->second.second;
          n.start = static_cast<double>(it->second.first) / div; n.length = std::max(1.0 / div, static_cast<double>(tick - it->second.first) / div);
          clip.notes.push_back(n);
          open.erase(it);
        }
      }
    }
    p = end;
    if (!clip.notes.empty()) {
      std::sort(clip.notes.begin(), clip.notes.end(), [](const Note& a, const Note& c) { return a.start != c.start ? a.start < c.start : a.pitch < c.pitch; });
      out.clips.push_back(std::move(clip));
    }
  }
  return true;
}

bool loadMidiFile(const std::string& path, MidiFile& out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { if (err) *err = "cannot open " + path; return false; }
  std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return readMidiFile(b, out, err);
}

}  // namespace wezt::music
