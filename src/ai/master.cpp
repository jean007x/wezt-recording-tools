#include "ai/master.h"

#include <algorithm>
#include <cmath>

namespace wezt::ai {

const std::vector<DeliveryTarget>& deliveryTargets() {
  static const std::vector<DeliveryTarget> t = {
      {"streaming", -14.f, -1.f}, {"apple", -16.f, -1.f}, {"cd", -9.f, -0.3f}, {"broadcast", -23.f, -1.f}, {"club", -8.f, -0.5f}};
  return t;
}

MasterSettings planMaster(const Features& f, const std::vector<Suggestion>& sug, float fallbackLufs, float ceilingDb) {
  MasterSettings m;
  m.ceilingDb = ceilingDb;
  m.targetLufs = fallbackLufs;
  for (const auto& s : sug) {
    switch (s.kind) {
      case SuggestionKind::Loudness: m.targetLufs = f.lufs + s.value; break;
      case SuggestionKind::EqBand: {
        dsp::EqBand b;
        b.type = s.band == 0 ? dsp::FilterType::LowShelf : s.band == kBands - 1 ? dsp::FilterType::HighShelf : dsp::FilterType::Peaking;
        b.freq = kBandCenters[static_cast<std::size_t>(std::clamp(s.band, 0, kBands - 1))];
        b.gainDb = s.value;
        b.q = 0.9f;  // about an octave wide
        if (b.type != dsp::FilterType::Peaking) b.q = 0.7f;
        m.eq.push_back(b);
        break;
      }
      case SuggestionKind::Dynamics:
        if (s.value < 0.f) {  // the target is more compressed than the track
          m.compress = true;
          const float amt = std::min(4.f, -s.value);
          m.comp.ratio = 1.5f + amt * 0.5f;               // 1.5:1 .. 3.5:1
          m.comp.thresholdDb = f.rmsDb + 3.f;             // act on the louder parts only
          m.comp.attackMs = 30.f; m.comp.releaseMs = 250.f; m.comp.kneeDb = 8.f;
        }  // more punch than the track has cannot be added by a compressor; the advice text says so
        break;
      case SuggestionKind::Width: {
        const float s0 = std::clamp(f.stereoWidth, 0.001f, 0.99f), s1 = std::clamp(f.stereoWidth + s.value, 0.001f, 0.99f);
        const float k2 = (s1 / (1.f - s1)) / (s0 / (1.f - s0));
        m.widthSide = std::clamp(std::sqrt(k2), 0.5f, 1.6f);
        break;
      }
    }
  }
  // Safety limit: asking for more than about -7 LUFS only destroys dynamics for no gain on streaming services.
  m.targetLufs = std::clamp(m.targetLufs, -30.f, -7.f);
  return m;
}


MasterResult applyMaster(const AudioBuffer& in, double sr, const MasterSettings& s) {
  MasterResult r;
  r.before = analyze(in, sr);
  r.settings = s;
  if (!r.before.valid() || in.channels == 0) return r;

  // 1. tone, dynamics, width (these do not depend on the final level)
  AudioBuffer cur = in, nxt;
  auto stage = [&](Processor& p) {
    p.prepare(sr, 1024);
    nxt = AudioBuffer(cur.channels, cur.frames);
    AudioBuffer bi(cur.channels, 1024), bo(cur.channels, 1024);
    for (std::size_t pos = 0; pos < cur.frames; pos += 1024) {
      const std::size_t n = std::min<std::size_t>(1024, cur.frames - pos);
      bi.setFrames(n); bo.setFrames(n);
      for (std::size_t c = 0; c < cur.channels; ++c) std::copy(cur.channel(c) + pos, cur.channel(c) + pos + n, bi.channel(c));
      p.process(bi, bo);
      for (std::size_t c = 0; c < cur.channels; ++c) std::copy(bo.channel(c), bo.channel(c) + n, nxt.channel(c) + pos);
    }
    cur = std::move(nxt);
  };
  if (!s.eq.empty()) { dsp::ParametricEq eq; eq.setBands(s.eq); stage(eq); }
  if (s.compress) { dsp::Compressor c; c.set(s.comp); stage(c); }
  if (s.widthSide != 1.f && cur.channels >= 2) { dsp::StereoWidth w; w.setWidth(s.widthSide); stage(w); }

  // 2. loudness + limiter, corrected by measuring: gain, limit, measure, repeat.
  float gainDb = 0.f, ceil = s.ceilingDb;
  AudioBuffer result;
  Features f;
  const Features pre = analyze(cur, sr);
  gainDb = pre.valid() ? s.targetLufs - pre.lufs : 0.f;
  if (!std::isfinite(gainDb)) gainDb = 0.f;
  gainDb = std::clamp(gainDb, -30.f, 30.f);
  for (int pass = 1; pass <= 6; ++pass) {
    r.passes = pass;
    AudioBuffer g(cur.channels, cur.frames + 1);  // +1 so the buffer is never empty
    g.setFrames(cur.frames);
    const float lin = dsp::dbToGain(gainDb);
    for (std::size_t c = 0; c < cur.channels; ++c) for (std::size_t i = 0; i < cur.frames; ++i) g.channel(c)[i] = cur.channel(c)[i] * lin;
    if (s.limiter) {
      dsp::Limiter lim;
      dsp::LimiterParams lp; lp.ceilingDb = ceil;
      lim.set(lp);
      lim.prepare(sr, 1024);
      const std::size_t L = lim.latencySamples();
      AudioBuffer padded(g.channels, g.frames + L), outp(g.channels, g.frames + L);
      for (std::size_t c = 0; c < g.channels; ++c) std::copy(g.channel(c), g.channel(c) + g.frames, padded.channel(c));  // the tail is zeros
      AudioBuffer bi(g.channels, 1024), bo(g.channels, 1024);
      for (std::size_t pos = 0; pos < padded.frames; pos += 1024) {
        const std::size_t n = std::min<std::size_t>(1024, padded.frames - pos);
        bi.setFrames(n); bo.setFrames(n);
        for (std::size_t c = 0; c < g.channels; ++c) std::copy(padded.channel(c) + pos, padded.channel(c) + pos + n, bi.channel(c));
        lim.process(bi, bo);
        for (std::size_t c = 0; c < g.channels; ++c) std::copy(bo.channel(c), bo.channel(c) + n, outp.channel(c) + pos);
      }
      result = AudioBuffer(g.channels, cur.frames);
      for (std::size_t c = 0; c < g.channels; ++c) std::copy(outp.channel(c) + L, outp.channel(c) + L + cur.frames, result.channel(c));
    } else {
      result = AudioBuffer(g.channels, cur.frames);
      for (std::size_t c = 0; c < g.channels; ++c) std::copy(g.channel(c), g.channel(c) + cur.frames, result.channel(c));
    }
    f = analyze(result, sr);
    const float lufsErr = s.targetLufs - f.lufs, tpOver = f.truePeakDb - s.ceilingDb;
    const bool loudOk = std::fabs(lufsErr) <= 0.3f, peakOk = !s.limiter || tpOver <= 0.05f;
    if (loudOk && peakOk) break;
    if (!peakOk) ceil -= std::min(1.f, tpOver + 0.05f);   // the true peak overshot: lower the limiter's ceiling
    if (!loudOk) gainDb += std::clamp(lufsErr, -6.f, 6.f);  // the limiter took some level: push the input harder
    // at the loudness limit the target cannot be reached without breaking the ceiling; stop after the passes
  }
  r.audio = std::move(result);
  r.after = f;
  r.gainDb = gainDb;
  r.limiterCeilingDb = ceil;
  r.ok = true;
  return r;
}

}  // namespace wezt::ai
