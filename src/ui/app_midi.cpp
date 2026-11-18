// MIDI parts, the piano roll window and the score window.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "io/wav.h"
#include "music/midi.h"
#include "music/notation.h"
#include "instruments/wezt_synth.h"
#include "music/synth.h"
#include "session/midi_player.h"
#include "voice/singing.h"
#include "ui/app.h"
#include "ui/style.h"

namespace wezt::ui {
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// parts
int App::newMidiPart(const std::string& name, const std::string& instrument) {
  MidiPart p; p.name = name.empty() ? "MIDI " + std::to_string(midiParts_.size() + 1) : name; p.instrument = instrument.empty() ? "keys" : instrument;
  p.clip.name = p.name;
  if (p.instrument == "drums") p.clip.channel = 9;
  midiParts_.push_back(std::move(p));
  selectMidiPart(static_cast<int>(midiParts_.size()) - 1);
  return selPart_;
}

int App::importMidiFile(const std::string& path, std::string* error) {
  music::MidiFile mf;
  if (!music::loadMidiFile(path, mf, error)) return 0;
  int added = 0;
  for (auto& c : mf.clips) {
    if (c.notes.empty()) continue;
    MidiPart p; p.name = c.name.empty() ? "MIDI " + std::to_string(midiParts_.size() + 1) : c.name; p.instrument = c.channel == 9 ? "drums" : "keys"; p.clip = c;
    midiParts_.push_back(std::move(p));
    ++added;
  }
  if (added) { selectMidiPart(static_cast<int>(midiParts_.size()) - 1); if (mf.bpm >= 30 && mf.bpm <= 300) tempo_ = static_cast<float>(mf.bpm); }
  else if (error && error->empty()) *error = "that file has no notes";
  return added;
}

bool App::exportMidiPart(int i, const std::string& path) {
  if (i < 0 || i >= static_cast<int>(midiParts_.size())) return false;
  return music::saveMidiFile(path, {midiParts_[static_cast<std::size_t>(i)].clip}, tempo_);
}

bool App::removeMidiPart(int i) {
  if (i < 0 || i >= static_cast<int>(midiParts_.size())) return false;
  midiParts_.erase(midiParts_.begin() + i);
  selectMidiPart(std::min(i, static_cast<int>(midiParts_.size()) - 1));
  return true;
}

bool App::editMidiPart(int i, const std::string& label, const std::function<void(edit::PianoRoll&)>& f) {
  if (i < 0 || i >= static_cast<int>(midiParts_.size())) return false;
  if (i != selPart_) selectMidiPart(i);
  const music::MidiClip before = roll_.clip();
  edit::PianoRoll startState = roll_;
  f(roll_);
  const music::MidiClip after = roll_.clip();
  midiParts_[static_cast<std::size_t>(i)].clip = after;
  edit::UndoStep st;
  st.label = label;
  auto apply = [this, i](const music::MidiClip& c) {
    if (i >= static_cast<int>(midiParts_.size())) return;
    midiParts_[static_cast<std::size_t>(i)].clip = c;
    if (selPart_ == i) roll_.load(c);
  };
  st.undo = [apply, before] { apply(before); };
  st.redo = [apply, after] { apply(after); };
  undo_.record(std::move(st));
  return true;
}

bool App::addAudioTrack(const std::string& name, const AudioBuffer& audio) {
  const double sr = session_->sampleRate();
  const char* h = std::getenv("HOME"); if (!h) h = std::getenv("USERPROFILE");
  const fs::path dir = fs::path(h ? h : ".") / ".wezt" / "generated";
  std::error_code ec; fs::create_directories(dir, ec);
  std::string safe = name; for (char& c : safe) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  const std::string file = (dir / (safe + "_" + std::to_string(session_->trackIds().size()) + ".wav")).string();
  if (!io::writeWavFloat(file, audio, static_cast<unsigned>(sr), name)) { setStatus("Could not write " + file); return false; }
  const session::TrackId t = session_->addTrack(name, false, {});
  std::string err;
  if (session_->addClipFromFile(t, file, 0, &err) < 0) { setStatus("Could not add the audio: " + err); return false; }
  selTrack_ = t;
  return true;
}

bool App::renderMidiPart(int i) {
  if (i < 0 || i >= static_cast<int>(midiParts_.size())) return false;
  const MidiPart& p = midiParts_[static_cast<std::size_t>(i)];
  if (p.clip.notes.empty()) { setStatus("That part has no notes"); return false; }
  const double sr = session_->sampleRate();
  const double bpm = std::max(30.f, tempo_);
  AudioBuffer audio = p.instrument == "drums" ? music::renderDrums(p.clip, bpm, sr) : music::renderSynth(p.clip, music::synthPreset(p.instrument), bpm, sr);
  if (!addAudioTrack(p.name, audio)) return false;
  setStatus("Rendered \"" + p.name + "\" to a new audio track");
  return true;
}


// ---------------------------------------------------------------------------------------------
// live MIDI: parts play through the instruments on a track, and keys can be tried by clicking them
namespace {
session::Track* findTrackByName(session::Session& s, const std::string& name) {
  if (name.empty()) return nullptr;
  for (session::TrackId id : s.trackIds()) if (session::Track* t = s.track(id)) if (t->name == name) return t;
  return nullptr;
}
bool hasInstrument(const session::Track& t) {
  for (const auto& in : t.inserts) if (in.instance && in.instance->acceptsMidi()) return true;
  return false;
}
std::uint64_t hashPart(const music::MidiClip& c, const std::string& instrument, double bpm, double sr, const void* where) {
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&](std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
  auto bits = [](double d) { std::uint64_t u; std::memcpy(&u, &d, sizeof u); return u; };
  for (const auto& n : c.notes) { mix(static_cast<std::uint64_t>(n.pitch)); mix(bits(n.start)); mix(bits(n.length)); mix(static_cast<std::uint64_t>(n.velocity)); }
  for (char ch : instrument) mix(static_cast<unsigned char>(ch));
  mix(bits(bpm)); mix(bits(sr)); mix(reinterpret_cast<std::uintptr_t>(where)); mix(static_cast<std::uint64_t>(std::clamp(c.channel, 0, 15)));
  return h;
}
}  // namespace

std::vector<std::string> App::midiTargetTracks() const {
  std::vector<std::string> out;
  for (session::TrackId id : session_->trackIds()) if (const session::Track* t = session_->track(id)) if (hasInstrument(*t)) out.push_back(t->name);
  return out;
}

bool App::setMidiPartTarget(int i, const std::string& trackName) {
  if (i < 0 || i >= static_cast<int>(midiParts_.size())) return false;
  MidiPart& part = midiParts_[static_cast<std::size_t>(i)];
  if (!trackName.empty()) {
    session::Track* t = findTrackByName(*session_, trackName);
    if (!t || !hasInstrument(*t)) { setStatus("\"" + trackName + "\" has no instrument to play the part"); return false; }
  }
  if (!part.liveTrack.empty() && part.liveTrack != trackName)
    if (session::Track* old = findTrackByName(*session_, part.liveTrack)) { old->source->setMidiSequence(nullptr); old->source->auditionAllOff(); }
  if (part.liveTrack != trackName) part.liveOwned = false;
  part.liveTrack = trackName;
  part.liveHash = 0;   // rebuilt on the next frame
  syncLiveMidi();
  setStatus(trackName.empty() ? "\"" + part.name + "\" plays only when rendered" : "\"" + part.name + "\" now plays live on \"" + trackName + "\"");
  return true;
}

bool App::playMidiPartLive(int i) {
  if (i < 0 || i >= static_cast<int>(midiParts_.size())) return false;
  MidiPart& part = midiParts_[static_cast<std::size_t>(i)];
  if (session::Track* t = findTrackByName(*session_, part.liveTrack)) if (hasInstrument(*t)) return true;
  std::string base = part.name + " (live)", name = base;
  for (int k = 2; findTrackByName(*session_, name); ++k) name = base + " " + std::to_string(k);
  auto synth = std::make_unique<instruments::WeztSynth>();
  synth->setPreset(part.instrument);
  session::TrackId id = -1;
  {
    const bool was = deviceRunning_; if (was) device_->stop();
    id = session_->addTrack(name, false, {});
    if (id >= 0) session_->addInsert(id, std::move(synth), instruments::WeztSynth::descriptor(), 0);
    if (was) { std::string e; device_->start(&e); }
  }
  if (id < 0) { setStatus("Could not add a track"); return false; }
  selTrack_ = id;
  part.liveTrack = name; part.liveOwned = true; part.liveHash = 0;
  syncLiveMidi();
  setStatus("\"" + part.name + "\" plays live on the new track \"" + name + "\"");
  return true;
}

void App::syncLiveMidi() {
  const double bpm = std::max(30.f, tempo_), sr = session_->sampleRate();
  for (MidiPart& part : midiParts_) {
    if (part.liveTrack.empty()) continue;
    session::Track* tr = findTrackByName(*session_, part.liveTrack);
    if (!tr) continue;   // the track is gone; the binding waits for a track of that name
    const std::uint64_t h = hashPart(part.clip, part.instrument, bpm, sr, tr->source);
    if (h == part.liveHash) continue;
    part.liveHash = h;
    if (part.liveOwned)
      for (auto& in : tr->inserts)
        if (auto* syn = dynamic_cast<instruments::WeztSynth*>(in.instance)) if (syn->preset() != part.instrument) syn->setPreset(part.instrument);
    tr->source->setMidiSequence(session::buildMidiSequence(part.clip, bpm, sr));
  }
  if (previewKey_ >= 0 && previewOffAt_ > 0 && ImGui::GetTime() >= previewOffAt_) previewNote(previewKey_, false);
  pollHardwareMidi();
}

bool App::openHardwareMidi(std::size_t index) {
  closeHardwareMidi();
  auto dev = midi::openMidiInput(index);
  std::string err;
  if (!dev || !dev->start(&err)) { setStatus(err.empty() ? "No MIDI input found" : err); return false; }
  hwMidi_ = std::move(dev);
  setStatus("MIDI keyboard \"" + hwMidi_->name() + "\" is connected");
  return true;
}

void App::closeHardwareMidi() {
  if (hwMidi_) hwMidi_->stop();
  if (session::Track* tr = findTrackByName(*session_, hwTrack_)) tr->source->auditionAllOff();
  hwMidi_.reset(); hwTrack_.clear();
}

void App::pollHardwareMidi() {
  if (!hwTried_) {   // the first keyboard found is opened once, quietly
    hwTried_ = true;
    if (!midi::listMidiInputs().empty()) openHardwareMidi(0);
  }
  if (!hwMidi_) return;
  midi::MidiMessage m;
  auto target = [&]() -> session::Track* {   // the track the last note went to, else the selected part's live track
    if (!hwTrack_.empty()) return findTrackByName(*session_, hwTrack_);
    return selPart_ >= 0 ? findTrackByName(*session_, midiParts_[static_cast<std::size_t>(selPart_)].liveTrack) : nullptr;
  };
  while (hwMidi_->queue().pop(m)) {
    if (m.type == midi::MidiMessage::NoteOn) {
      if (selPart_ < 0) continue;
      MidiPart& part = midiParts_[static_cast<std::size_t>(selPart_)];
      if (part.liveTrack.empty() && !playMidiPartLive(selPart_)) continue;
      session::Track* tr = findTrackByName(*session_, part.liveTrack);
      if (!tr || !hasInstrument(*tr)) continue;
      hwTrack_ = part.liveTrack;
      tr->source->auditionNote(m.a, m.b, true);
    } else if (m.type == midi::MidiMessage::NoteOff) {
      if (session::Track* tr = findTrackByName(*session_, hwTrack_)) tr->source->auditionNote(m.a, 0, false);
    } else if (m.type == midi::MidiMessage::Control && m.a == 123) {   // all notes off
      if (session::Track* tr = findTrackByName(*session_, hwTrack_)) tr->source->auditionAllOff();
    } else if (m.type == midi::MidiMessage::Control) {                 // sustain pedal (64), mod wheel (1), ...
      if (session::Track* tr = target()) tr->source->auditionController(m.a, m.b);
    } else if (m.type == midi::MidiMessage::PitchBend) {
      if (session::Track* tr = target()) tr->source->auditionPitchBend(m.pitchBendValue() + 8192);
    }
  }
}

bool App::previewNote(int pitch, bool down, int velocity) {
  if (pitch < 0 || pitch > 127) return false;
  if (down) {
    if (selPart_ < 0) return false;
    if (midiParts_[static_cast<std::size_t>(selPart_)].liveTrack.empty() && !playMidiPartLive(selPart_)) return false;
    if (previewKey_ >= 0 && previewKey_ != pitch) previewNote(previewKey_, false);
    const std::string& name = midiParts_[static_cast<std::size_t>(selPart_)].liveTrack;
    session::Track* tr = findTrackByName(*session_, name);
    if (!tr || !hasInstrument(*tr)) { setStatus("\"" + name + "\" has no instrument to preview on"); return false; }
    if (!deviceRunning_) setStatus("The audio device is not running, so there is nothing to hear yet");
    previewKey_ = pitch; previewTrack_ = name; previewOffAt_ = -1;
    return tr->source->auditionNote(pitch, velocity, true);
  }
  const int key = previewKey_ >= 0 ? previewKey_ : pitch;
  previewKey_ = -1; previewOffAt_ = -1;
  session::Track* tr = findTrackByName(*session_, previewTrack_);
  return tr ? tr->source->auditionNote(key, 0, false) : false;
}

bool App::singMidiPart(int i, const std::string& lyrics, const std::string& voiceName, int choir, std::string* message, double humanize, double room, const std::string& character, const std::string& bankName) {
  if (i < 0 || i >= static_cast<int>(midiParts_.size())) { if (message) *message = "No part selected."; return false; }
  voice::SingParams sp;
  if (!voice::parseVoice(voiceName, sp.voice)) { if (message) *message = "Voices: bass, tenor, alto, soprano."; return false; }
  if (!character.empty() && !voice::applyCharacter(character, sp)) { if (message) *message = "Unknown voice character."; return false; }
  voice::VoiceBank bank;
  if (!bankName.empty()) {
    std::string err;
    if (!voice::loadVoiceBank(voiceDir() + "/" + bankName + ".wvb", bank, &err)) { if (message) *message = "Could not load the voice bank \"" + bankName + "\": " + err; setStatus("Could not load the voice bank"); return false; }
    sp.bank = &bank;
  }
  sp.bpm = std::max(30.f, tempo_); sp.sampleRate = session_->sampleRate(); sp.choirVoices = std::clamp(choir, 1, 8);
  sp.humanize = std::clamp(humanize, 0.0, 1.0); sp.room = std::clamp(room, 0.0, 1.0);
  const auto r = voice::sing(lyrics, midiParts_[static_cast<std::size_t>(i)].clip, sp);
  std::string note = r.message;
  if (sp.bank) {
    note += " Vowels from your voice bank (" + std::to_string(r.bankNotes) + " notes).";
    if (r.bankMissing) note += " " + std::to_string(r.bankMissing) + " used a vowel you did not record.";
    if (r.bankConsonants + r.bankConsonantsSynth > 0) note += " Consonants: " + std::to_string(r.bankConsonants) + " yours, " + std::to_string(r.bankConsonantsSynth) + " synthesized.";
    if (r.bankFarNotes) note += " " + std::to_string(r.bankFarNotes) + " are far from the recorded pitch (record that vowel at another pitch).";
  }
  if (message) *message = note;
  if (!r.ok) { setStatus(r.message); return false; }
  if (!addAudioTrack(sp.choirVoices > 1 ? "Choir (sung)" : "Vocal (sung)", r.audio)) return false;
  setStatus(note);
  return true;
}

bool App::saveMidiParts(const std::string& sessionPath) const {
  const std::string path = sessionPath + ".midi";
  std::error_code ec;
  if (midiParts_.empty()) { fs::remove(path, ec); return true; }
  std::vector<music::MidiClip> clips;
  for (const MidiPart& p : midiParts_) { music::MidiClip c = p.clip; c.name = p.name + "|" + p.instrument + "|" + p.liveTrack + "|" + (p.liveOwned ? "1" : "0"); clips.push_back(std::move(c)); }
  return music::saveMidiFile(path, clips, tempo_);
}

bool App::loadMidiParts(const std::string& sessionPath) {
  music::MidiFile mf;
  if (!music::loadMidiFile(sessionPath + ".midi", mf)) return false;
  for (auto& c : mf.clips) {
    MidiPart p;
    std::vector<std::string> f;   // name|instrument[|track|owned]; older files have only the first two
    for (std::size_t a = 0;;) { const auto b = c.name.find('|', a); f.push_back(c.name.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
    p.name = f[0];
    p.instrument = f.size() > 1 && !f[1].empty() ? f[1] : (c.channel == 9 ? "drums" : "keys");
    if (f.size() > 2) p.liveTrack = f[2];
    p.liveOwned = f.size() > 3 && f[3] == "1";
    c.name = p.name; p.clip = c;
    midiParts_.push_back(std::move(p));
  }
  if (mf.bpm >= 30 && mf.bpm <= 300) tempo_ = static_cast<float>(mf.bpm);
  if (!midiParts_.empty()) selectMidiPart(0);
  return true;
}

// ---------------------------------------------------------------------------------------------
// piano roll
namespace {
const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
bool isBlack(int p) { const int k = p % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }
const float kRollGrids[] = {1.f, 0.5f, 0.25f, 0.125f};
const char* kRollGridNames[] = {"1/4", "1/8", "1/16", "1/32"};
const char* kInstruments[] = {"keys", "bass", "pad", "lead", "pluck", "drums"};
}  // namespace

void App::drawPianoRollWindow() {
  ImGui::SetNextWindowSize(ImVec2(900, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Piano Roll", &showPianoRoll_)) { ImGui::End(); return; }
  const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

  // ---- toolbar: part, instrument, tools
  if (ImGui::Button("New part")) newMidiPart("");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150);
  const std::string cur = selPart_ >= 0 ? midiParts_[static_cast<std::size_t>(selPart_)].name : "(no part)";
  if (ImGui::BeginCombo("##part", cur.c_str())) {
    for (int i = 0; i < static_cast<int>(midiParts_.size()); ++i) if (ImGui::Selectable((midiParts_[static_cast<std::size_t>(i)].name + "##p" + std::to_string(i)).c_str(), i == selPart_)) selectMidiPart(i);
    ImGui::EndCombo();
  }
  if (selPart_ >= 0) {
    MidiPart& part = midiParts_[static_cast<std::size_t>(selPart_)];
    ImGui::SameLine(); ImGui::SetNextItemWidth(110); ImGui::InputText("##pname", &part.name);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80);
    if (ImGui::BeginCombo("##inst", part.instrument.c_str())) {
      for (const char* n : kInstruments) if (ImGui::Selectable(n, part.instrument == n)) { part.instrument = n; part.clip.channel = part.instrument == "drums" ? 9 : 0; }
      ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(150);
    {
      const std::string liveCur = part.liveTrack.empty() ? "Play on: (render only)" : "Play on: " + part.liveTrack;
      if (ImGui::BeginCombo("##live", liveCur.c_str())) {
        if (ImGui::Selectable("(render only)", part.liveTrack.empty())) setMidiPartTarget(selPart_, "");
        if (ImGui::Selectable("New Wezt Synth track")) { part.liveTrack.clear(); playMidiPartLive(selPart_); }
        ImGui::Separator();
        for (const std::string& n : midiTargetTracks()) if (ImGui::Selectable((n + "##t").c_str(), n == part.liveTrack)) setMidiPartTarget(selPart_, n);
        ImGui::EndCombo();
      }
    }
    ImGui::SameLine(); if (ImGui::Button("Render to track")) renderMidiPart(selPart_);
    ImGui::SameLine(); if (ImGui::Button("Delete part")) { removeMidiPart(selPart_); }
  }
  ImGui::SameLine(); ImGui::Checkbox("Draw", &rollDraw_);
  ImGui::SameLine(); ImGui::SetNextItemWidth(60);
  if (ImGui::BeginCombo("##g", kRollGridNames[rollGridIdx_ % 4])) { for (int i = 0; i < 4; ++i) if (ImGui::Selectable(kRollGridNames[i], i == rollGridIdx_)) rollGridIdx_ = i; ImGui::EndCombo(); }
  ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderFloat("##zoom", &rollPxPerBeat_, 16.f, 200.f, "zoom %.0f");
  ImGui::SameLine(); ImGui::SetNextItemWidth(200);
  if (ImGui::BeginCombo("##midiin", hwMidi_ ? hardwareMidiName().c_str() : "MIDI keyboard: none")) {   // device chooser
    if (ImGui::Selectable("none", !hwMidi_)) { closeHardwareMidi(); hwTried_ = true; }   // hwTried_: do not reopen it by itself
    const auto devices = midi::listMidiInputs();
    for (std::size_t i = 0; i < devices.size(); ++i)
      if (ImGui::Selectable((devices[i] + "##dev" + std::to_string(i)).c_str(), hwMidi_ && hwMidi_->name() == devices[i])) openHardwareMidi(i);
    if (devices.empty()) ImGui::TextDisabled("no MIDI input devices found");
    ImGui::EndCombo();
  }
  if (selPart_ >= 0) {
    static const char* kVoices[] = {"bass", "tenor", "alto", "soprano"};
    ImGui::SetNextItemWidth(360); ImGui::InputTextWithHint("##lyr", "Lyrics (use - between syllables, e.g. won-der-ful)", &rollLyrics_);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80);
    if (ImGui::BeginCombo("##voice", kVoices[rollVoice_ % 4])) { for (int v = 0; v < 4; ++v) if (ImGui::Selectable(kVoices[v], v == rollVoice_)) rollVoice_ = v; ImGui::EndCombo(); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::SliderInt("##choir", &rollChoir_, 1, 8, "%d voices");
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::SliderFloat("##human", &rollHuman_, 0.f, 1.f, "human %.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much the pitch, timing, loudness and vibrato wander like a real singer. 0 is machine-steady.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::SliderFloat("##room", &rollRoom_, 0.f, 1.f, "room %.2f");
    {
      const auto names = voice::characterNames();
      ImGui::SetNextItemWidth(130);
      if (ImGui::BeginCombo("##char", rollChar_ > 0 ? names[static_cast<std::size_t>(rollChar_ - 1) % names.size()].c_str() : "character: none")) {
        if (ImGui::Selectable("none (just the voice type)", rollChar_ == 0)) rollChar_ = 0;
        for (std::size_t k = 0; k < names.size(); ++k) if (ImGui::Selectable(names[k].c_str(), rollChar_ == static_cast<int>(k) + 1)) rollChar_ = static_cast<int>(k) + 1;
        ImGui::EndCombo();
      }
      ImGui::SameLine(); ImGui::SetNextItemWidth(130);
      if (ImGui::BeginCombo("##bank", rollBank_.empty() ? "vowels: synthesizer" : rollBank_.c_str())) {
        if (ImGui::Selectable("synthesizer", rollBank_.empty())) rollBank_.clear();
        std::error_code ec;
        for (auto& e : fs::directory_iterator(voiceDir(), ec)) if (e.path().extension() == ".wvb") { const std::string n = e.path().stem().string(); if (ImGui::Selectable(n.c_str(), n == rollBank_)) rollBank_ = n; }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sing the vowels from your own recordings (My Voice Bank in the View menu).");
    }
    ImGui::SameLine();
    if (ImGui::Button("Sing it")) {
      const auto names = voice::characterNames();
      std::string msg; singMidiPart(selPart_, rollLyrics_, kVoices[rollVoice_ % 4], rollChoir_, &msg, rollHuman_, rollRoom_, rollChar_ > 0 ? names[static_cast<std::size_t>(rollChar_ - 1) % names.size()] : std::string(), rollBank_);
    }
  }
  if (selPart_ < 0) { ImGui::TextDisabled("Make a part with New part, or ask the assistant for a melody, then import its .mid file."); ImGui::End(); return; }

  const double grid = static_cast<double>(kRollGrids[rollGridIdx_ % 4]);
  const float keysW = 54.f, rulerH = 18.f, velH = 56.f;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float gridW = std::max(100.f, avail.x - keysW), gridH = std::max(80.f, avail.y - rulerH - velH - 6.f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int rows = std::max(1, static_cast<int>(gridH / rollRowH_));
  const double beatsVisible = gridW / rollPxPerBeat_;
  const ImVec2 g0(origin.x + keysW, origin.y + rulerH);   // top-left of the note grid
  auto pitchAtY = [&](float y) { return rollTopPitch_ - static_cast<int>(std::floor((y - g0.y) / rollRowH_)); };
  auto beatAtX = [&](float x) { return static_cast<double>(rollScrollBeat_) + (x - g0.x) / rollPxPerBeat_; };
  auto xOfBeat = [&](double b) { return g0.x + static_cast<float>((b - rollScrollBeat_) * rollPxPerBeat_); };
  auto yOfPitch = [&](int p) { return g0.y + (rollTopPitch_ - p) * rollRowH_; };

  // ---- ruler and grid
  dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + keysW + gridW, origin.y + rulerH), IM_COL32(38, 40, 46, 255));
  dl->AddRectFilled(g0, ImVec2(g0.x + gridW, g0.y + gridH), IM_COL32(26, 27, 31, 255));
  for (int r = 0; r < rows; ++r) {
    const int p = rollTopPitch_ - r;
    if (p < 0 || p > 127) continue;
    const float y = g0.y + r * rollRowH_;
    dl->AddRectFilled(ImVec2(g0.x, y), ImVec2(g0.x + gridW, y + rollRowH_), isBlack(p) ? IM_COL32(32, 33, 38, 255) : IM_COL32(40, 42, 48, 255));
    dl->AddLine(ImVec2(g0.x, y), ImVec2(g0.x + gridW, y), p % 12 == 0 ? IM_COL32(90, 92, 100, 255) : IM_COL32(50, 52, 58, 255));
    dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + keysW, y + rollRowH_), isBlack(p) ? IM_COL32(30, 30, 34, 255) : IM_COL32(225, 225, 228, 255));
    dl->AddRect(ImVec2(origin.x, y), ImVec2(origin.x + keysW, y + rollRowH_), IM_COL32(70, 70, 76, 255));
    if (p % 12 == 0) { char b[8]; std::snprintf(b, sizeof b, "C%d", p / 12 - 1); dl->AddText(ImVec2(origin.x + 4, y), IM_COL32(40, 40, 44, 255), b); }
  }
  const double firstBeat = std::floor(rollScrollBeat_ / grid) * grid;
  for (double b = firstBeat; b < rollScrollBeat_ + beatsVisible + grid; b += grid) {
    const float x = xOfBeat(b);
    if (x < g0.x || x > g0.x + gridW) continue;
    const bool isBeat = std::fabs(b - std::round(b)) < 1e-6, isBar = isBeat && static_cast<long long>(std::llround(b)) % 4 == 0;
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g0.y + gridH), isBar ? IM_COL32(120, 122, 130, 255) : isBeat ? IM_COL32(70, 72, 80, 255) : IM_COL32(48, 50, 56, 255));
    if (isBar) { char b2[16]; std::snprintf(b2, sizeof b2, "%d", static_cast<int>(std::llround(b)) / 4 + 1); dl->AddText(ImVec2(x + 3, origin.y + 2), IM_COL32(200, 200, 205, 255), b2); }
  }

  // ---- notes
  const ImU32 base = style::trackColor(selPart_);
  for (std::size_t i = 0; i < roll_.size(); ++i) {
    const music::Note& n = roll_.note(i);
    if (n.pitch > rollTopPitch_ || n.pitch < rollTopPitch_ - rows) continue;
    const float x0 = xOfBeat(n.start), x1 = std::max(x0 + 3.f, xOfBeat(n.start + n.length)), y0 = yOfPitch(n.pitch);
    if (x1 < g0.x || x0 > g0.x + gridW) continue;
    const float k = 0.55f + 0.45f * static_cast<float>(n.velocity) / 127.f;
    const ImU32 col = roll_.selected(i) ? IM_COL32(255, 210, 90, 255) : style::shade(base, k, 255);
    dl->AddRectFilled(ImVec2(std::max(x0, g0.x), y0 + 1), ImVec2(std::min(x1, g0.x + gridW), y0 + rollRowH_ - 1), col, 2.f);
    dl->AddRect(ImVec2(std::max(x0, g0.x), y0 + 1), ImVec2(std::min(x1, g0.x + gridW), y0 + rollRowH_ - 1), IM_COL32(0, 0, 0, 160), 2.f);
  }
  // playhead (the part is assumed to start at the beginning of the session)
  {
    const double beatPos = static_cast<double>(position()) / (session_->sampleRate() * 60.0 / std::max(30.f, tempo_));
    const float x = xOfBeat(beatPos);
    if (x >= g0.x && x <= g0.x + gridW) dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g0.y + gridH), IM_COL32(255, 90, 90, 220), 1.5f);
  }

  // ---- mouse on the grid
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##roll", ImVec2(gridW, gridH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const bool shift = ImGui::GetIO().KeyShift, ctrl = ImGui::GetIO().KeyCtrl;
  if (hovered && ImGui::GetIO().MouseWheel != 0.f) {
    if (ctrl) rollPxPerBeat_ = std::clamp(rollPxPerBeat_ * (1.f + 0.1f * ImGui::GetIO().MouseWheel), 16.f, 200.f);
    else if (shift) rollScrollBeat_ = std::max(0.f, rollScrollBeat_ - ImGui::GetIO().MouseWheel * 2.f);
    else rollTopPitch_ = std::clamp(rollTopPitch_ + static_cast<int>(ImGui::GetIO().MouseWheel) * 3, rows, 127);
  }
  if (hovered && ImGui::IsMouseClicked(0)) {
    const double beat = beatAtX(mp.x); const int pitch = pitchAtY(mp.y);
    std::size_t idx = 0;
    const auto hit = roll_.hitTest(beat, pitch, 6.0 / rollPxPerBeat_, idx);
    rollStart_ = roll_;
    rollDragBeat_ = beat; rollDragPitch_ = pitch;
    if (hit != edit::PianoRoll::Hit::None) {
      if (!roll_.selected(idx) || shift) roll_.select(idx, shift);
      rollStart_ = roll_;
      rollDrag_ = hit == edit::PianoRoll::Hit::ResizeEnd ? 2 : 1;
      if (!midiParts_[static_cast<std::size_t>(selPart_)].liveTrack.empty() && previewNote(roll_.note(idx).pitch, true, roll_.note(idx).velocity)) previewOffAt_ = ImGui::GetTime() + 0.35;  // click a note to hear it
    } else if (rollDraw_) {
      const int k = roll_.addNote(pitch, beat, grid, 96, grid);
      rollStart_ = roll_;
      rollDrag_ = k >= 0 ? 2 : 0;
      if (k >= 0 && previewNote(pitch, true, 96)) previewOffAt_ = ImGui::GetTime() + 0.35;   // hear the note you just drew
    } else {
      if (!shift) roll_.selectNone();
      rollDrag_ = 3; rollBandX_ = mp.x; rollBandY_ = mp.y;
    }
    if (rollDrag_ == 1 || rollDrag_ == 2) { /* drag handled below */ }
  }
  if (rollDrag_ && ImGui::IsMouseDown(0)) {
    const double beat = beatAtX(mp.x); const int pitch = pitchAtY(mp.y);
    if (rollDrag_ == 1) { roll_ = rollStart_; roll_.moveSelected(beat - rollDragBeat_, pitch - rollDragPitch_, grid); }
    else if (rollDrag_ == 2) { roll_ = rollStart_; roll_.resizeSelected(beat - rollDragBeat_, grid); }
    else if (rollDrag_ == 3) {
      dl->AddRect(ImVec2(rollBandX_, rollBandY_), mp, IM_COL32(255, 255, 255, 200)); dl->AddRectFilled(ImVec2(rollBandX_, rollBandY_), mp, IM_COL32(255, 255, 255, 30));
    }
  }
  if (rollDrag_ && ImGui::IsMouseReleased(0)) {
    if (rollDrag_ == 3) {
      roll_.selectRect(beatAtX(std::min(rollBandX_, mp.x)), beatAtX(std::max(rollBandX_, mp.x)), pitchAtY(std::max(rollBandY_, mp.y)), pitchAtY(std::min(rollBandY_, mp.y)), shift);
    } else {
      const music::MidiClip before = rollStart_.clip(), after = roll_.clip();
      bool changed = before.notes.size() != after.notes.size();
      for (std::size_t i = 0; !changed && i < before.notes.size(); ++i) { const auto &a = before.notes[i], &b = after.notes[i]; changed = a.pitch != b.pitch || a.start != b.start || a.length != b.length || a.velocity != b.velocity; }
      if (changed) {
        const int idx = selPart_;
        midiParts_[static_cast<std::size_t>(idx)].clip = after;
        edit::UndoStep st; st.label = rollDrag_ == 1 ? "Move notes" : rollDraw_ && before.notes.size() != after.notes.size() ? "Add note" : "Resize notes";
        auto apply = [this, idx](const music::MidiClip& c) { if (idx < static_cast<int>(midiParts_.size())) { midiParts_[static_cast<std::size_t>(idx)].clip = c; if (selPart_ == idx) roll_.load(c); } };
        st.undo = [apply, before] { apply(before); }; st.redo = [apply, after] { apply(after); };
        undo_.record(std::move(st));
      }
    }
    rollDrag_ = 0;
  }
  if (hovered && ImGui::IsMouseClicked(1)) {   // right click deletes the note under the pointer
    std::size_t idx = 0;
    if (roll_.hitTest(beatAtX(mp.x), pitchAtY(mp.y), 0, idx) != edit::PianoRoll::Hit::None) editMidiPart(selPart_, "Delete note", [&](edit::PianoRoll& r) { r.select(idx, false); r.deleteSelected(); });
  }

  // ---- piano keys: press and hold a key (slide to others) to hear it on the part's instrument
  {
    ImGui::SetCursorScreenPos(ImVec2(origin.x, g0.y));
    ImGui::InvisibleButton("##keys", ImVec2(keysW, gridH), ImGuiButtonFlags_MouseButtonLeft);
    if (ImGui::IsItemActive()) {
      const int p = std::clamp(pitchAtY(mp.y), 0, 127);
      if (p != previewKey_) { previewNote(p, true); previewOffAt_ = -1; }
    } else if (previewKey_ >= 0 && previewOffAt_ < 0) previewNote(previewKey_, false);
    if (previewKey_ >= 0 && previewKey_ <= rollTopPitch_ && previewKey_ > rollTopPitch_ - rows) {
      const float y = yOfPitch(previewKey_);
      dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + keysW, y + rollRowH_), IM_COL32(255, 190, 70, 255));
      dl->AddRect(ImVec2(origin.x, y), ImVec2(origin.x + keysW, y + rollRowH_), IM_COL32(60, 40, 0, 255));
    }
  }

  // ---- keyboard (only while this window has focus and no text field is active)
  if (focused && !ImGui::IsAnyItemActive()) {
    auto key = [&](ImGuiKey k, bool c = false) { return ImGui::IsKeyPressed(k, true) && ImGui::GetIO().KeyCtrl == c; };
    if (key(ImGuiKey_Delete) || key(ImGuiKey_Backspace)) editMidiPart(selPart_, "Delete notes", [](edit::PianoRoll& r) { r.deleteSelected(); });
    else if (key(ImGuiKey_A, true)) roll_.selectAll();
    else if (key(ImGuiKey_C, true)) roll_.copySelected();
    else if (key(ImGuiKey_V, true)) editMidiPart(selPart_, "Paste notes", [&](edit::PianoRoll& r) { r.paste(std::floor(static_cast<double>(position()) / (session_->sampleRate() * 60.0 / std::max(30.f, tempo_)) / grid) * grid); });
    else if (key(ImGuiKey_D, true)) editMidiPart(selPart_, "Duplicate notes", [](edit::PianoRoll& r) { r.duplicateSelected(); });
    else if (key(ImGuiKey_UpArrow)) editMidiPart(selPart_, "Transpose", [](edit::PianoRoll& r) { r.transposeSelected(1); });
    else if (key(ImGuiKey_DownArrow)) editMidiPart(selPart_, "Transpose", [](edit::PianoRoll& r) { r.transposeSelected(-1); });
    else if (key(ImGuiKey_UpArrow, true)) editMidiPart(selPart_, "Transpose octave", [](edit::PianoRoll& r) { r.transposeSelected(12); });
    else if (key(ImGuiKey_DownArrow, true)) editMidiPart(selPart_, "Transpose octave", [](edit::PianoRoll& r) { r.transposeSelected(-12); });
    else if (key(ImGuiKey_RightArrow)) editMidiPart(selPart_, "Nudge", [&](edit::PianoRoll& r) { r.moveSelected(grid, 0, 0); });
    else if (key(ImGuiKey_LeftArrow)) editMidiPart(selPart_, "Nudge", [&](edit::PianoRoll& r) { r.moveSelected(-grid, 0, 0); });
    else if (key(ImGuiKey_Q)) editMidiPart(selPart_, "Quantize", [&](edit::PianoRoll& r) { r.quantizeSelected(grid); });
    else if (key(ImGuiKey_L)) editMidiPart(selPart_, "Legato", [](edit::PianoRoll& r) { r.legatoSelected(); });
  }

  // ---- velocity lane
  {
    const ImVec2 v0(g0.x, g0.y + gridH + 6.f);
    dl->AddRectFilled(v0, ImVec2(v0.x + gridW, v0.y + velH), IM_COL32(26, 27, 31, 255));
    dl->AddText(ImVec2(origin.x + 4, v0.y + 2), IM_COL32(170, 172, 180, 255), "Velocity");
    for (std::size_t i = 0; i < roll_.size(); ++i) {
      const music::Note& n = roll_.note(i);
      const float x = xOfBeat(n.start);
      if (x < v0.x || x > v0.x + gridW) continue;
      const float h = (velH - 4.f) * static_cast<float>(n.velocity) / 127.f;
      dl->AddRectFilled(ImVec2(x, v0.y + velH - 2.f - h), ImVec2(x + 4.f, v0.y + velH - 2.f), roll_.selected(i) ? IM_COL32(255, 210, 90, 255) : style::shade(base, 1.f, 255));
    }
    ImGui::SetCursorScreenPos(v0);
    ImGui::InvisibleButton("##vel", ImVec2(gridW, velH));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.f) && roll_.selectedCount()) {
      const int v = std::clamp(static_cast<int>(std::lround(127.0 * (v0.y + velH - 2.f - mp.y) / (velH - 4.f))), 1, 127);
      roll_.setVelocity(v);
      midiParts_[static_cast<std::size_t>(selPart_)].clip = roll_.clip();
    }
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// score
namespace {
void drawTrebleClef(ImDrawList* dl, float x, float yBottomLine, float sp, ImU32 col) {
  // a small original sketch of a G clef: a vertical stroke, a loop around the G line and a curl at the foot
  const float yG = yBottomLine - sp, t = 1.4f;
  dl->AddBezierCubic(ImVec2(x, yG), ImVec2(x - 0.9f * sp, yG - 1.0f * sp), ImVec2(x + 1.2f * sp, yG - 2.2f * sp), ImVec2(x + 0.5f * sp, yG - 4.2f * sp), col, t);
  dl->AddBezierCubic(ImVec2(x + 0.5f * sp, yG - 4.2f * sp), ImVec2(x + 0.1f * sp, yG - 5.0f * sp), ImVec2(x - 0.4f * sp, yG - 4.3f * sp), ImVec2(x - 0.1f * sp, yG - 3.0f * sp), col, t);
  dl->AddBezierCubic(ImVec2(x - 0.1f * sp, yG - 3.0f * sp), ImVec2(x + 0.2f * sp, yG - 1.0f * sp), ImVec2(x + 0.7f * sp, yG + 0.6f * sp), ImVec2(x + 0.5f * sp, yG + 1.8f * sp), col, t);
  dl->AddBezierCubic(ImVec2(x + 0.5f * sp, yG + 1.8f * sp), ImVec2(x + 0.4f * sp, yG + 2.6f * sp), ImVec2(x - 0.6f * sp, yG + 2.6f * sp), ImVec2(x - 0.5f * sp, yG + 1.9f * sp), col, t);
  dl->AddCircleFilled(ImVec2(x - 0.5f * sp, yG + 1.9f * sp), 0.22f * sp, col);
}
void drawBassClef(ImDrawList* dl, float x, float yBottomLine, float sp, ImU32 col) {
  const float yF = yBottomLine - 3.f * sp;   // the F line
  dl->AddCircleFilled(ImVec2(x, yF), 0.42f * sp, col);
  dl->AddBezierCubic(ImVec2(x, yF - 0.4f * sp), ImVec2(x + 1.3f * sp, yF - 0.7f * sp), ImVec2(x + 2.0f * sp, yF + 0.8f * sp), ImVec2(x - 0.2f * sp, yF + 3.0f * sp), col, 1.6f);
  dl->AddCircleFilled(ImVec2(x + 2.1f * sp, yF - 0.5f * sp), 0.2f * sp, col);
  dl->AddCircleFilled(ImVec2(x + 2.1f * sp, yF + 0.5f * sp), 0.2f * sp, col);
}
void drawRest(ImDrawList* dl, float x, float yTopLine, float sp, int value, ImU32 col) {
  const float mid = yTopLine + 2.f * sp;
  if (value == 0) dl->AddRectFilled(ImVec2(x - 0.6f * sp, yTopLine + sp), ImVec2(x + 0.6f * sp, yTopLine + 1.5f * sp), col);              // hangs from the 4th line
  else if (value == 1) dl->AddRectFilled(ImVec2(x - 0.6f * sp, mid - 0.5f * sp), ImVec2(x + 0.6f * sp, mid), col);                          // sits on the middle line
  else if (value == 2) { dl->AddBezierCubic(ImVec2(x - 0.3f * sp, mid - 1.4f * sp), ImVec2(x + 0.6f * sp, mid - 0.6f * sp), ImVec2(x - 0.5f * sp, mid), ImVec2(x + 0.3f * sp, mid + 1.2f * sp), col, 2.f); }
  else { dl->AddLine(ImVec2(x + 0.4f * sp, mid - 0.8f * sp), ImVec2(x - 0.4f * sp, mid + 1.0f * sp), col, 1.5f); dl->AddCircleFilled(ImVec2(x - 0.2f * sp, mid - 0.6f * sp), 0.3f * sp, col); }
}
}  // namespace

void App::drawScoreWindow() {
  ImGui::SetNextWindowSize(ImVec2(900, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Score", &showScore_)) { ImGui::End(); return; }
  if (selPart_ < 0) { ImGui::TextDisabled("Select a MIDI part (Piano Roll window) to see it as notation."); ImGui::End(); return; }
  static const char* kRoots[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
  static const char* kModes[6] = {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian"};
  ImGui::SetNextItemWidth(60); if (ImGui::BeginCombo("##kr", kRoots[scoreKeyRoot_ % 12])) { for (int i = 0; i < 12; ++i) if (ImGui::Selectable(kRoots[i], i == scoreKeyRoot_)) scoreKeyRoot_ = i; ImGui::EndCombo(); }
  ImGui::SameLine(); ImGui::SetNextItemWidth(100); if (ImGui::BeginCombo("##km", kModes[scoreKeyMode_ % 6])) { for (int i = 0; i < 6; ++i) if (ImGui::Selectable(kModes[i], i == scoreKeyMode_)) scoreKeyMode_ = i; ImGui::EndCombo(); }
  ImGui::SameLine(); ImGui::SetNextItemWidth(70); if (ImGui::BeginCombo("##clef", scoreClef_ < 0 ? "auto" : scoreClef_ == 0 ? "treble" : "bass")) { if (ImGui::Selectable("auto")) scoreClef_ = -1; if (ImGui::Selectable("treble")) scoreClef_ = 0; if (ImGui::Selectable("bass")) scoreClef_ = 1; ImGui::EndCombo(); }
  ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::SliderInt("beats/bar", &scoreBeatsPerBar_, 2, 7);
  ImGui::SameLine(); ImGui::TextDisabled("%s", midiParts_[static_cast<std::size_t>(selPart_)].name.c_str());

  music::Key key; key.root = scoreKeyRoot_ % 12; key.mode = static_cast<music::Mode>(scoreKeyMode_ % 6);
  const music::Score sc = music::engrave(roll_.clip(), key, scoreBeatsPerBar_, scoreClef_);

  ImGui::BeginChild("##scorearea", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const float sp = 9.f;                               // distance between staff lines
  const ImU32 ink = IM_COL32(235, 235, 238, 255);
  const float beatW = 34.f, barPad = 14.f, headerW = 62.f + std::abs(sc.fifths) * 9.f;
  const float barW = beatW * static_cast<float>(sc.beatsPerBar) + 2 * barPad;
  float x = headerW, y = 46.f;
  const int botStep = music::bottomLineStep(sc.clef);
  float sysTop = y;
  auto stepY = [&](float top, int step) { return top + 4.f * sp - (step - botStep) * 0.5f * sp; };   // y of a note position
  auto beginSystem = [&](float top) {
    for (int l = 0; l < 5; ++l) dl->AddLine(ImVec2(p0.x + 8.f, p0.y + top + l * sp), ImVec2(p0.x + 8.f + std::max(width - 16.f, headerW + barW), p0.y + top + l * sp), IM_COL32(150, 150, 156, 255), 1.f);
    if (sc.clef == music::Clef::Treble) drawTrebleClef(dl, p0.x + 24.f, p0.y + top + 4.f * sp, sp, ink); else drawBassClef(dl, p0.x + 22.f, p0.y + top + 4.f * sp, sp, ink);
    // key signature
    static const int sharpSteps[7] = {5, 2, 6, 3, 0, 4, 1}; // F C G D A E B  (letters)
    static const int flatSteps[7] = {6, 2, 5, 1, 4, 0, 3};
    for (int i = 0; i < std::abs(sc.fifths); ++i) {
      const int letter = sc.fifths > 0 ? sharpSteps[i] : flatSteps[i];
      int step = botStep + ((letter - botStep % 7) % 7 + 7) % 7;
      if (sc.fifths > 0 && step > botStep + 6) step -= 7;
      const float yy = p0.y + stepY(top, step), xx = p0.x + 50.f + i * 9.f;
      if (sc.fifths > 0) { dl->AddLine(ImVec2(xx, yy - 5), ImVec2(xx, yy + 5), ink); dl->AddLine(ImVec2(xx + 3, yy - 5), ImVec2(xx + 3, yy + 5), ink); dl->AddLine(ImVec2(xx - 2, yy - 1.5f), ImVec2(xx + 5, yy - 1.5f), ink); dl->AddLine(ImVec2(xx - 2, yy + 1.5f), ImVec2(xx + 5, yy + 1.5f), ink); }
      else { dl->AddLine(ImVec2(xx, yy - 6), ImVec2(xx, yy + 3), ink); dl->AddBezierCubic(ImVec2(xx, yy), ImVec2(xx + 5, yy - 2), ImVec2(xx + 5, yy + 4), ImVec2(xx, yy + 3), ink, 1.2f); }
    }
  };
  beginSystem(sysTop);
  for (const music::ScoreBar& bar : sc.bars) {
    if (x + barW > width && x > headerW) { x = headerW; y += 8.f * sp + 24.f; sysTop = y; beginSystem(sysTop); }
    const float bx = p0.x + x;
    dl->AddLine(ImVec2(bx + barW, p0.y + sysTop), ImVec2(bx + barW, p0.y + sysTop + 4 * sp), ink);
    if (bar.number % 4 == 1 || bar.number == 1) { char b[8]; std::snprintf(b, sizeof b, "%d", bar.number); dl->AddText(ImVec2(bx + 2, p0.y + sysTop - 16.f), IM_COL32(140, 142, 150, 255), b); }
    for (const music::ScoreEvent& e : bar.events) {
      const float ex = bx + barPad + static_cast<float>(e.beat) * beatW + 6.f;
      if (e.rest) { drawRest(dl, ex, p0.y + sysTop, sp, e.value, ink); continue; }
      int lo = 1 << 20, hi = -(1 << 20);
      for (const auto& n : e.notes) { lo = std::min(lo, n.step); hi = std::max(hi, n.step); }
      for (const auto& n : e.notes) {
        const float ny = p0.y + stepY(sysTop, n.step);
        // ledger lines
        for (int s = botStep - 2; s >= n.step; s -= 2) dl->AddLine(ImVec2(ex - 8, p0.y + stepY(sysTop, s)), ImVec2(ex + 8, p0.y + stepY(sysTop, s)), ink);
        for (int s = botStep + 10; s <= n.step; s += 2) dl->AddLine(ImVec2(ex - 8, p0.y + stepY(sysTop, s)), ImVec2(ex + 8, p0.y + stepY(sysTop, s)), ink);
        const bool hollow = e.value <= 1;
        if (hollow) dl->AddEllipse(ImVec2(ex, ny), ImVec2(0.62f * sp, 0.46f * sp), ink, -0.35f, 0, 1.6f);
        else dl->AddEllipseFilled(ImVec2(ex, ny), ImVec2(0.62f * sp, 0.46f * sp), ink, -0.35f);
        for (int d = 0; d < e.dots; ++d) dl->AddCircleFilled(ImVec2(ex + 0.95f * sp + d * 4.f, ny - ((n.step - botStep) % 2 == 0 ? 0.5f * sp : 0.f)), 1.6f, ink);
        if (n.accidental != -2) {
          const float ax = ex - 1.3f * sp;
          if (n.accidental == 1) { dl->AddLine(ImVec2(ax, ny - 6), ImVec2(ax, ny + 6), ink); dl->AddLine(ImVec2(ax + 3, ny - 6), ImVec2(ax + 3, ny + 6), ink); dl->AddLine(ImVec2(ax - 2, ny - 2), ImVec2(ax + 5, ny - 2), ink, 1.4f); dl->AddLine(ImVec2(ax - 2, ny + 2), ImVec2(ax + 5, ny + 2), ink, 1.4f); }
          else if (n.accidental == -1) { dl->AddLine(ImVec2(ax, ny - 7), ImVec2(ax, ny + 3), ink); dl->AddBezierCubic(ImVec2(ax, ny), ImVec2(ax + 5, ny - 2), ImVec2(ax + 5, ny + 4), ImVec2(ax, ny + 3), ink, 1.3f); }
          else { dl->AddLine(ImVec2(ax, ny - 5), ImVec2(ax, ny + 2), ink); dl->AddLine(ImVec2(ax + 4, ny - 2), ImVec2(ax + 4, ny + 5), ink); dl->AddLine(ImVec2(ax, ny - 1), ImVec2(ax + 4, ny - 2), ink); dl->AddLine(ImVec2(ax, ny + 2), ImVec2(ax + 4, ny + 1), ink); }
        }
      }
      if (e.value >= 1) {   // stem and flags
        const int mid = botStep + 4;
        const bool up = (lo + hi) / 2 < mid;
        const float sx = up ? ex + 0.6f * sp : ex - 0.6f * sp;
        const float yA = p0.y + stepY(sysTop, up ? lo : hi), yB = p0.y + stepY(sysTop, up ? hi : lo);
        const float yEnd = up ? yB - 3.4f * sp : yB + 3.4f * sp;
        dl->AddLine(ImVec2(sx, yA), ImVec2(sx, yEnd), ink, 1.3f);
        for (int f = 0; f < std::max(0, e.value - 2); ++f) {
          const float fy = yEnd + (up ? 1 : -1) * f * 0.8f * sp * (up ? 1 : 1);
          dl->AddBezierCubic(ImVec2(sx, fy), ImVec2(sx + 0.9f * sp, fy + (up ? 0.8f : -0.8f) * sp), ImVec2(sx + 1.2f * sp, fy + (up ? 1.6f : -1.6f) * sp), ImVec2(sx + 0.5f * sp, fy + (up ? 2.6f : -2.6f) * sp), ink, 1.4f);
        }
      }
      if (e.tieToNext && !e.notes.empty()) {
        const float ty = p0.y + stepY(sysTop, e.notes.back().step) + 0.7f * sp;
        dl->AddBezierQuadratic(ImVec2(ex, ty), ImVec2(ex + 0.5f * beatW * static_cast<float>(e.length), ty + 0.9f * sp), ImVec2(ex + beatW * static_cast<float>(e.length), ty), ink, 1.2f);
      }
    }
    x += barW;
  }
  ImGui::Dummy(ImVec2(width, y + 8.f * sp + 30.f));
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace wezt::ui
