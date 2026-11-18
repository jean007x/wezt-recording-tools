#include "instruments/sampler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>

#include "engine/denormals.h"
#include "io/wav.h"

namespace wezt::instruments {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kControl = 16;  // samples per control step (pitch, filter, LFO)
enum EvType : std::uint8_t { kOn = 1, kOff = 2, kCc = 3, kBend = 4 };

struct EnvState {
  int stage = 0;  // 0 attack, 1 decay, 2 sustain, 3 release, 4 hold
  float level = 0.f, relFrom = 0.f, holdT = 0.f;
  bool done = false;
};

float stepEnv(EnvState& s, const Envelope& e, float dt, bool release) {
  if (release && s.stage != 3) { s.stage = 3; s.relFrom = s.level; }
  switch (s.stage) {
    case 0:
      if (e.attack <= 0.f) s.level = 1.f; else s.level += dt / e.attack;
      if (s.level >= 1.f) { s.level = 1.f; s.stage = e.hold > 0.f ? 4 : 1; }
      break;
    case 4:
      s.holdT += dt;
      if (s.holdT >= e.hold) s.stage = 1;
      break;
    case 1:
      if (e.decay <= 0.f) s.level = e.sustain; else s.level -= (1.f - e.sustain) * dt / e.decay;
      if (s.level <= e.sustain) { s.level = e.sustain; s.stage = 2; }
      break;
    case 2: s.level = e.sustain; break;
    default:
      if (e.release <= 0.f || s.relFrom <= 0.f) s.level = 0.f; else s.level -= s.relFrom * dt / e.release;
      if (s.level <= 0.f) { s.level = 0.f; s.done = true; }
  }
  return s.level;
}

float lfoValue(LfoShape shape, float ph) {  // ph in [0,1)
  switch (shape) {
    case LfoShape::Sine: return std::sin(2.f * static_cast<float>(kPi) * ph);
    case LfoShape::Triangle: return ph < 0.5f ? 4.f * ph - 1.f : 3.f - 4.f * ph;
    case LfoShape::Square: return ph < 0.5f ? 1.f : -1.f;
    case LfoShape::SawUp: return 2.f * ph - 1.f;
    default: return 1.f - 2.f * ph;
  }
}

inline float hermite(float y0, float y1, float y2, float y3, float t) {
  const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
  return ((c3 * t + c2) * t + c1) * t + y1;
}
}  // namespace

struct Sampler::Voice {
  bool active = false, held = false, sustained = false, releasing = false, stealing = false;
  const Zone* zone = nullptr;
  int key = 0, vel = 0;
  std::uint64_t age = 0;
  double pos = 0.0;
  int dir = 1;
  EnvState amp, fenv, penv;
  float lfoPhase[2] = {0.f, 0.f}, lfoTime = 0.f, curSemis = 0.f, stealGain = 1.f, ampMod = 1.f;
  float gL = 1.f, gR = 1.f;
  float s1[2] = {0, 0}, s2[2] = {0, 0};  // filter state per channel
  // resolved zone geometry
  std::int64_t s = 0, e = 0, ls = 0, le = 0;
  bool looping = false;
  int channels = 1;
};

struct Sampler::EditGuard {
  std::atomic_flag& f;
  explicit EditGuard(std::atomic_flag& fl) : f(fl) { while (f.test_and_set(std::memory_order_acquire)) std::this_thread::yield(); }
  ~EditGuard() { f.clear(std::memory_order_release); }
};

void Sampler::killVoices() { for (auto& v : *voices_) v = Voice{}; active_.store(0, std::memory_order_relaxed); }

void Sampler::setParams(const SamplerParams& p) { EditGuard g(edit_); params_ = p; }

void Sampler::updateZone(std::size_t index, Zone z) {
  EditGuard g(edit_);
  if (index >= zones_.size()) return;
  killVoices();
  zones_[index] = std::move(z);
}

bool Sampler::removeZone(std::size_t index) {
  EditGuard g(edit_);
  if (index >= zones_.size()) return false;
  killVoices();
  zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

Sampler::Sampler() : voices_(std::make_unique<std::array<Voice, 64>>()) {}
Sampler::~Sampler() = default;

// ---------------------------------------------------------------------------------------------
// zones, mapping, slicing
int Sampler::addZone(Zone z) {
  z.keyLo = std::clamp(z.keyLo, 0, 127); z.keyHi = std::clamp(z.keyHi, 0, 127);
  if (z.keyLo > z.keyHi) std::swap(z.keyLo, z.keyHi);
  z.velLo = std::clamp(z.velLo, 1, 127); z.velHi = std::clamp(z.velHi, 1, 127);
  if (z.velLo > z.velHi) std::swap(z.velLo, z.velHi);
  z.rootKey = std::clamp(z.rootKey, 0, 127);
  z.rrGroup = std::clamp(z.rrGroup, 0, 63);
  EditGuard g(edit_);
  killVoices();  // the zone list may move in memory
  zones_.push_back(std::move(z));
  return static_cast<int>(zones_.size() - 1);
}

void Sampler::clearZones() {
  EditGuard g(edit_);
  killVoices();
  zones_.clear();
}

bool Sampler::loadSampleFile(const std::string& wav, Zone& zone, std::string* error) {
  io::WavReader rd;
  if (!rd.open(wav, error)) return false;
  const auto& info = rd.info();
  if (info.frames == 0 || info.channels == 0 || info.channels > 2) {
    if (error) *error = "sample must have 1 or 2 channels and at least one frame";
    return false;
  }
  auto buf = std::make_shared<AudioBuffer>(info.channels, static_cast<std::size_t>(info.frames));
  std::vector<float*> dst(info.channels);
  for (unsigned c = 0; c < info.channels; ++c) dst[c] = buf->channel(c);
  const std::size_t got = rd.read(0, static_cast<std::size_t>(info.frames), dst.data());
  if (got == 0) { if (error) *error = "could not read samples"; return false; }
  buf->setFrames(got);
  zone.audio = std::move(buf);
  zone.file = wav;
  zone.sampleRate = info.sampleRate;
  return true;
}

int Sampler::noteNameToKey(const std::string& text) {
  // Strip a directory and extension, then look at the last note name in the rest.
  std::string s = text;
  if (auto p = s.find_last_of("/\\"); p != std::string::npos) s = s.substr(p + 1);
  if (auto p = s.find_last_of('.'); p != std::string::npos && p > 0) s = s.substr(0, p);
  int found = -1;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char ch = s[i];
    if (ch < 'A' || ch > 'G') continue;
    if (i > 0 && std::isalpha(static_cast<unsigned char>(s[i - 1]))) continue;  // inside a word
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    int semi = semis[ch - 'A'];
    std::size_t j = i + 1;
    if (j < s.size() && s[j] == '#') { ++semi; ++j; }
    else if (j < s.size() && s[j] == 'b') { --semi; ++j; }
    bool neg = false;
    if (j < s.size() && s[j] == '-') { neg = true; ++j; }
    if (j >= s.size() || !std::isdigit(static_cast<unsigned char>(s[j]))) continue;
    int oct = 0;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && oct < 100) oct = oct * 10 + (s[j++] - '0');
    if (neg) oct = -oct;
    const int key = (oct + 2) * 12 + semi;  // C3 = 60
    if (key >= 0 && key <= 127) found = key;
  }
  if (found >= 0) return found;
  // A plain number between 0 and 127 as its own word.
  for (std::size_t i = 0; i < s.size();) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; continue; }
    std::size_t j = i;
    int v = 0;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && v < 1000) v = v * 10 + (s[j++] - '0');
    const bool wordStart = i == 0 || !std::isalpha(static_cast<unsigned char>(s[i - 1]));
    const bool wordEnd = j >= s.size() || !std::isalpha(static_cast<unsigned char>(s[j]));
    if (wordStart && wordEnd && v <= 127) found = v;
    i = j;
  }
  return found;
}

int Sampler::autoMap(const std::vector<std::string>& files, std::string* error) {
  std::vector<Zone> items;
  std::string problems;
  for (const auto& f : files) {
    const int root = noteNameToKey(f);
    if (root < 0) { problems += "no note name in \"" + f + "\"; "; continue; }
    Zone z;
    std::string err;
    if (!loadSampleFile(f, z, &err)) { problems += "\"" + f + "\": " + err + "; "; continue; }
    z.rootKey = root;
    items.push_back(std::move(z));
  }
  std::stable_sort(items.begin(), items.end(), [](const Zone& a, const Zone& b) { return a.rootKey < b.rootKey; });
  std::vector<int> roots;
  for (const auto& z : items) if (roots.empty() || roots.back() != z.rootKey) roots.push_back(z.rootKey);
  for (std::size_t i = 0; i < items.size();) {
    const int root = items[i].rootKey;
    std::size_t j = i;
    while (j < items.size() && items[j].rootKey == root) ++j;
    const auto ri = static_cast<std::size_t>(std::find(roots.begin(), roots.end(), root) - roots.begin());
    const int lo = ri == 0 ? 0 : (roots[ri - 1] + root) / 2 + 1;
    const int hi = ri + 1 == roots.size() ? 127 : (root + roots[ri + 1]) / 2;
    for (std::size_t k = i; k < j; ++k) {
      items[k].keyLo = lo; items[k].keyHi = hi;
      if (j - i > 1) { items[k].rrGroup = 1 + root % 63; items[k].rrIndex = static_cast<int>(k - i); }
    }
    i = j;
  }
  const int n = static_cast<int>(items.size());
  for (auto& z : items) addZone(std::move(z));
  if (error) *error = problems;
  return n;
}

int Sampler::sliceEqual(std::shared_ptr<const AudioBuffer> audio, double sampleRate, int count, int firstKey) {
  if (!audio || audio->frames == 0 || count < 1) return 0;
  count = std::min({count, 128, static_cast<int>(audio->frames)});
  const std::int64_t total = static_cast<std::int64_t>(audio->frames);
  int made = 0;
  for (int i = 0; i < count && firstKey + i <= 127; ++i) {
    Zone z;
    z.audio = audio; z.sampleRate = sampleRate;
    z.keyLo = z.keyHi = z.rootKey = firstKey + i;
    z.start = total * i / count;
    z.end = total * (i + 1) / count;
    z.oneShot = true;
    addZone(std::move(z));
    ++made;
  }
  return made;
}

std::vector<std::int64_t> Sampler::findTransients(const AudioBuffer& audioRef, double sampleRate, float sensitivity, int maxSlices) {
  const AudioBuffer* audio = &audioRef;
  if (audio->frames == 0) return {};
  const std::size_t hop = std::max<std::size_t>(1, static_cast<std::size_t>(sampleRate * 0.005));
  const std::size_t nh = audio->frames / hop;
  std::vector<float> en(nh, 0.f);
  for (std::size_t h = 0; h < nh; ++h) {
    double sum = 0.0;
    for (std::size_t c = 0; c < audio->channels; ++c)
      for (std::size_t i = h * hop; i < (h + 1) * hop; ++i) { const double v = audio->channel(c)[i]; sum += v * v; }
    en[h] = static_cast<float>(std::sqrt(sum / static_cast<double>(hop * audio->channels)));
  }
  float peak = 0.f;
  for (float v : en) peak = std::max(peak, v);
  std::vector<std::int64_t> cuts{0};
  if (peak > 0.f) {
    const float ratio = 1.5f + (1.f - std::clamp(sensitivity, 0.f, 1.f)) * 6.f;
    const std::size_t minGap = std::max<std::size_t>(1, static_cast<std::size_t>(0.05 / 0.005));
    std::size_t lastCut = 0;
    for (std::size_t h = 1; h < nh; ++h) {
      float avg = 0.f;
      const std::size_t from = h > 8 ? h - 8 : 0;
      for (std::size_t k = from; k < h; ++k) avg += en[k];
      avg /= static_cast<float>(h - from);
      if (en[h] > 0.05f * peak && en[h] > avg * ratio && h - lastCut >= minGap && static_cast<int>(cuts.size()) < maxSlices) {
        cuts.push_back(static_cast<std::int64_t>(h * hop));
        lastCut = h;
      }
    }
  }
  return cuts;
}

int Sampler::sliceTransients(std::shared_ptr<const AudioBuffer> audio, double sampleRate, float sensitivity, int maxSlices, int firstKey) {
  if (!audio || audio->frames == 0) return 0;
  const std::vector<std::int64_t> cuts = findTransients(*audio, sampleRate, sensitivity, maxSlices);
  const std::int64_t total = static_cast<std::int64_t>(audio->frames);
  int made = 0;
  for (std::size_t i = 0; i < cuts.size() && firstKey + static_cast<int>(i) <= 127; ++i) {
    Zone z;
    z.audio = audio; z.sampleRate = sampleRate;
    z.keyLo = z.keyHi = z.rootKey = firstKey + static_cast<int>(i);
    z.start = cuts[i];
    z.end = i + 1 < cuts.size() ? cuts[i + 1] : total;
    z.oneShot = true;
    addZone(std::move(z));
    ++made;
  }
  return made;
}

// ---------------------------------------------------------------------------------------------
// events
bool Sampler::push(Event e) {
  const auto h = head_.load(std::memory_order_relaxed);
  const auto n = (h + 1) % kQueue;
  if (n == tail_.load(std::memory_order_acquire)) return false;
  queue_[h] = e;
  head_.store(n, std::memory_order_release);
  return true;
}
bool Sampler::noteOn(int key, int vel) {
  if (key < 0 || key > 127) return false;
  if (vel <= 0) return noteOff(key);  // MIDI convention
  return push({kOn, static_cast<std::int16_t>(key), static_cast<std::int16_t>(std::min(vel, 127))});
}
bool Sampler::noteOff(int key) { return key >= 0 && key <= 127 && push({kOff, static_cast<std::int16_t>(key), 0}); }
bool Sampler::controller(int cc, int value) { return cc >= 0 && cc <= 127 && push({kCc, static_cast<std::int16_t>(cc), static_cast<std::int16_t>(std::clamp(value, 0, 127))}); }
bool Sampler::pitchBend(int v) { return push({kBend, static_cast<std::int16_t>(std::clamp(v, 0, 16383)), 0}); }

Sampler::Voice* Sampler::allocate() {
  auto& vs = *voices_;
  const std::size_t limit = static_cast<std::size_t>(std::clamp(params_.polyphony, 1, 64));
  std::size_t live = 0;
  for (auto& v : vs) if (v.active && !v.stealing) ++live;
  if (live >= limit) {
    // Steal the quietest releasing voice, else the oldest; it fades out over a few ms.
    Voice* victim = nullptr;
    for (auto& v : vs) if (v.active && !v.stealing && v.releasing && (!victim || v.amp.level < victim->amp.level)) victim = &v;
    if (!victim) for (auto& v : vs) if (v.active && !v.stealing && (!victim || v.age < victim->age)) victim = &v;
    if (victim) victim->stealing = true;
  }
  for (auto& v : vs) if (!v.active) return &v;
  Voice* o = nullptr;  // every physical voice is busy: take the oldest
  for (auto& v : vs) if (!o || v.age < o->age) o = &v;
  return o;
}

void Sampler::startNote(int key, int vel) {
  const SamplerParams& p = params_;
  // Legato: a note played while another is held just changes the pitch of the sounding voices.
  if (p.mono && p.legato) {
    bool any = false;
    for (auto& v : *voices_) if (v.active && v.held && !v.stealing) { v.key = key; any = true; }
    if (any) { lastKey_ = key; return; }
  }
  int plain[64], nPlain = 0;
  int grp[64][64], gCount[64] = {};
  for (std::size_t i = 0; i < zones_.size(); ++i) {
    const Zone& z = zones_[i];
    if (key < z.keyLo || key > z.keyHi || vel < z.velLo || vel > z.velHi) continue;
    if (z.rrGroup == 0) { if (nPlain < 64) plain[nPlain++] = static_cast<int>(i); }
    else if (gCount[z.rrGroup] < 64) grp[z.rrGroup][gCount[z.rrGroup]++] = static_cast<int>(i);
  }
  int chosen[128], nChosen = 0;
  for (int i = 0; i < nPlain; ++i) chosen[nChosen++] = plain[i];
  for (int g = 1; g < 64; ++g) {
    const int n = gCount[g];
    if (!n) continue;
    std::stable_sort(grp[g], grp[g] + n, [&](int a, int b) { return zones_[static_cast<std::size_t>(a)].rrIndex < zones_[static_cast<std::size_t>(b)].rrIndex; });
    int k;
    if (p.alternation == Alternation::Random) { rng_ = rng_ * 1664525u + 1013904223u; k = static_cast<int>((rng_ >> 8) % static_cast<unsigned>(n)); }
    else k = rrCounter_[static_cast<std::size_t>(g)]++ % n;
    chosen[nChosen++] = grp[g][k];
  }
  if (nChosen == 0) return;
  const float from = (p.mono && lastKey_ >= 0 && p.glideSeconds > 0.f) ? glideFrom_ : static_cast<float>(key);
  if (p.mono) for (auto& v : *voices_) if (v.active) v.stealing = true;  // one voice at a time (layers of one note excepted)
  for (int c = 0; c < nChosen; ++c) {
    const Zone& z = zones_[static_cast<std::size_t>(chosen[c])];
    const std::int64_t frames = z.audio ? static_cast<std::int64_t>(z.audio->frames) : 0;
    Voice tmp;
    tmp.s = std::clamp<std::int64_t>(z.start, 0, frames);
    tmp.e = z.end < 0 ? frames : std::clamp<std::int64_t>(z.end, 0, frames);
    if (tmp.e <= tmp.s) continue;  // missing or empty sample: silent
    Voice* v = allocate();
    if (!v) return;
    tmp.active = true; tmp.held = true; tmp.zone = &z; tmp.key = key; tmp.vel = vel; tmp.age = ++age_;
    tmp.channels = static_cast<int>(std::min<std::size_t>(2, z.audio->channels));
    tmp.ls = std::clamp<std::int64_t>(z.loopStart, tmp.s, tmp.e);
    tmp.le = z.loopEnd < 0 ? tmp.e : std::clamp<std::int64_t>(z.loopEnd, tmp.s, tmp.e);
    tmp.looping = z.loop != LoopMode::Off && tmp.le - tmp.ls >= 2;
    tmp.dir = z.reverse ? -1 : 1;
    tmp.pos = z.reverse ? static_cast<double>(tmp.e - 1) : static_cast<double>(tmp.s);
    tmp.curSemis = from;
    tmp.lfoPhase[0] = p.lfo[0].phase; tmp.lfoPhase[1] = p.lfo[1].phase;
    *v = tmp;
  }
  glideFrom_ = static_cast<float>(key);
  lastKey_ = key;
}

void Sampler::releaseKey(int key) {
  for (auto& v : *voices_) {
    if (!v.active || !v.held || v.key != key) continue;
    v.held = false;
    if (v.zone->oneShot) continue;
    if (sustain_) v.sustained = true; else v.releasing = true;
  }
}

void Sampler::handle(const Event& e) {
  switch (e.type) {
    case kOn: startNote(e.a, e.b); break;
    case kOff: releaseKey(e.a); break;
    case kBend: bend_ = (static_cast<float>(e.a) - 8192.f) / 8192.f; break;
    case kCc:
      if (e.a == 1) modWheel_ = static_cast<float>(e.b) / 127.f;
      else if (e.a == 64) {
        const bool down = e.b >= 64;
        if (sustain_ && !down) for (auto& v : *voices_) if (v.active && v.sustained) { v.sustained = false; v.releasing = true; }
        sustain_ = down;
      } else if (e.a == 123) {
        sustain_ = false;
        for (auto& v : *voices_) if (v.active && !v.zone->oneShot) { v.held = false; v.sustained = false; v.releasing = true; }
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------
// rendering
void Sampler::prepare(double sampleRate, std::size_t maxFrames) {
  rate_ = sampleRate;
  scratchL_.assign(std::max<std::size_t>(maxFrames, kControl), 0.f);
  scratchR_.assign(std::max<std::size_t>(maxFrames, kControl), 0.f);
  for (auto& v : *voices_) v = Voice{};
  active_.store(0, std::memory_order_relaxed);
}

void Sampler::render(Voice& v, float* outL, float* outR, std::size_t n) {
  const SamplerParams& p = params_;
  const Zone& z = *v.zone;
  const AudioBuffer& a = *z.audio;
  const float* ch0 = a.channel(0);
  const float* ch1 = v.channels > 1 ? a.channel(1) : ch0;
  const float dt = 1.f / static_cast<float>(rate_);
  const double zoneRatio = z.sampleRate / rate_;
  const float velN = static_cast<float>(v.vel) / 127.f;
  const float velGain = (1.f - p.velocityToAmp) + p.velocityToAmp * velN;
  const float zoneGain = std::pow(10.f, (z.gainDb + p.masterGainDb) / 20.f);
  // Pan: constant power for a mono sample, balance for a stereo one.
  const float zpan = std::clamp(z.pan + p.pan, -1.f, 1.f);
  const float theta = (zpan + 1.f) * static_cast<float>(kPi) / 4.f;
  float panL, panR;
  if (v.channels == 1) { panL = std::cos(theta); panR = std::sin(theta); }
  else { panL = zpan > 0.f ? 1.f - zpan : 1.f; panR = zpan < 0.f ? 1.f + zpan : 1.f; }

  auto fetch = [&](const float* d, std::int64_t i) -> float {
    if (v.looping && z.loop == LoopMode::Forward && i >= v.le) i = v.ls + (i - v.ls) % (v.le - v.ls);
    if (i < v.s) i = v.s;
    if (i >= v.e) return 0.f;
    return d[i];
  };
  auto interp = [&](const float* d, double pos) -> float {
    const std::int64_t i = static_cast<std::int64_t>(std::floor(pos));
    const float t = static_cast<float>(pos - static_cast<double>(i));
    return hermite(fetch(d, i - 1), fetch(d, i), fetch(d, i + 1), fetch(d, i + 2), t);
  };

  std::size_t done = 0;
  while (done < n && v.active) {
    const std::size_t blk = std::min(kControl, n - done);
    // ---- control-rate modulation ----
    v.lfoTime += dt * static_cast<float>(blk);
    float lfoV[2];
    for (int k = 0; k < 2; ++k) {
      const LfoParams& l = p.lfo[k];
      const double hz = l.sync ? static_cast<double>(tempo_.load(std::memory_order_relaxed)) / 60.0 / std::max(0.0625, static_cast<double>(l.syncBeats)) : static_cast<double>(l.rateHz);
      double ph;
      if (l.poly && l.keyTrigger) { v.lfoPhase[k] += static_cast<float>(hz) * dt * static_cast<float>(blk); v.lfoPhase[k] -= std::floor(v.lfoPhase[k]); ph = v.lfoPhase[k]; }
      else { ph = gStart_[k] + hz * static_cast<double>(dt) * static_cast<double>(done + blk) + static_cast<double>(l.phase); ph -= std::floor(ph); }
      float val = lfoValue(l.shape, static_cast<float>(ph));
      if (!l.bipolar) val = 0.5f * (val + 1.f);
      if (l.fadeIn > 0.f) val *= std::min(1.f, v.lfoTime / l.fadeIn);
      if (l.via == LfoVia::ModWheel) val *= modWheel_; else if (l.via == LfoVia::Velocity) val *= velN;
      lfoV[k] = l.on ? val : 0.f;
    }
    float semis = v.curSemis;
    if (p.glideSeconds > 0.f) {
      const float target = static_cast<float>(v.key), step = 12.f * dt * static_cast<float>(blk) / p.glideSeconds;  // one octave per glide time
      if (semis < target) semis = std::min(target, semis + step); else semis = std::max(target, semis - step);
      v.curSemis = semis;
    } else { v.curSemis = semis = static_cast<float>(v.key); }
    const float pEnv = stepEnv(v.penv, p.pitchEnv, dt * static_cast<float>(blk), v.releasing || (!v.held && !v.sustained));
    const float pEnvCents = p.pitchEnvCents * pEnv * ((1.f - p.pitchEnvVelocity) + p.pitchEnvVelocity * velN);
    const float lfoCents = lfoV[0] * p.lfo[0].pitchCents + lfoV[1] * p.lfo[1].pitchCents;
    const float keyPart = p.pitchKeyTrack ? semis - static_cast<float>(z.rootKey) : 0.f;
    const float totalSemis = keyPart + p.coarseSemitones + (z.tuneCents + p.masterTuneCents + lfoCents + pEnvCents) / 100.f + bend_ * p.pitchBendSemitones;
    const double inc = zoneRatio * std::pow(2.0, static_cast<double>(totalSemis) / 12.0);
    float fk = 0.f, fa1 = 0.f, fa2 = 0.f, fa3 = 0.f;
    const bool filt = p.filter != FilterType::Off;
    // advance the filter envelope once per control step
    float fenvV = 0.f;
    if (filt) {
      fenvV = stepEnv(v.fenv, p.filterEnv, dt * static_cast<float>(blk), v.releasing || (!v.held && !v.sustained));
      const float oct = fenvV * p.filterEnvOctaves + p.filterKeyTrack * static_cast<float>(v.key - 60) / 12.f + p.filterVelOctaves * velN + lfoV[0] * p.lfo[0].filterOctaves + lfoV[1] * p.lfo[1].filterOctaves;
      const float fcut = std::clamp(p.cutoffHz * std::pow(2.f, oct), 20.f, static_cast<float>(rate_) * 0.45f);
      const float g = std::tan(static_cast<float>(kPi) * fcut / static_cast<float>(rate_));
      const float q = 0.7071f + std::clamp(p.resonance, 0.f, 1.f) * 19.3f;
      fk = 1.f / q;
      fa1 = 1.f / (1.f + g * (g + fk)); fa2 = g * fa1; fa3 = g * fa2;
    }
    float ampMod = 1.f;   // tremolo: each LFO takes the level from 1 down to 1 - depth
    for (int k = 0; k < 2; ++k) ampMod *= 1.f + (lfoV[k] - 1.f) * 0.5f * std::clamp(p.lfo[k].amp, 0.f, 1.f);
    const float ampFrom = v.ampMod, ampTo = std::max(0.f, ampMod);
    const bool relNow = v.releasing || (!v.held && !v.sustained);

    for (std::size_t i = 0; i < blk; ++i) {
      // amplitude envelope (per sample)
      const float env = stepEnv(v.amp, p.amp, dt, relNow);
      float gain = env * velGain * zoneGain;
      gain *= ampFrom + (ampTo - ampFrom) * static_cast<float>(i + 1) / static_cast<float>(blk);
      if (v.stealing) { v.stealGain -= dt / 0.003f; if (v.stealGain <= 0.f) { v.active = false; break; } gain *= v.stealGain; }
      // read
      double pos = v.pos;
      float sl, sr;
      const bool xf = v.looping && z.loop == LoopMode::Forward && z.loopCrossfade > 0 && v.dir > 0;
      sl = interp(ch0, pos);
      sr = v.channels > 1 ? interp(ch1, pos) : sl;
      if (xf) {
        const double span = static_cast<double>(std::min<std::int64_t>(z.loopCrossfade, v.le - v.ls));
        const double into = pos - (static_cast<double>(v.le) - span);
        if (into > 0.0) {
          const float t = static_cast<float>(std::min(1.0, into / span));
          const double back = pos - static_cast<double>(v.le - v.ls);
          const float bl = interp(ch0, back), br = v.channels > 1 ? interp(ch1, back) : bl;
          sl = sl * (1.f - t) + bl * t;
          sr = sr * (1.f - t) + br * t;
        }
      }
      if (filt) {
        float* x[2] = {&sl, &sr};
        for (int c = 0; c < (v.channels > 1 ? 2 : 1); ++c) {
          float in = *x[c];
          if (p.filterDrive > 0.f) { const float g = 1.f + p.filterDrive * 9.f; in = std::tanh(g * in) / g * std::sqrt(g); }
          const float v3 = in - v.s2[c];
          const float v1 = fa1 * v.s1[c] + fa2 * v3;
          const float v2 = v.s2[c] + fa2 * v.s1[c] + fa3 * v3;
          v.s1[c] = 2.f * v1 - v.s1[c];
          v.s2[c] = 2.f * v2 - v.s2[c];
          *x[c] = p.filter == FilterType::LowPass ? v2 : p.filter == FilterType::BandPass ? v1 : in - fk * v1 - v2;
        }
        if (v.channels == 1) sr = sl;
      }
      outL[done + i] += sl * gain * panL;
      outR[done + i] += sr * gain * panR;
      // advance
      pos += static_cast<double>(v.dir) * inc;
      if (v.looping) {
        if (z.loop == LoopMode::Forward) {
          if (v.dir > 0) { while (pos >= static_cast<double>(v.le)) pos -= static_cast<double>(v.le - v.ls); }
          else { while (pos < static_cast<double>(v.ls)) pos += static_cast<double>(v.le - v.ls); }
        } else {
          if (pos >= static_cast<double>(v.le)) { pos = 2.0 * static_cast<double>(v.le) - pos; v.dir = -1; }
          else if (pos < static_cast<double>(v.ls)) { pos = 2.0 * static_cast<double>(v.ls) - pos; v.dir = 1; }
        }
      } else if (pos >= static_cast<double>(v.e) || pos < static_cast<double>(v.s)) { v.active = false; break; }
      v.pos = pos;
      if (v.amp.done) { v.active = false; break; }
    }
    v.ampMod = ampTo;
    done += blk;
  }
}

// ---------------------------------------------------------------------------------------------
// MIDI from the sequencer: stored with their offsets, applied inside process() at the right sample
void Sampler::handleMidi(const MidiEvent* events, std::size_t count) {
  // If the last block was skipped (an edit held the lock), keep its releases so no note sticks.
  std::size_t kept = 0;
  for (std::size_t i = 0; i < midiN_; ++i)
    if (midi_[i].type == MidiEvent::NoteOff || midi_[i].type == MidiEvent::AllNotesOff) { midi_[kept] = midi_[i]; midi_[kept++].offset = 0; }
  midiN_ = kept;
  for (std::size_t i = 0; i < count && midiN_ < midi_.size(); ++i) midi_[midiN_++] = events[i];
  std::stable_sort(midi_.begin(), midi_.begin() + static_cast<std::ptrdiff_t>(midiN_), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
}

// ---------------------------------------------------------------------------------------------
// universal control surface
namespace {
struct SamplerParamDef { const char* name; const char* unit; double min, max, def; int steps; bool log; const char* semantic; };
const SamplerParamDef kSamplerParams[] = {
    {"Filter type", "", 0, 3, 0, 3, false, "mode"},          // 0 off, 1 low-pass, 2 high-pass, 3 band-pass
    {"Cutoff", "Hz", 20, 20000, 20000, 0, true, "cutoff"},
    {"Resonance", "", 0, 1, 0, 0, false, "resonance"},
    {"Filter drive", "", 0, 1, 0, 0, false, "drive"},
    {"Filter envelope", "oct", -6, 6, 0, 0, false, "envelope"},
    {"Attack", "s", 0, 5, 0.005, 0, false, "attack"},
    {"Decay", "s", 0, 5, 0.1, 0, false, "decay"},
    {"Sustain", "", 0, 1, 1, 0, false, "sustain"},
    {"Release", "s", 0, 10, 0.2, 0, false, "release"},
    {"Gain", "dB", -48, 12, 0, 0, false, "gain"},
    {"Pan", "", -1, 1, 0, 0, false, "pan"},
    {"Fine tune", "cents", -100, 100, 0, 0, false, "tune"},
    {"Coarse tune", "semitones", -24, 24, 0, 48, false, "tune"},
    {"Glide", "s", 0, 2, 0, 0, false, "glide"},
    {"Velocity sensitivity", "", 0, 1, 1, 0, false, "velocity"},
};
constexpr std::size_t kSamplerParamCount = sizeof kSamplerParams / sizeof kSamplerParams[0];
double samplerNorm(const SamplerParamDef& d, double v) {
  v = std::clamp(v, d.min, d.max);
  if (d.log) return std::log(v / d.min) / std::log(d.max / d.min);
  return d.max == d.min ? 0.0 : (v - d.min) / (d.max - d.min);
}
double samplerPlain(const SamplerParamDef& d, double n) {
  n = std::clamp(std::isfinite(n) ? n : 0.0, 0.0, 1.0);
  if (d.steps > 0) n = std::round(n * d.steps) / d.steps;
  return d.log ? d.min * std::pow(d.max / d.min, n) : d.min + n * (d.max - d.min);
}
}  // namespace

std::size_t Sampler::parameterCount() const { return kSamplerParamCount; }

std::vector<plugins::ParamInfo> Sampler::parameters() const {
  std::vector<plugins::ParamInfo> out;
  for (std::size_t i = 0; i < kSamplerParamCount; ++i) {
    const auto& d = kSamplerParams[i];
    plugins::ParamInfo p;
    p.id = static_cast<unsigned>(i); p.name = d.name; p.defaultNormalized = samplerNorm(d, d.def);
    p.min = d.min; p.max = d.max; p.unit = d.unit; p.steps = d.steps; p.logScale = d.log; p.semantic = d.semantic;
    out.push_back(std::move(p));
  }
  return out;
}

double Sampler::parameterNormalized(unsigned id) const {
  if (id >= kSamplerParamCount) return 0.0;
  const SamplerParams& p = params_;
  const double v[] = {static_cast<double>(p.filter), p.cutoffHz, p.resonance, p.filterDrive, p.filterEnvOctaves, p.amp.attack, p.amp.decay, p.amp.sustain, p.amp.release,
                      p.masterGainDb, p.pan, p.masterTuneCents, p.coarseSemitones, p.glideSeconds, p.velocityToAmp};
  return samplerNorm(kSamplerParams[id], v[id]);
}

static bool applySamplerParam(SamplerParams& p, unsigned id, double normalized) {
  if (id >= kSamplerParamCount || !std::isfinite(normalized)) return false;
  const float v = static_cast<float>(samplerPlain(kSamplerParams[id], normalized));
  switch (id) {
    case 0: p.filter = static_cast<FilterType>(static_cast<int>(std::lround(v))); break;
    case 1: p.cutoffHz = v; if (p.filter == FilterType::Off) p.filter = FilterType::LowPass; break;  // a cutoff does nothing with the filter off
    case 2: p.resonance = v; break;
    case 3: p.filterDrive = v; break;
    case 4: p.filterEnvOctaves = v; break;
    case 5: p.amp.attack = v; break;
    case 6: p.amp.decay = v; break;
    case 7: p.amp.sustain = v; break;
    case 8: p.amp.release = v; break;
    case 9: p.masterGainDb = v; break;
    case 10: p.pan = v; break;
    case 11: p.masterTuneCents = v; break;
    case 12: p.coarseSemitones = v; break;
    case 13: p.glideSeconds = v; break;
    case 14: p.velocityToAmp = v; break;
    default: return false;
  }
  return true;
}

bool Sampler::setParameterNormalized(unsigned id, double normalized) {
  EditGuard g(edit_);   // read-modify-write under the guard so a concurrent automated change is not overwritten
  return applySamplerParam(params_, id, normalized);
}

bool Sampler::automateParameter(unsigned id, double normalized) {   // audio thread: gives up instead of waiting
  if (edit_.test_and_set(std::memory_order_acquire)) return false;
  const bool ok = applySamplerParam(params_, id, normalized);
  edit_.clear(std::memory_order_release);
  return ok;
}

std::string Sampler::parameterText(unsigned id, double normalized) const {
  if (id >= kSamplerParamCount) return {};
  const auto& d = kSamplerParams[id];
  const double v = samplerPlain(d, normalized);
  if (id == 0) { static const char* names[] = {"Off", "Low-pass", "High-pass", "Band-pass"}; return names[std::clamp(static_cast<int>(std::lround(v)), 0, 3)]; }
  char buf[64];
  if (std::string(d.unit) == "Hz" && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000.0);
  else std::snprintf(buf, sizeof buf, "%.*f%s%s", std::fabs(v) >= 100 ? 0 : 2, v, *d.unit ? " " : "", d.unit);
  return buf;
}

void Sampler::process(const AudioBuffer& in, AudioBuffer& out) {
  ScopedNoDenormals nd;
  out.clearValid();
  if (edit_.test_and_set(std::memory_order_acquire)) return;  // an edit is in progress: this block stays silent
  struct Release { std::atomic_flag& f; ~Release() { f.clear(std::memory_order_release); } } release{edit_};
  for (std::size_t c = 0; c < std::min(in.channels, out.channels); ++c)
    std::copy(in.channel(c), in.channel(c) + std::min(in.frames, out.frames), out.channel(c));
  // events
  for (;;) {
    const auto t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) break;
    const Event e = queue_[t];
    tail_.store((t + 1) % kQueue, std::memory_order_release);
    handle(e);
  }
  if (out.channels == 0) return;
  std::size_t off = 0, mi = 0;
  const std::size_t cap = scratchL_.size();
  while (off < out.frames && cap > 0) {
    // sequencer events whose time has come take effect now; the segment ends at the next one
    while (mi < midiN_ && midi_[mi].offset <= off) {
      const MidiEvent& m = midi_[mi++];
      switch (m.type) {
        case MidiEvent::NoteOn: if (m.b > 0) handle({kOn, m.a, std::min<std::int16_t>(m.b, 127)}); else handle({kOff, m.a, 0}); break;
        case MidiEvent::NoteOff: handle({kOff, m.a, 0}); break;
        case MidiEvent::Controller: handle({kCc, m.a, std::clamp<std::int16_t>(m.b, 0, 127)}); break;
        case MidiEvent::PitchBend: handle({kBend, std::clamp<std::int16_t>(m.a, 0, 16383), 0}); break;
        case MidiEvent::AllNotesOff: handle({kCc, 123, 0}); break;
      }
    }
    std::size_t n = std::min(cap, out.frames - off);
    if (mi < midiN_) n = std::min(n, std::max<std::size_t>(1, midi_[mi].offset - off));
    std::fill(scratchL_.begin(), scratchL_.begin() + static_cast<std::ptrdiff_t>(n), 0.f);
    std::fill(scratchR_.begin(), scratchR_.begin() + static_cast<std::ptrdiff_t>(n), 0.f);
    for (int k = 0; k < 2; ++k) gStart_[k] = gPhase_[k];
    for (auto& v : *voices_) if (v.active) render(v, scratchL_.data(), scratchR_.data(), n);
    for (int k = 0; k < 2; ++k) {
      const LfoParams& l = params_.lfo[k];
      const double hz = l.sync ? static_cast<double>(tempo_.load(std::memory_order_relaxed)) / 60.0 / std::max(0.0625, static_cast<double>(l.syncBeats)) : static_cast<double>(l.rateHz);
      gPhase_[k] += hz * static_cast<double>(n) / rate_;
      gPhase_[k] -= std::floor(gPhase_[k]);
    }
    float* l = out.channel(0) + off;
    float* r = out.channels > 1 ? out.channel(1) + off : nullptr;
    for (std::size_t i = 0; i < n; ++i) {
      if (r) { l[i] += scratchL_[i]; r[i] += scratchR_[i]; }
      else l[i] += 0.5f * (scratchL_[i] + scratchR_[i]);
    }
    off += n;
  }
  while (mi < midiN_) { const MidiEvent& m = midi_[mi++]; if (m.type == MidiEvent::NoteOff) handle({kOff, m.a, 0}); }  // offsets past the end: only releases matter
  midiN_ = 0;
  int act = 0;
  for (auto& v : *voices_) if (v.active) ++act;
  active_.store(act, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// patches
namespace {
template <class F> void forEachParam(SamplerParams& p, F f) {
  auto e = [&](const char* name, auto& en) { int v = static_cast<int>(en); f(name, v); en = static_cast<std::remove_reference_t<decltype(en)>>(v); };
  f("polyphony", p.polyphony); f("mono", p.mono); f("legato", p.legato); f("glide", p.glideSeconds);
  f("ampA", p.amp.attack); f("ampD", p.amp.decay); f("ampS", p.amp.sustain); f("ampR", p.amp.release);
  e("filter", p.filter); f("cutoff", p.cutoffHz); f("resonance", p.resonance);
  f("fenvOct", p.filterEnvOctaves); f("fkey", p.filterKeyTrack); f("fvel", p.filterVelOctaves);
  f("fenvA", p.filterEnv.attack); f("fenvD", p.filterEnv.decay); f("fenvS", p.filterEnv.sustain); f("fenvR", p.filterEnv.release);
  f("ampH", p.amp.hold); f("fenvH", p.filterEnv.hold);
  f("coarse", p.coarseSemitones); f("pitchKey", p.pitchKeyTrack);
  f("penvA", p.pitchEnv.attack); f("penvH", p.pitchEnv.hold); f("penvD", p.pitchEnv.decay); f("penvS", p.pitchEnv.sustain); f("penvR", p.pitchEnv.release);
  f("penvCents", p.pitchEnvCents); f("penvVel", p.pitchEnvVelocity); f("drive", p.filterDrive); e("mode", p.mode);
  for (int k = 0; k < 2; ++k) {
    LfoParams& l = p.lfo[k];
    const std::string pre = "lfo" + std::to_string(k);
    auto n = [&](const char* s2) { return pre + s2; };
    f(n("On").c_str(), l.on); e(n("Shape").c_str(), l.shape); f(n("Rate").c_str(), l.rateHz); f(n("Sync").c_str(), l.sync); f(n("Beats").c_str(), l.syncBeats);
    f(n("Fade").c_str(), l.fadeIn); f(n("Phase").c_str(), l.phase); f(n("Bi").c_str(), l.bipolar); f(n("Poly").c_str(), l.poly); f(n("Key").c_str(), l.keyTrigger);
    e(n("Via").c_str(), l.via); f(n("Pitch").c_str(), l.pitchCents); f(n("Filter").c_str(), l.filterOctaves); f(n("Amp").c_str(), l.amp);
  }
  f("velAmp", p.velocityToAmp); f("bendRange", p.pitchBendSemitones); f("gain", p.masterGainDb); f("tune", p.masterTuneCents); f("pan", p.pan);
  e("alternation", p.alternation);
}
void put(std::ostringstream& o, const char* n, int v) { o << "p\t" << n << '\t' << v << '\n'; }
void put(std::ostringstream& o, const char* n, bool v) { o << "p\t" << n << '\t' << (v ? 1 : 0) << '\n'; }
void put(std::ostringstream& o, const char* n, float v) { char b[48]; std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v)); o << "p\t" << n << '\t' << b << '\n'; }
}  // namespace

std::string Sampler::savePatch() const {
  std::ostringstream o;
  o << "WEZT-SAMPLER 1\n";
  SamplerParams copy = params_;
  forEachParam(copy, [&](const char* n, auto& v) { put(o, n, v); });
  for (const auto& z : zones_) {
    char b[400];
    std::snprintf(b, sizeof b, "\tk=%d-%d\tv=%d-%d\troot=%d\ttune=%.9g\tgain=%.9g\tpan=%.9g\tstart=%lld\tend=%lld\tloop=%d\tls=%lld\tle=%lld\txf=%lld\trev=%d\tone=%d\trr=%d,%d\tsr=%.9g",
                  z.keyLo, z.keyHi, z.velLo, z.velHi, z.rootKey, static_cast<double>(z.tuneCents), static_cast<double>(z.gainDb), static_cast<double>(z.pan),
                  static_cast<long long>(z.start), static_cast<long long>(z.end), static_cast<int>(z.loop), static_cast<long long>(z.loopStart),
                  static_cast<long long>(z.loopEnd), static_cast<long long>(z.loopCrossfade), z.reverse ? 1 : 0, z.oneShot ? 1 : 0, z.rrGroup, z.rrIndex, z.sampleRate);
    o << "z\t" << z.file << b << '\n';
  }
  return o.str();
}

bool Sampler::loadPatch(const std::string& text, std::string* error) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line != "WEZT-SAMPLER 1") { if (error) *error = "not a Wezt sampler patch"; return false; }
  SamplerParams np;
  std::vector<Zone> nz;
  std::string warnings;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::vector<std::string> f;
    std::size_t a = 0;
    for (;;) { const auto t = line.find('\t', a); f.push_back(line.substr(a, t == std::string::npos ? t : t - a)); if (t == std::string::npos) break; a = t + 1; }
    if (f[0] == "p" && f.size() == 3) {
      forEachParam(np, [&](const char* n, auto& v) {
        if (f[1] != n) return;
        using T = std::remove_reference_t<decltype(v)>;
        if constexpr (std::is_same_v<T, float>) v = static_cast<float>(std::atof(f[2].c_str()));
        else v = static_cast<T>(std::atoi(f[2].c_str()));
      });
    } else if (f[0] == "z" && f.size() >= 2) {
      Zone z;
      z.file = f[1];
      for (std::size_t i = 2; i < f.size(); ++i) {
        const auto eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        long long a1 = 0, a2 = 0;
        if (k == "k") std::sscanf(v.c_str(), "%d-%d", &z.keyLo, &z.keyHi);
        else if (k == "v") std::sscanf(v.c_str(), "%d-%d", &z.velLo, &z.velHi);
        else if (k == "root") z.rootKey = std::atoi(v.c_str());
        else if (k == "tune") z.tuneCents = static_cast<float>(std::atof(v.c_str()));
        else if (k == "gain") z.gainDb = static_cast<float>(std::atof(v.c_str()));
        else if (k == "pan") z.pan = static_cast<float>(std::atof(v.c_str()));
        else if (k == "start") z.start = std::atoll(v.c_str());
        else if (k == "end") z.end = std::atoll(v.c_str());
        else if (k == "loop") z.loop = static_cast<LoopMode>(std::clamp(std::atoi(v.c_str()), 0, 2));
        else if (k == "ls") z.loopStart = std::atoll(v.c_str());
        else if (k == "le") z.loopEnd = std::atoll(v.c_str());
        else if (k == "xf") z.loopCrossfade = std::atoll(v.c_str());
        else if (k == "rev") z.reverse = v == "1";
        else if (k == "one") z.oneShot = v == "1";
        else if (k == "rr") { std::sscanf(v.c_str(), "%lld,%lld", &a1, &a2); z.rrGroup = static_cast<int>(a1); z.rrIndex = static_cast<int>(a2); }
        else if (k == "sr") z.sampleRate = std::atof(v.c_str());
      }
      if (!z.file.empty()) {
        Zone loaded = z;
        std::string err;
        if (!loadSampleFile(z.file, loaded, &err)) warnings += "missing sample \"" + z.file + "\"; ";
        else { z.audio = loaded.audio; z.sampleRate = loaded.sampleRate; }
      }
      nz.push_back(std::move(z));
    } else { if (error) *error = "bad line in patch"; return false; }
  }
  {
    EditGuard g(edit_);
    killVoices();
    zones_.clear();
    params_ = np;
  }
  for (auto& z : nz) addZone(std::move(z));
  if (error) *error = warnings;
  return true;
}

}  // namespace wezt::instruments
