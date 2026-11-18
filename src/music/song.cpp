#include "music/song.h"
#include <map>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

#include "io/wav.h"
#include "session/session.h"

namespace wezt::music {
namespace {

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
bool wants(const SongSpec& s, const char* part) { return s.parts.empty() || std::find(s.parts.begin(), s.parts.end(), part) != s.parts.end(); }

double defaultEnergy(const std::string& kind) {
  if (kind == "intro" || kind == "outro") return 0.3;
  if (kind == "chorus") return 0.85;
  if (kind == "bridge") return 0.45;
  return 0.55;
}

}  // namespace

std::string Song::chartText() const {
  std::ostringstream o;
  o << spec.title << " - " << keyName(key) << ", " << static_cast<int>(std::lround(bpm)) << " bpm, " << spec.style << " / " << spec.mood << "\n";
  std::size_t ci = 0;
  for (auto& s : layout) {
    o << s.name << " (" << s.bars << " bars): ";
    double t = 0;
    bool first = true;
    while (ci < chords.size() && t < s.bars * 4.0 - 1e-9) { o << (first ? "" : " | ") << chords[ci].name(); first = false; t += chords[ci].beats; ++ci; }
    o << "\n";
  }
  return o.str();
}

Song composeSong(SongSpec spec) {
  Song song;
  const StyleInfo si = styleInfo(spec.style);
  spec.style = si.name;
  if (!spec.keyGiven) spec.key.mode = modeForMood(spec.mood, si.defaultMode);
  song.key = spec.key;
  song.bpm = spec.bpm > 0 ? std::clamp(spec.bpm, 40.0, 240.0) : si.defaultBpm;
  if (spec.sections.empty())
    spec.sections = {{"intro", "intro", 4, -1}, {"verse 1", "verse", 8, -1}, {"chorus 1", "chorus", 8, -1}, {"verse 2", "verse", 8, -1}, {"chorus 2", "chorus", 8, -1}, {"outro", "outro", 4, -1}};
  song.spec = spec;

  struct KindMaterial { std::vector<Chord> chords; MidiClip melody; };
  std::map<std::string, KindMaterial> material;
  auto materialFor = [&](const std::string& kind, int bars) -> KindMaterial& {
    const std::string key = kind + "#" + std::to_string(bars);
    auto it = material.find(key);
    if (it != material.end()) return it->second;
    KindMaterial m;
    ProgressionParams pp;
    pp.key = song.key; pp.style = spec.style; pp.mood = spec.mood; pp.bars = bars;
    const std::string base = (kind == "intro" || kind == "outro") ? "verse" : kind;
    pp.seed = spec.seed * 31 + (base == "verse" ? 1 : base == "chorus" ? 2 : 3);
    m.chords = generateProgression(pp);
    MelodyParams mp;
    mp.key = song.key; mp.style = spec.style; mp.mood = spec.mood;
    mp.seed = pp.seed;
    mp.density = base == "chorus" ? 0.65 : 0.4;
    if (base == "chorus") { mp.lowPitch = 66; mp.highPitch = 86; }
    m.melody = generateMelody(m.chords, mp);
    return material.emplace(key, std::move(m)).first->second;
  };

  auto track = [&](const char* name, const char* preset, float gain, float pan) -> SongTrack& {
    for (auto& t : song.tracks) if (t.name == name) return t;
    SongTrack t; t.name = name; t.preset = preset; t.gainDb = gain; t.pan = pan;
    t.midi.name = name;
    if (std::string(preset) == "drums") t.midi.channel = 9;
    song.tracks.push_back(std::move(t));
    return song.tracks.back();
  };
  // fixed order in the session
  if (wants(spec, "drums")) track("Drums", "drums", -3, 0);
  if (wants(spec, "bass")) track("Bass", "bass", -3, 0);
  if (wants(spec, "keys")) track("Keys", "keys", -5, -0.2f);
  if (wants(spec, "pad")) track("Pad", "pad", -9, 0.2f);
  if (wants(spec, "melody")) track("Melody", "lead", -5, 0.1f);

  double at = 0;
  for (auto& sec : spec.sections) {
    const std::string kind = lower(sec.kind);
    const double energy = sec.energy >= 0 ? sec.energy : defaultEnergy(kind);
    auto& mat = materialFor(kind, sec.bars);
    PartParams pp;
    pp.style = spec.style; pp.key = song.key; pp.energy = energy;
    pp.seed = spec.seed * 101 + static_cast<std::uint64_t>(kind.size());
    auto put = [&](const char* name, const MidiClip& c) {
      for (auto& t : song.tracks) if (t.name == name) {
        for (Note n : c.notes) { n.start += at; t.midi.notes.push_back(n); }
        return;
      }
    };
    const bool full = kind == "chorus" || kind == "verse";
    if (full) put("Drums", drumPart(sec.bars, pp, true));
    if (kind == "bridge") put("Drums", drumPart(sec.bars, [&] { auto q = pp; q.energy = 0.3; return q; }(), false));
    if (full || kind == "bridge") put("Bass", bassPart(mat.chords, pp));
    put("Keys", keysPart(mat.chords, pp));
    if (kind != "verse") put("Pad", padPart(mat.chords, pp));
    if (full) put("Melody", mat.melody);
    for (auto c : mat.chords) song.chords.push_back(c);
    song.layout.push_back({sec.name, kind, at, sec.bars});
    at += sec.bars * 4.0;
  }
  song.beats = at;
  for (auto& t : song.tracks) t.midi.lengthBeats = at;
  // drop tracks that ended up empty (e.g. a pad-only request with no pad sections)
  song.tracks.erase(std::remove_if(song.tracks.begin(), song.tracks.end(), [](const SongTrack& t) { return t.midi.notes.empty(); }), song.tracks.end());
  return song;
}

std::vector<Stem> renderStems(const Song& song, double sr) {
  std::vector<Stem> out;
  const std::size_t want = static_cast<std::size_t>(song.seconds() * sr) + static_cast<std::size_t>(2.5 * sr);
  for (auto& t : song.tracks) {
    Stem s;
    s.name = t.name; s.gainDb = t.gainDb; s.pan = t.pan;
    AudioBuffer raw = t.preset == "drums" ? renderDrums(t.midi, song.bpm, sr) : renderSynth(t.midi, synthPreset(t.preset), song.bpm, sr);
    s.audio = AudioBuffer(2, want);
    const std::size_t n = std::min(want, raw.frames);
    for (std::size_t c = 0; c < 2; ++c) std::copy(raw.channel(c), raw.channel(c) + n, s.audio.channel(c));
    out.push_back(std::move(s));
  }
  return out;
}

AudioBuffer mixStems(const Song& song, const std::vector<Stem>& stems, double sr, bool sidechain) {
  std::size_t frames = 0;
  for (auto& s : stems) frames = std::max(frames, s.audio.frames);
  AudioBuffer mix(2, frames);
  // kick envelope: 1 normally, dips on each kick and recovers over ~180 ms
  std::vector<float> duck;
  if (sidechain) {
    duck.assign(frames, 1.f);
    for (auto& t : song.tracks) if (t.preset == "drums")
      for (auto& n : t.midi.notes) if (n.pitch == Kick) {
        const std::size_t s0 = static_cast<std::size_t>(n.start * 60.0 / song.bpm * sr);
        const std::size_t len = static_cast<std::size_t>(0.18 * sr);
        for (std::size_t i = 0; i < len && s0 + i < frames; ++i) {
          const float x = static_cast<float>(i) / static_cast<float>(len);
          const float g = 1.f - 0.5f * (1.f - x) * (1.f - x);
          duck[s0 + i] = std::min(duck[s0 + i], g);
        }
      }
  }
  for (auto& s : stems) {
    const float g = std::pow(10.f, s.gainDb / 20.f);
    const float pl = std::cos((s.pan * 0.5f + 0.5f) * 1.5707963f) * 1.41421356f, pr = std::sin((s.pan * 0.5f + 0.5f) * 1.5707963f) * 1.41421356f;
    const bool ducked = sidechain && (s.name == "Bass" || s.name == "Pad" || s.name == "Keys");
    for (std::size_t i = 0; i < s.audio.frames; ++i) {
      const float d = ducked ? duck[i] : 1.f;
      mix.channel(0)[i] += s.audio.channel(0)[i] * g * pl * d;
      mix.channel(1)[i] += s.audio.channel(1)[i] * g * pr * d;
    }
  }
  return mix;
}

bool writeProject(const Song& song, const std::string& dir, double sr, ProjectFiles& files, std::string* error) {
  namespace fs = std::filesystem;
  auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
  std::error_code ec;
  fs::create_directories(fs::path(dir) / "stems", ec);
  if (ec) return fail("cannot create " + dir);
  files = ProjectFiles();
  files.dir = dir;
  const auto stems = renderStems(song, sr);
  for (auto& s : stems) {
    const std::string p = (fs::path(dir) / "stems" / (s.name + ".wav")).string();
    if (!io::writeWavFloat(p, s.audio, static_cast<unsigned>(sr), song.spec.title + " - " + s.name)) return fail("cannot write " + p);
    files.stems.push_back(p);
  }
  const AudioBuffer mix = mixStems(song, stems, sr, true);
  files.mix = (fs::path(dir) / "mix.wav").string();
  if (!io::writeWavFloat(files.mix, mix, static_cast<unsigned>(sr), song.spec.title)) return fail("cannot write mix.wav");
  files.midi = (fs::path(dir) / "song.mid").string();
  std::vector<MidiClip> clips;
  for (auto& t : song.tracks) clips.push_back(t.midi);
  if (!saveMidiFile(files.midi, clips, song.bpm)) return fail("cannot write song.mid");

  session::Session s(sr, 2, 256);
  for (std::size_t i = 0; i < stems.size(); ++i) {
    const auto t = s.addTrack(stems[i].name, false, {});
    s.track(t)->strip->setGainDb(stems[i].gainDb);
    s.track(t)->strip->setPan(stems[i].pan);
    std::string e;
    if (s.addClipFromFile(t, files.stems[i], 0, &e) < 0) return fail("cannot add clip: " + e);
  }
  const std::int64_t beatFrames = static_cast<std::int64_t>(60.0 / song.bpm * sr);
  for (auto& p : song.layout) {
    edit::MemoryLocation m;
    m.name = p.name;
    m.start = m.end = static_cast<std::int64_t>(p.startBeat) * beatFrames;
    s.memory().add(m);
  }
  files.session = (fs::path(dir) / "song.wezt").string();
  std::string e;
  if (!s.save(files.session, &e)) return fail("cannot save session: " + e);
  return true;
}

}  // namespace wezt::music
