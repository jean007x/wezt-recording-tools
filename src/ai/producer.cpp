#include "ai/producer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

#include "ai/audio_file.h"
#include "ai/master.h"
#include "edit/clip_tools.h"
#include "io/wav.h"
#include "ai/features.h"
#include "dsp/elastic.h"
#include "edit/beat_detective.h"
#include "music/midi_edit.h"
#include "voice/singing.h"
#include "music/audio_to_midi.h"
#include "music/separation.h"
#include "music/vocal_fit.h"
#include "voice/voice_model.h"
#include "voice/voice_takes.h"

namespace wezt::ai {
namespace {
namespace fs = std::filesystem;
using session::TrackId;

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string safeName(std::string s) { for (auto& c : s) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != ' ') c = '_'; return s; }

bool roleMatches(Role want, const std::string& name) {
  const Role r = guessRole(name);
  if (want == Role::Drums) return r == Role::Drums || r == Role::Kick;
  return r == want;
}

}  // namespace

std::string Producer::path(const std::string& file) const {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  return (fs::path(dir_) / file).string();
}

std::vector<TrackId> Producer::tracksByRole(Role role) const {
  std::vector<TrackId> out;
  for (TrackId t : s_.trackIds()) if (roleMatches(role, s_.track(t)->name)) out.push_back(t);
  return out;
}

TrackId Producer::trackByName(const std::string& name) const {
  const std::string n = lower(name);
  for (TrackId t : s_.trackIds()) if (lower(s_.track(t)->name) == n) return t;
  for (TrackId t : s_.trackIds()) if (lower(s_.track(t)->name).find(n) != std::string::npos) return t;
  return -1;
}

bool Producer::addStemTrack(const std::string& name, const AudioBuffer& audio, float gainDb, float pan, std::string& err) {
  const std::string file = path(safeName(name) + "_" + std::to_string(s_.trackIds().size()) + ".wav");
  if (!io::writeWavFloat(file, audio, static_cast<unsigned>(s_.sampleRate()), name)) { err = "cannot write " + file; return false; }
  const TrackId t = s_.addTrack(name, false, {});
  s_.track(t)->strip->setGainDb(gainDb);
  s_.track(t)->strip->setPan(pan);
  if (s_.addClipFromFile(t, file, 0, &err) < 0) return false;
  return true;
}

bool Producer::firstClipAudio(TrackId t, AudioBuffer& out, double& rate, std::int64_t& start) const {
  session::Track* tr = s_.track(t);
  if (!tr) return false;
  auto pl = tr->source->playlist();
  if (pl.empty()) return false;
  std::sort(pl.begin(), pl.end(), [](const session::Clip& a, const session::Clip& b) { return a.start < b.start; });
  auto r = edit::renderClip(pl.front());
  if (!r) return false;
  out = std::move(*r);
  rate = s_.sampleRate();
  start = pl.front().start;
  return true;
}

Outcome Producer::generateSong(const music::SongSpec& spec) {
  const music::Song song = music::composeSong(spec);
  const double sr = s_.sampleRate();
  const auto stems = music::renderStems(song, sr);
  std::string err;
  for (auto& st : stems) if (!addStemTrack(st.name, st.audio, st.gainDb, st.pan, err)) return {false, "Could not add the " + st.name + " track: " + err};
  // MIDI and chart next to the audio, so the song can be reopened in any other DAW
  std::vector<music::MidiClip> clips;
  for (auto& t : song.tracks) { clips.push_back(t.midi); if (t.name == "Melody") lastMelody_ = t.midi; }
  lastBpm_ = song.bpm; lastKey_ = song.key;
  const std::string midi = path(safeName(song.spec.title) + ".mid");
  music::saveMidiFile(midi, clips, song.bpm);
  const std::int64_t beatFrames = static_cast<std::int64_t>(60.0 / song.bpm * sr);
  for (auto& p : song.layout) { edit::MemoryLocation m; m.name = p.name; m.start = m.end = static_cast<std::int64_t>(p.startBeat) * beatFrames; s_.memory().add(m); }
  std::ostringstream o;
  o << "Composed \"" << song.spec.title << "\": " << song.tracks.size() << " tracks, " << static_cast<int>(song.seconds()) << " seconds. Tempo " << static_cast<int>(std::lround(song.bpm))
    << " bpm; set the session tempo to match.\n" << song.chartText() << "MIDI saved to " << midi << ".\nThe sounds are the built-in synth and drums; swap in your own instruments for the real thing.";
  return {true, o.str()};
}

Outcome Producer::addPart(const std::string& kind0, const music::Key& key, const std::string& style, const std::string& mood, int bars, std::uint64_t seed, double bpm, const std::string& nameIn) {
  const std::string kind = lower(kind0);
  using namespace music;
  if (bars < 1 || bars > 64) return {false, "Choose between 1 and 64 bars."};
  const StyleInfo si = styleInfo(style);
  const double tempo = bpm > 0 ? bpm : si.defaultBpm;
  ProgressionParams pp; pp.key = key; pp.style = si.name; pp.mood = mood; pp.bars = bars; pp.seed = seed;
  const auto chords = generateProgression(pp);
  PartParams part; part.key = key; part.style = si.name; part.seed = seed; part.energy = 0.6;
  MidiClip clip; std::string preset, name;
  if (kind == "pad") { clip = padPart(chords, part); preset = "pad"; name = "Pad"; }
  else if (kind == "bass") { clip = bassPart(chords, part); preset = "bass"; name = "Bass"; }
  else if (kind == "keys") { clip = keysPart(chords, part); preset = "keys"; name = "Keys"; }
  else if (kind == "drums") { clip = drumPart(bars, part); preset = "drums"; name = "Drums"; }
  else if (kind == "melody" || kind == "lead") { MelodyParams mp; mp.key = key; mp.style = si.name; mp.mood = mood; mp.seed = seed; clip = generateMelody(chords, mp); preset = "lead"; name = "Melody"; }
  else return {false, "I can add drums, bass, keys, pad or melody."};
  if (!nameIn.empty()) name = nameIn;
  const double sr = s_.sampleRate();
  AudioBuffer audio = preset == "drums" ? renderDrums(clip, tempo, sr) : renderSynth(clip, synthPreset(preset), tempo, sr);
  std::string err;
  const float gain = preset == "pad" ? -9.f : preset == "bass" ? -3.f : -5.f;
  if (!addStemTrack(name, audio, gain, 0.f, err)) return {false, err};
  saveMidiFile(path(safeName(name) + ".mid"), {clip}, tempo);
  if (preset == "lead") { lastMelody_ = clip; lastBpm_ = tempo; lastKey_ = key; }
  std::string chordsText;
  for (auto& c : chords) chordsText += (chordsText.empty() ? "" : " | ") + c.name();
  return {true, "Added \"" + name + "\" (" + std::to_string(bars) + " bars, " + keyName(key) + ", " + mood + ", " + std::to_string(static_cast<int>(std::lround(tempo))) + " bpm). Chords: " + chordsText + "."};
}

Outcome Producer::separateStems(const std::string& trackName, bool preferNeural) {
  const TrackId t = trackByName(trackName);
  if (t < 0) return {false, "I could not find a track called \"" + trackName + "\"."};
  AudioBuffer in; double rate = 0; std::int64_t start = 0;
  if (!firstClipAudio(t, in, rate, start)) return {false, "That track has no audio clip to split."};
  music::StemSet st;
  std::string method = "built-in DSP splitter", err;
  bool done = false;
  if (preferNeural && music::externalSeparatorAvailable()) {
    const std::string tmp = path("_sep_in.wav");
    io::writeWavFloat(tmp, in, static_cast<unsigned>(rate));
    double r2 = 0;
    if (music::separateExternal(tmp, path("_sep_out"), st, r2, &err)) { done = true; method = "Demucs (neural, installed on this computer)"; }
  }
  if (!done && !music::separateDsp(in, rate, st, &err)) return {false, "Could not split: " + err};
  const std::string base = s_.track(t)->name;
  for (auto& [label, buf] : std::vector<std::pair<std::string, AudioBuffer*>>{{"Drums", &st.drums}, {"Bass", &st.bass}, {"Vocals", &st.vocals}, {"Other", &st.other}})
    if (!addStemTrack(base + " - " + label, *buf, 0.f, 0.f, err)) return {false, err};
  return {true, "Split \"" + base + "\" into four tracks using the " + method + "." + (method[0] == 'b' ? " This method is rough: drums and bass separate usefully, vocals only when they are centred. Install Demucs for much better results." : "")};
}

Outcome Producer::audioToMidi(const std::string& trackName, const std::string& instrument, double bpm, bool beatbox) {
  const TrackId t = trackByName(trackName);
  if (t < 0) return {false, "I could not find a track called \"" + trackName + "\"."};
  AudioBuffer in; double rate = 0; std::int64_t start = 0;
  if (!firstClipAudio(t, in, rate, start)) return {false, "That track has no audio clip."};
  music::AudioToMidiParams p; p.bpm = bpm > 0 ? bpm : 120;
  const music::MidiClip clip = beatbox ? music::beatboxToDrums(in, rate, p) : music::audioToMidi(in, rate, p);
  if (clip.notes.empty()) return {false, "I did not find any notes. Is the recording clear and loud enough?"};
  const std::string inst = beatbox ? "drums" : (lower(instrument).empty() ? "keys" : lower(instrument));
  AudioBuffer audio = inst == "drums" ? music::renderDrums(clip, p.bpm, rate) : music::renderSynth(clip, music::synthPreset(inst), p.bpm, rate);
  std::string err;
  const std::string name = s_.track(t)->name + (beatbox ? " (drums)" : " (MIDI)");
  if (!addStemTrack(name, audio, -4.f, 0.f, err)) return {false, err};
  const std::string midi = path(safeName(name) + ".mid");
  music::saveMidiFile(midi, {clip}, p.bpm);
  return {true, "Found " + std::to_string(clip.notes.size()) + (beatbox ? " drum hits" : " notes") + " at " + std::to_string(static_cast<int>(p.bpm)) + " bpm and added a track playing them. MIDI file: " + midi + "."};
}

Outcome Producer::fitVocal(const std::string& vocalTrack, const std::string& midiFile, double bpm, bool harmony) {
  const TrackId t = trackByName(vocalTrack);
  if (t < 0) return {false, "I could not find a track called \"" + vocalTrack + "\". Record or import your vocal on a track first."};
  AudioBuffer in; double rate = 0; std::int64_t start = 0;
  if (!firstClipAudio(t, in, rate, start)) return {false, "That track has no audio clip."};
  music::MidiClip melody; double tempo = bpm > 0 ? bpm : lastBpm_; music::Key key = lastKey_;
  if (!midiFile.empty()) {
    music::MidiFile mf; std::string e;
    if (!music::loadMidiFile(midiFile, mf, &e)) return {false, "Could not read the MIDI file: " + e};
    const music::MidiClip* pick = nullptr;
    for (auto& c : mf.clips) if (c.channel != 9 && !c.notes.empty()) { if (!pick || lower(c.name).find("melody") != std::string::npos) pick = &c; }
    if (!pick) return {false, "That MIDI file has no melody notes."};
    melody = *pick; if (bpm <= 0) tempo = mf.bpm;
  } else {
    melody = lastMelody_;
    if (melody.notes.empty()) return {false, "There is no melody yet. Compose a song or add a melody part first, or give me a .mid file."};
  }
  if (tempo <= 0) tempo = 120;
  music::VocalFitParams vp; vp.bpm = tempo; vp.key = key; vp.harmony = harmony;
  const music::VocalFit f = music::fitVocal(in, rate, melody, vp);
  if (!f.ok) return {false, f.message};
  std::string err;
  const std::string base = s_.track(t)->name;
  if (!addStemTrack(base + " (fitted)", f.lead, 0.f, 0.f, err)) return {false, err};
  if (harmony && f.harmony.frames && !addStemTrack(base + " (harmony)", f.harmony, 0.f, 0.f, err)) return {false, err};
  return {true, f.message + " Added \"" + base + " (fitted)\"" + (harmony ? " and a harmony track" : "") + ", at " + std::to_string(static_cast<int>(std::lround(tempo))) +
          " bpm. This is your own recording retuned and retimed, not a voice model: the timbre stays yours, and large pitch moves sound less natural."};
}

Outcome Producer::makeChoir(const std::string& vocalTrack, const music::Key& key, int voices) {
  const TrackId t = trackByName(vocalTrack);
  if (t < 0) return {false, "I could not find a track called \"" + vocalTrack + "\"."};
  AudioBuffer in; double rate = 0; std::int64_t start = 0;
  if (!firstClipAudio(t, in, rate, start)) return {false, "That track has no audio clip."};
  music::ChoirParams cp; cp.key = key; cp.voices = voices;
  AudioBuffer out = music::makeChoir(in, rate, cp);
  if (out.frames == 0) return {false, "I could not find sung notes to build a choir from. Sing clearly, one syllable per note."};
  std::string err;
  const std::string name = s_.track(t)->name + " (choir)";
  if (!addStemTrack(name, out, -3.f, 0.f, err)) return {false, err};
  return {true, "Built a " + std::to_string(std::clamp(voices, 1, 8)) + "-voice choir from your recording in " + music::keyName(key) + " and added \"" + name + "\". It is your own take moved to other scale notes, detuned and spread in time."};
}

Outcome Producer::stretchTrack(const std::string& trackName, double ratio, double sourceBpm, double targetBpm, const std::string& modeText) {
  const TrackId t = trackByName(trackName);
  if (t < 0) return {false, "I could not find a track called \"" + trackName + "\"."};
  dsp::ElasticMode mode = dsp::ElasticMode::Polyphonic;
  if (!modeText.empty() && !dsp::parseElasticMode(lower(modeText).c_str(), mode)) return {false, "Mode must be polyphonic, rhythmic, monophonic or varispeed."};
  session::Track* tr = s_.track(t);
  auto pl = tr->source->playlist();
  if (pl.empty()) return {false, "That track has no clips."};
  std::string how;
  if (targetBpm > 0) {
    if (sourceBpm <= 0) {
      AudioBuffer a; double r = 0; std::int64_t st = 0;
      if (!firstClipAudio(t, a, r, st)) return {false, "That track has no audio clip."};
      const auto f = ai::analyze(a, r);
      if (f.bpm <= 0 || f.bpmConfidence < 0.2) return {false, "I could not measure the tempo reliably. Tell me the original tempo."};
      sourceBpm = f.bpm; how = " (measured tempo " + std::to_string(static_cast<int>(std::lround(sourceBpm))) + " bpm)";
    }
    ratio = sourceBpm / targetBpm;
  }
  if (ratio < 0.25 || ratio > 4.0) return {false, "That is a stretch of " + std::to_string(ratio) + "x, outside 0.25x..4x where the result stays usable."};
  for (auto& c : pl) {
    if (c.locked) continue;
    auto n = edit::elasticClip(c, ratio, mode, s_.sampleRate());
    if (!n) continue;
    n.value().id = c.id;
    n.value().start = static_cast<std::int64_t>(static_cast<double>(c.start) * ratio);
    c = *n;
  }
  tr->source->setPlaylist(std::move(pl));
  return {true, "Stretched \"" + tr->name + "\" to " + std::to_string(ratio) + "x its length (" + dsp::elasticModeName(mode) + ")" + how + ". Clip positions scaled with it. The edited audio is held in memory; save the session to keep the result."};
}

Outcome Producer::pitchTrack(const std::string& trackName, double semitones, const std::string& modeText) {
  const TrackId t = trackByName(trackName);
  if (t < 0) return {false, "I could not find a track called \"" + trackName + "\"."};
  dsp::ElasticMode mode = dsp::ElasticMode::Polyphonic;
  if (!modeText.empty() && !dsp::parseElasticMode(lower(modeText).c_str(), mode)) return {false, "Mode must be polyphonic, rhythmic, monophonic or varispeed."};
  if (std::fabs(semitones) > 12) return {false, "Shift by at most 12 semitones."};
  session::Track* tr = s_.track(t);
  auto pl = tr->source->playlist();
  if (pl.empty()) return {false, "That track has no clips."};
  for (auto& c : pl) {
    if (c.locked) continue;
    auto n = edit::elasticPitchClip(c, semitones, mode, s_.sampleRate());
    if (!n) continue;
    n.value().id = c.id; n.value().start = c.start;
    c = *n;
  }
  tr->source->setPlaylist(std::move(pl));
  return {true, "Shifted \"" + tr->name + "\" by " + std::to_string(semitones) + " semitones (" + dsp::elasticModeName(mode) + ")."};
}

std::vector<std::string> Producer::voiceModels() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(fs::path(dir_) / "voices", ec)) if (e.path().extension() == ".wvm") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> Producer::voiceBanks() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(fs::path(dir_) / "voices", ec)) if (e.path().extension() == ".wvb") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}


namespace {
bool readMono(const std::string& file, AudioBuffer& mono, double& rate, std::string& err) {
  io::WavReader r;
  if (!r.open(file, &err)) return false;
  const auto& info = r.info();
  AudioBuffer a(info.channels, static_cast<std::size_t>(info.frames));
  std::vector<float*> dst; for (unsigned c = 0; c < info.channels; ++c) dst.push_back(a.channel(c));
  r.read(0, static_cast<std::size_t>(info.frames), dst.data());
  mono = AudioBuffer(1, a.frames);
  for (std::size_t i = 0; i < a.frames; ++i) { float s = 0; for (unsigned c = 0; c < info.channels; ++c) s += a.channel(c)[i]; mono.channel(0)[i] = s / static_cast<float>(info.channels); }
  rate = info.sampleRate; return true;
}
}  // namespace

Outcome Producer::splitVoiceTakes(const std::string& wavFile) {
  AudioBuffer mono; double rate = 0; std::string err;
  if (!readMono(wavFile, mono, rate, err)) return {false, "Could not read the recording: " + err};
  const auto takes = voice::splitTakes(mono, rate);
  if (takes.empty()) return {false, "I found no separate sounds in that recording (it is silent, or the sounds run together without pauses)."};
  std::ostringstream o; o << takes.size() << " takes. Name the ones you want with build_voice_bank_from_takes, using what the person says they recorded, in order; the guesses below come from measurements only.\n";
  o << "idx  from-to(s)  kind  f0 F1 F2 hiss-centre guess\n";
  for (std::size_t i = 0; i < takes.size(); ++i) { const auto& t = takes[i]; char b[200]; std::snprintf(b, sizeof b, "%zu  %.2f-%.2f  %s  %.0f %.0f %.0f %.0f %s\n", i, t.from, t.to, t.kind.c_str(), t.f0, t.f1, t.f2, t.hissCentroidHz, t.guess.c_str()); o << b; }
  return {true, o.str()};
}

Outcome Producer::buildVoiceBankFromTakes(const std::string& wavFile, const std::string& use, const std::string& name, const std::string& owner, bool consent, bool derive, bool shortVowels) {
  if (!consent) return {false, "I can only build a voice bank from a voice that is yours, or whose owner agreed. Tell me: \"I confirm this is my own voice\" (or that the speaker agreed), and I will go ahead."};
  if (use.empty() || use.size() > 2000) return {false, "Say which takes are which: use is \"idx=name,idx=name\", for example \"0=s,1=t,4=eh,5=ah\" (at most 2000 characters)."};
  AudioBuffer mono; double rate = 0; std::string err;
  if (!readMono(wavFile, mono, rate, err)) return {false, "Could not read the recording: " + err};
  const auto takes = voice::splitTakes(mono, rate);
  if (takes.empty()) return {false, "I found no separate sounds in that recording."};
  voice::VoiceBank bank; bank.name = name.empty() ? "my voice bank" : name; bank.owner = owner; bank.consent = "the caller confirmed the voice is their own or the speaker agreed";
  const auto rep = voice::buildBankFromTakes(mono, rate, takes, use, bank, derive, shortVowels ? 0.15 : 0.35);
  if (bank.entries.empty() && bank.consonants.empty()) return {false, "Nothing could be used.\n" + rep.log};
  std::error_code ec; fs::create_directories(fs::path(dir_) / "voices", ec);
  if (!voice::saveVoiceBank(path("voices/" + safeName(bank.name) + ".wvb"), bank, &err)) return {false, "Built, but could not save: " + err};
  return {true, "Voice bank \"" + bank.name + "\" saved with " + std::to_string(bank.entries.size()) + " vowel and " + std::to_string(bank.consonants.size()) + " consonant recordings (" + std::to_string(rep.used) + " takes used, " + std::to_string(rep.skipped) + " skipped). Everything stayed on this computer.\n" + rep.log};
}

Outcome Producer::buildVoiceBank(const std::vector<std::pair<std::string, std::string>>& recs, const std::string& name, const std::string& owner, bool consent) {
  if (!consent) return {false, "I can only build a voice bank from a voice that is yours, or whose owner agreed. Tell me: \"I confirm this is my own voice\" (or that the speaker agreed), and I will go ahead."};
  if (recs.empty()) return {false, "Give pairs of vowel and track, for example ah -> \"Take ah\", ee -> \"Take ee\". Each track holds one held, steady note of that vowel."};
  static const char* ok[] = {"ah", "ee", "eh", "ae", "ih", "oh", "oo", "uh", "er"};   // vowels; consonant names (s, t, sh ...) are recordings of a syllable like "sah"
  voice::VoiceBank bank; bank.name = name.empty() ? "my voice bank" : name; bank.owner = owner; bank.consent = "the caller confirmed the voice is their own or the speaker agreed";
  std::string report;
  for (const auto& [vowel, track] : recs) {
    const std::string vw = lower(vowel);
    const bool isCons = voice::isConsonantName(vw);
    if (!isCons && std::none_of(std::begin(ok), std::end(ok), [&](const char* o) { return vw == o; })) return {false, "\"" + vowel + "\" is not a vowel or consonant I know. Vowels: ah, ee, eh, ae, ih, oh, oo, uh, er. Consonants (record the sound followed by \"ah\", like \"sah\"): p t k b d g f v s z sh ch th h m n l r w y j ng."};
    const TrackId t = trackByName(track);
    if (t < 0) return {false, "I could not find a track called \"" + track + "\"."};
    AudioBuffer a; double rate = 0; std::int64_t start = 0;
    if (!firstClipAudio(t, a, rate, start)) return {false, "The track \"" + track + "\" has no audio clip."};
    if (isCons) {
      const auto cr = voice::makeConsonantEntry(vw, a, rate);
      if (!cr.ok) return {false, cr.message};
      bank.consonants.push_back(cr.entry); report += (report.empty() ? "" : "; ") + cr.message;
      continue;
    }
    const auto r = voice::makeBankEntry(vw, a, rate);
    if (!r.ok) return {false, r.message};
    bank.entries.push_back(r.entry); report += (report.empty() ? "" : "; ") + r.message;
  }
  std::error_code ec;
  fs::create_directories(fs::path(dir_) / "voices", ec);
  const std::string file = path("voices/" + safeName(bank.name) + ".wvb");
  std::string err;
  if (!voice::saveVoiceBank(file, bank, &err)) return {false, "Built, but could not save: " + err};
  std::string missing;
  for (const char* o : {"ah", "ee", "oh", "oo", "eh"}) if (!bank.has(o)) missing += std::string(missing.empty() ? "" : ", ") + o;
  return {true, "Voice bank \"" + bank.name + "\" saved with " + std::to_string(bank.entries.size()) + " vowel and " + std::to_string(bank.consonants.size()) + " consonant recordings (" + report + "). Everything stayed on this computer." +
          (missing.empty() ? "" : " Not recorded yet: " + missing + " (the singer fills in with a similar vowel or its synthesizer).")};
}

Outcome Producer::trainVoice(const std::vector<std::string>& tracks, const std::string& modelName, const std::string& owner, bool consent) {
  if (!consent) return {false, "I can only train a voice model on a voice that is yours, or whose owner agreed. Tell me: \"I confirm this is my own voice\" (or that the speaker agreed), and I will go ahead."};
  if (tracks.empty()) return {false, "Name the track(s) that hold the recordings of the voice."};
  std::vector<AudioBuffer> recs;
  for (const auto& n : tracks) {
    const TrackId t = trackByName(n);
    if (t < 0) return {false, "I could not find a track called \"" + n + "\"."};
    for (const auto& c : s_.track(t)->source->playlist()) if (auto r = edit::renderClip(c)) recs.push_back(std::move(*r));
  }
  if (recs.empty()) return {false, "Those tracks have no audio."};
  voice::TrainParams tp; tp.name = modelName.empty() ? "my voice" : modelName; tp.owner = owner; tp.consent = consent;
  const auto r = voice::trainVoiceModel(recs, s_.sampleRate(), tp);
  if (!r.ok) return {false, r.message};
  std::error_code ec;
  fs::create_directories(fs::path(dir_) / "voices", ec);
  const std::string file = path("voices/" + safeName(tp.name) + ".wvm");
  std::string err;
  if (!voice::saveVoiceModel(file, r.model, &err)) return {false, "Trained, but could not save: " + err};
  return {true, r.message + " Saved to " + file + ". Everything stayed on this computer."};
}

Outcome Producer::convertVoice(const std::string& trackName, const std::string& modelName, double semitones) {
  const TrackId t = trackByName(trackName);
  if (t < 0) return {false, "I could not find a track called \"" + trackName + "\"."};
  voice::VoiceModel m; std::string err;
  if (!voice::loadVoiceModel(path("voices/" + safeName(modelName) + ".wvm"), m, &err)) {
    std::string have;
    for (auto& n : voiceModels()) have += (have.empty() ? "" : ", ") + n;
    return {false, "I could not load the voice model \"" + modelName + "\" (" + err + ")." + (have.empty() ? " No models are trained yet." : " Available: " + have + ".")};
  }
  AudioBuffer in; double rate = 0; std::int64_t start = 0;
  if (!firstClipAudio(t, in, rate, start)) return {false, "That track has no audio clip."};
  voice::ConvertParams cp; cp.pitchShiftSemitones = semitones;
  AudioBuffer out = voice::convertVoice(in, rate, m, cp);
  if (out.frames == 0) return {false, "I could not analyse that audio. It needs a clear, solo voice."};
  const std::string name = s_.track(t)->name + " (" + m.name + ")";
  if (!addStemTrack(name, out, 0.f, 0.f, err)) return {false, err};
  return {true, "Re-voiced \"" + s_.track(t)->name + "\" with the trained voice \"" + m.name + "\" (owner: " + (m.owner.empty() ? "not recorded" : m.owner) + ") on the new track \"" + name +
          "\". This is a classical vocoder-based conversion: it carries the timbre but sounds more synthetic than a neural voice system."};
}

Outcome Producer::singLyrics(const std::string& lyrics, const std::string& voiceName, const std::string& midiFile, double bpm, int choir, double humanize, double room, const std::string& character, const std::string& voiceModel, const std::string& voiceBank) {
  voice::SingParams sp;
  sp.humanize = std::clamp(humanize, 0.0, 1.0); sp.room = std::clamp(room, 0.0, 1.0);
  if (!character.empty()) {
    if (!voice::applyCharacter(character, sp)) { std::string have; for (auto& n : voice::characterNames()) have += (have.empty() ? "" : ", ") + n; return {false, "Characters: " + have + "."}; }
  }
  if ((character.empty() || !voiceName.empty()) && !voice::parseVoice(voiceName.empty() ? "alto" : voiceName, sp.voice)) return {false, "Voices: bass, tenor, alto, soprano."};
  music::MidiClip melody; double tempo = bpm > 0 ? bpm : lastBpm_;
  if (!midiFile.empty()) {
    music::MidiFile mf; std::string e;
    if (!music::loadMidiFile(midiFile, mf, &e)) return {false, "Could not read the MIDI file: " + e};
    const music::MidiClip* pick = nullptr;
    for (auto& c : mf.clips) if (c.channel != 9 && !c.notes.empty()) { if (!pick || lower(c.name).find("melody") != std::string::npos) pick = &c; }
    if (!pick) return {false, "That MIDI file has no melody notes."};
    melody = *pick; if (bpm <= 0) tempo = mf.bpm;
  } else {
    melody = lastMelody_;
    if (melody.notes.empty()) return {false, "There is no melody yet. Compose a song or add a melody part first, or give me a .mid file."};
  }
  sp.bpm = tempo > 0 ? tempo : 120; sp.sampleRate = s_.sampleRate(); sp.choirVoices = std::clamp(choir, 1, 8);
  voice::VoiceBank bank;
  if (!voiceBank.empty()) {
    std::string e2;
    if (!voice::loadVoiceBank(path("voices/" + safeName(voiceBank) + ".wvb"), bank, &e2)) {
      std::string have; for (auto& n : voiceBanks()) have += (have.empty() ? "" : ", ") + n;
      return {false, "I could not load the voice bank \"" + voiceBank + "\" (" + e2 + "). Banks: " + (have.empty() ? "none yet; record vowels and use build_voice_bank" : have) + "."};
    }
    sp.bank = &bank;
  }
  auto r = voice::sing(lyrics, melody, sp);
  if (!r.ok) return {false, r.message};
  std::string err, usedModel;
  if (sp.bank) {
    r.message += " Vowels come from the recordings in your voice bank \"" + bank.name + "\" (" + std::to_string(r.bankNotes) + " notes).";
    if (r.bankMissing) r.message += " " + std::to_string(r.bankMissing) + " note(s) used a vowel you did not record, so the synthesizer sang those.";
    if (r.bankConsonants + r.bankConsonantsSynth > 0) r.message += " Consonants: " + std::to_string(r.bankConsonants) + " from your recordings, " + std::to_string(r.bankConsonantsSynth) + " synthesized.";
    if (r.bankFarNotes) r.message += " " + std::to_string(r.bankFarNotes) + " note(s) are more than 5 semitones from the recorded pitch and will sound like a different-sized person; record that vowel at another pitch.";
  }
  if (!voiceModel.empty()) {   // put the sung line into a voice model trained on the person's own recordings (training needs consent)
    voice::VoiceModel m;
    if (!voice::loadVoiceModel(path("voices/" + safeName(voiceModel) + ".wvm"), m, &err)) {
      std::string have; for (auto& n : voiceModels()) have += (have.empty() ? "" : ", ") + n;
      return {false, "I could not load the voice model \"" + voiceModel + "\" (" + err + "). Models: " + (have.empty() ? "none yet; train one with train_voice_model" : have) + "."};
    }
    voice::ConvertParams cp; cp.matchRange = false;
    r.audio = voice::convertVoice(r.audio, s_.sampleRate(), m, cp);
    usedModel = m.name.empty() ? voiceModel : m.name;
  }
  const std::string name = sp.choirVoices > 1 ? "Choir (sung)" : "Vocal (sung)";
  if (!addStemTrack(name, r.audio, -3.f, 0.f, err)) return {false, err};
  if (!usedModel.empty()) r.message += " Timbre from your voice model \"" + usedModel + "\" (classical voice conversion: it captures your tone, not a neural clone).";
  return {true, r.message + " Added \"" + name + "\". This is a formant synthesizer built from scratch, not a trained singer: it now varies pitch, vibrato, timing and loudness like a person, but it will still sound synthetic next to a real voice. Use [ah]/[oo] after a syllable to fix a vowel; for a human timbre, train a voice model on your own voice and convert this track with it."};
}

Outcome Producer::beatDetective(const std::string& trackName, double bpm, double grid, double strength, double swing) {
  const TrackId t = trackByName(trackName);
  if (t < 0) return {false, "I could not find a track called \"" + trackName + "\"."};
  session::Track* tr = s_.track(t);
  auto pl = tr->source->playlist();
  if (pl.empty()) return {false, "That track has no clips."};
  if (bpm <= 0) {
    AudioBuffer a; double r = 0; std::int64_t st = 0;
    if (!firstClipAudio(t, a, r, st)) return {false, "That track has no audio clip."};
    const auto f = ai::analyze(a, r);
    if (f.bpm <= 0 || f.bpmConfidence < 0.2) return {false, "I could not measure the tempo reliably. Tell me the tempo."};
    bpm = f.bpm;
  }
  edit::BeatDetectiveParams p; p.bpm = bpm; p.gridBeats = grid > 0 ? grid : 0.25; p.strength = static_cast<float>(std::clamp(strength, 0.0, 1.0)); p.swing = swing;
  const auto r = edit::beatDetective(pl, s_.sampleRate(), p);
  if (r.hits == 0) return {false, "I did not find any hits to move. Try a track with clear drums or plucks."};
  int id = 1; auto out = r.clips; for (auto& c : out) c.id = id++;
  tr->nextClipId = id;
  tr->source->setPlaylist(std::move(out));
  std::ostringstream o;
  o.precision(1); o << std::fixed;
  o << "Found " << r.hits << " hits at " << bpm << " bpm and moved " << r.moved << " of them toward the " << p.gridBeats << "-beat grid (strength " << static_cast<int>(p.strength * 100)
    << "%). Average move " << r.meanShiftMs << " ms, largest " << r.maxShiftMs << " ms. The track is now cut into one clip per hit; undo restores the old playlist.";
  return {true, o.str()};
}

Outcome Producer::editMidi(const std::string& inFile, const std::string& outFile, const std::string& op0, double a, double b, const music::Key& key, const std::string& instrument, double bpm) {
  music::MidiFile mf; std::string e;
  if (!music::loadMidiFile(inFile, mf, &e)) return {false, "Could not read the MIDI file: " + e};
  const std::string op = lower(op0);
  std::vector<music::MidiClip> out;
  std::ostringstream o;
  for (const auto& c : mf.clips) {
    if (op == "describe") { o << "Track \"" << c.name << "\":\n" << music::describeMidi(c); continue; }
    music::MidiClip n = c;
    if (c.channel == 9 && op != "quantize" && op != "humanize" && op != "velocity" && op != "crop") { out.push_back(c); continue; }  // never transpose drums
    if (op == "quantize") n = music::quantizeMidi(c, a > 0 ? a : 0.25, b > 0 ? b : 1.0);
    else if (op == "transpose") n = music::transposeMidi(c, static_cast<int>(std::lround(a)), false, key);
    else if (op == "humanize") n = music::humanizeMidi(c, a > 0 ? a : 0.03, static_cast<int>(b > 0 ? b : 10), 7);
    else if (op == "legato") n = music::legatoMidi(c);
    else if (op == "velocity") n = music::scaleVelocity(c, a > 0 ? a : 1.0, static_cast<int>(std::lround(b)));
    else if (op == "crop") { if (b <= a) return {false, "Crop needs a start beat and a later end beat."}; n = music::cropMidi(c, a, b); }
    else if (op == "snap_scale") n = music::snapToScale(c, key);
    else return {false, "Operations: describe, quantize, transpose, humanize, legato, velocity, crop, snap_scale."};
    out.push_back(std::move(n));
  }
  if (op == "describe") return {true, o.str().empty() ? "No notes in that file." : o.str()};
  if (out.empty()) return {false, "That MIDI file has no tracks."};
  const double tempo = bpm > 0 ? bpm : mf.bpm;
  if (!music::saveMidiFile(outFile, out, tempo)) return {false, "Could not write " + outFile};
  std::string msg = "Applied " + op + " to " + std::to_string(out.size()) + " track(s); saved " + outFile + ".";
  if (!instrument.empty()) {
    for (auto& c : out) {
      if (c.notes.empty()) continue;
      const bool drums = c.channel == 9;
      const std::string inst = drums ? "drums" : lower(instrument);
      AudioBuffer audio = drums ? music::renderDrums(c, tempo, s_.sampleRate()) : music::renderSynth(c, music::synthPreset(inst), tempo, s_.sampleRate());
      std::string err;
      if (!addStemTrack((c.name.empty() ? "MIDI" : c.name) + " (edited)", audio, -5.f, 0.f, err)) return {false, err};
    }
    msg += " Added tracks that play it with the built-in instruments.";
  }
  return {true, msg};
}

Outcome Producer::analyzeMix() {
  std::vector<AudioBuffer> bufs;
  std::vector<std::string> names;
  for (TrackId t : s_.trackIds()) {
    AudioBuffer a; double r = 0; std::int64_t st = 0;
    if (firstClipAudio(t, a, r, st)) { names.push_back(s_.track(t)->name); bufs.push_back(std::move(a)); }
  }
  if (bufs.size() < 2) return {false, "I need at least two tracks with audio to compare."};
  std::vector<MixTrack> tracks;
  for (std::size_t i = 0; i < bufs.size(); ++i) tracks.push_back({names[i], &bufs[i]});
  lastReport_ = ai::analyzeMix(tracks, s_.sampleRate());
  std::ostringstream o;
  if (lastReport_.fixes.empty()) o << "No fixes to suggest.\n";
  for (std::size_t i = 0; i < lastReport_.fixes.size(); ++i) o << (i + 1) << ". " << lastReport_.fixes[i].why << "\n";
  for (auto& n : lastReport_.notes) o << n << "\n";
  if (!lastReport_.fixes.empty()) o << "Say \"apply the fixes\" (or give numbers) to apply them. Nothing changes until you do.";
  return {true, o.str()};
}

Outcome Producer::applyMixFixes(const std::vector<int>& indices) {
  if (lastReport_.fixes.empty()) return {false, "There are no proposed fixes; analyze the mix first."};
  int applied = 0;
  for (std::size_t i = 0; i < lastReport_.fixes.size(); ++i) {
    if (!indices.empty() && std::find(indices.begin(), indices.end(), static_cast<int>(i) + 1) == indices.end()) continue;
    const MixFix& fx = lastReport_.fixes[i];
    const TrackId t = trackByName(fx.track);
    if (t < 0) continue;
    auto pl = s_.track(t)->source->playlist();
    std::vector<session::Clip> nu;
    for (auto c : pl) {
      auto r = edit::renderClip(c);
      if (!r) { nu.push_back(c); continue; }
      AudioBuffer fixed = applyFix(*r, s_.sampleRate(), fx);
      const std::string f = path(safeName(fx.track) + "_eq" + std::to_string(i) + "_" + std::to_string(c.id) + ".wav");
      io::writeWavFloat(f, fixed, static_cast<unsigned>(s_.sampleRate()), "Wezt corrective EQ");
      c.audio = std::make_shared<AudioBuffer>(std::move(fixed));
      c.file = f; c.offset = 0; c.gainDb = 0; c.fadeIn = c.fadeOut = 0; c.gainEnv.clear();
      nu.push_back(std::move(c));
    }
    s_.track(t)->source->setPlaylist(std::move(nu));
    ++applied;
  }
  lastReport_.fixes.clear();
  return {applied > 0, applied > 0 ? "Applied " + std::to_string(applied) + " fix(es) to copies of the clips; use undo or re-import the originals to go back." : "Nothing matched those numbers."};
}

Outcome Producer::sidechain(const std::string& keyName, const std::string& targetName, const SidechainParams& p) {
  const TrackId k = trackByName(keyName), t = trackByName(targetName);
  if (k < 0) return {false, "I could not find a track called \"" + keyName + "\"."};
  if (t < 0) return {false, "I could not find a track called \"" + targetName + "\"."};
  if (k == t) return {false, "The key and the target must be different tracks."};
  const double sr = s_.sampleRate();
  // key: the key track on the timeline, low-passed so that the kick drives it, not the hats
  std::int64_t endF = 0;
  for (auto& c : s_.track(t)->source->playlist()) endF = std::max(endF, c.start + c.length);
  if (endF <= 0) return {false, "The target track has no clips."};
  AudioBuffer key(2, static_cast<std::size_t>(endF));
  for (auto& c : s_.track(k)->source->playlist()) {
    auto r = edit::renderClip(c);
    if (!r) continue;
    for (std::size_t ch = 0; ch < 2; ++ch)
      for (std::size_t i = 0; i < r->frames; ++i) { const std::size_t pos = static_cast<std::size_t>(c.start) + i; if (pos < key.frames) key.channel(ch)[pos] += r->channel(std::min(ch, r->channels - 1))[i]; }
  }
  dsp::ParametricEq lp;
  lp.setBands({dsp::EqBand{dsp::FilterType::LowPass, 140.f, 0.f, 0.7f, true}});
  lp.prepare(sr, 4096);
  AudioBuffer keyLow(2, key.frames);
  for (std::size_t pos = 0; pos < key.frames; pos += 4096) {
    const std::size_t n = std::min<std::size_t>(4096, key.frames - pos);
    AudioBuffer in(2, n), o(2, n);
    for (std::size_t ch = 0; ch < 2; ++ch) std::copy(key.channel(ch) + pos, key.channel(ch) + pos + n, in.channel(ch));
    lp.process(in, o);
    for (std::size_t ch = 0; ch < 2; ++ch) std::copy(o.channel(ch), o.channel(ch) + n, keyLow.channel(ch) + pos);
  }
  float kpk = 0; for (std::size_t i = 0; i < keyLow.frames; ++i) kpk = std::max(kpk, std::fabs(keyLow.channel(0)[i]));
  if (kpk < 1e-4f) return {false, "The key track is silent, so there is nothing to duck under."};
  SidechainParams sp = p;
  sp.thresholdDb = 20.f * std::log10(kpk) - 9.f;     // relative to the key's own peak, so it works at any level
  std::vector<session::Clip> nu;
  for (auto c : s_.track(t)->source->playlist()) {
    auto r = edit::renderClip(c);
    if (!r) { nu.push_back(c); continue; }
    AudioBuffer seg(2, r->frames);
    for (std::size_t ch = 0; ch < 2; ++ch)
      for (std::size_t i = 0; i < r->frames; ++i) { const std::size_t pos = static_cast<std::size_t>(c.start) + i; seg.channel(ch)[i] = pos < keyLow.frames ? keyLow.channel(ch)[pos] : 0.f; }
    AudioBuffer in2(2, r->frames);
    for (std::size_t ch = 0; ch < 2; ++ch) std::copy(r->channel(std::min(ch, r->channels - 1)), r->channel(std::min(ch, r->channels - 1)) + r->frames, in2.channel(ch));
    AudioBuffer ducked = sidechainCompress(in2, seg, sr, sp);
    const std::string f = path(safeName(s_.track(t)->name) + "_sc_" + std::to_string(c.id) + ".wav");
    io::writeWavFloat(f, ducked, static_cast<unsigned>(sr), "Wezt sidechain");
    c.audio = std::make_shared<AudioBuffer>(std::move(ducked));
    c.file = f; c.offset = 0; c.gainDb = 0; c.fadeIn = c.fadeOut = 0; c.gainEnv.clear();
    nu.push_back(std::move(c));
  }
  s_.track(t)->source->setPlaylist(std::move(nu));
  return {true, "\"" + s_.track(t)->name + "\" now ducks under \"" + s_.track(k)->name + "\" (up to " + std::to_string(static_cast<int>(sp.maxReductionDb)) + " dB, " + std::to_string(static_cast<int>(sp.releaseMs)) + " ms release). The clips were replaced by processed copies."};
}

Outcome Producer::masterSession(const std::string& target, const std::string& outPath) {
  std::int64_t endF = 0;
  for (TrackId t : s_.trackIds()) for (auto& c : s_.track(t)->source->playlist()) endF = std::max(endF, c.start + c.length);
  if (endF <= 0) return {false, "The session is empty."};
  const std::string pre = path("_premaster.wav");
  const auto b = s_.bounce(pre, 0, endF, static_cast<std::int64_t>(2 * s_.sampleRate()));
  if (!b.ok) return {false, "Could not bounce the mix: " + b.error};
  AudioBuffer mix; double sr = 0; std::string err;
  if (!loadAudioFile(pre, mix, sr, &err)) return {false, err};
  const Features f = analyze(mix, sr);
  float lufs = -14.f, ceiling = -1.f;
  bool found = false;
  for (auto& d : deliveryTargets()) if (lower(target) == d.name) { lufs = d.lufs; ceiling = d.truePeakDb; found = true; }
  if (!found && !target.empty()) return {false, "Unknown target \"" + target + "\". Try streaming, apple, cd, club or broadcast."};
  const MasterSettings ms = planMaster(f, {}, lufs, ceiling);
  MasterResult r = applyMaster(mix, sr, ms);
  if (!r.ok) return {false, "Mastering failed."};
  if (!io::writeWavFloat(outPath, r.audio, static_cast<unsigned>(sr), "Wezt local master")) return {false, "Could not write " + outPath};
  char buf[300];
  std::snprintf(buf, sizeof buf, "Mastered to %s: %.1f LUFS (was %.1f), true peak %.1f dBTP (was %.1f). Written to %s.", found ? target.c_str() : "streaming", r.after.lufs, r.before.lufs, r.after.truePeakDb, r.before.truePeakDb, outPath.c_str());
  return {true, buf};
}

}  // namespace wezt::ai
