// Undo/redo watcher, strip silence, the Memory Locations window and the local Assistant panel.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui/app.h"
#include "ai/producer.h"
#include "ai/tutor.h"
#include "agent/semantics.h"
#include <regex>

namespace wezt::ui {
using session::Clip;
using session::TrackId;

namespace {
bool sameClip(const Clip& a, const Clip& b) {
  if (a.id != b.id || a.start != b.start || a.offset != b.offset || a.length != b.length || a.gainDb != b.gainDb || a.fadeIn != b.fadeIn ||
      a.fadeOut != b.fadeOut || a.audio.get() != b.audio.get() || a.muted != b.muted || a.locked != b.locked || a.name != b.name)
    return false;
  const auto &pa = a.gainEnv.points(), &pb = b.gainEnv.points();
  if (pa.size() != pb.size()) return false;
  for (std::size_t i = 0; i < pa.size(); ++i) if (pa[i].frame != pb[i].frame || pa[i].value != pb[i].value) return false;
  return true;
}
bool samePlaylist(const std::vector<Clip>& a, const std::vector<Clip>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) if (!sameClip(a[i], b[i])) return false;
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// undo / redo: compare the playlists with the last settled state once the mouse is up. That catches
// every kind of clip edit (keyboard, menu, dragging) with one hook, and a drag is one step.
void App::watchEdits() {
  if (ImGui::IsMouseDown(0) || ImGui::IsAnyItemActive()) return;
  std::map<TrackId, std::vector<Clip>> cur;
  for (TrackId t : session_->trackIds()) cur[t] = session_->track(t)->source->playlist();
  bool sameSet = snapInit_ && cur.size() == snap_.size();
  if (sameSet) for (auto& [t, v] : cur) if (!snap_.count(t)) { sameSet = false; break; }
  if (!sameSet) { snap_ = std::move(cur); snapInit_ = true; return; }   // tracks added or removed: a new baseline, not an undo step
  std::map<TrackId, std::vector<Clip>> before, after;
  for (auto& [t, v] : cur) if (!samePlaylist(v, snap_[t])) { before[t] = snap_[t]; after[t] = v; }
  if (before.empty()) return;
  auto apply = [this](const std::map<TrackId, std::vector<Clip>>& m) {
    for (auto& [t, v] : m) if (session::Track* tr = session_->track(t)) tr->source->setPlaylist(v);
    snapInit_ = false;   // rebuilt on the next frame
  };
  edit::UndoStep st;
  st.label = before.size() == 1 ? "Edit clips" : "Edit clips on " + std::to_string(before.size()) + " tracks";
  st.undo = [apply, before] { apply(before); };
  st.redo = [apply, after] { apply(after); };
  undo_.record(std::move(st));
  snap_ = std::move(cur);
}

bool App::undoEdit() {
  const std::string label = undo_.undoLabel();
  if (!undo_.undo()) return false;
  setStatus("Undo: " + label);
  return true;
}
bool App::redoEdit() {
  const std::string label = undo_.redoLabel();
  if (!undo_.redo()) return false;
  setStatus("Redo: " + label);
  return true;
}

void App::stripSilenceSelected() {
  session::Track* tr = session_->track(selTrack_);
  if (!tr || selClip_ < 0) { setStatus("Select a clip first"); return; }
  const auto pl = tr->source->playlist();
  const auto it = std::find_if(pl.begin(), pl.end(), [&](const Clip& c) { return c.id == selClip_; });
  if (it == pl.end() || it->locked) { setStatus("Select an unlocked clip first"); return; }
  const auto parts = edit::stripSilence(*it, session_->sampleRate());
  if (parts.empty()) { setStatus("The whole clip is below the silence threshold; nothing changed"); return; }
  const std::size_t before = pl.size();
  const int clipId = it->id;
  session_->removeClip(selTrack_, clipId);
  int first = -1;
  for (const Clip& p : parts) { const int id = session_->addClip(selTrack_, p); if (first < 0) first = id; }
  selClip_ = first;
  setStatus("Strip Silence: " + std::to_string(parts.size()) + (parts.size() == 1 ? " clip" : " clips") + " kept (" + std::to_string(before) + " before on this track)");
}

// ---------------------------------------------------------------------------------------------
// Memory Locations
void App::drawMemoryWindow() {
  ImGui::SetNextWindowSize(ImVec2(620, 300), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Memory Locations", &showMemory_)) { ImGui::End(); return; }
  auto& mem = session_->memory();
  if (ImGui::Button("Add at cursor")) {
    edit::MemoryLocation m;
    m.start = position();
    m.end = selEnd_ > selStart_ && position() == selStart_ ? selEnd_ : m.start;
    m.name = "Marker " + std::to_string(mem.all().size() + 1);
    mem.add(std::move(m));
  }
  ImGui::SameLine();
  if (ImGui::Button("Previous")) { if (auto* m = mem.previous(position())) engine_->transport().locate(m->start); }
  ImGui::SameLine();
  if (ImGui::Button("Next")) { if (auto* m = mem.next(position())) engine_->transport().locate(m->start); }
  if (ImGui::BeginTable("mem", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.f);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.f);
    ImGui::TableSetupColumn("Comment");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.f);
    ImGui::TableHeadersRow();
    int removeNum = 0;
    for (const auto& m : mem.sorted()) {
      edit::MemoryLocation* mp = mem.find(m.number);
      if (!mp) continue;
      ImGui::PushID(m.number);
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%d", m.number);
      ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::InputText("##n", &mp->name);
      ImGui::TableNextColumn();
      {
        const double sec = static_cast<double>(m.start) / session_->sampleRate();
        ImGui::Text("%d:%05.2f", static_cast<int>(sec / 60.0), sec - 60.0 * static_cast<int>(sec / 60.0));
      }
      ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::InputText("##c", &mp->comment);
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Go")) {
        engine_->transport().locate(m.start);
        if (m.isRange()) { selStart_ = m.start; selEnd_ = m.end; }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete")) removeNum = m.number;
      ImGui::PopID();
    }
    if (removeNum) mem.remove(removeNum);
    ImGui::EndTable();
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Assistant
void App::setAssistantDir(const std::string& dir) {
  assistantDir_ = dir;
  assistant_.reset();
}

ai::Assistant& App::assistant() {
  if (!assistant_) {
    std::string dir = assistantDir_;
    if (dir.empty()) {
      const char* home = std::getenv("HOME");
      if (!home) home = std::getenv("USERPROFILE");
      dir = std::string(home ? home : ".") + "/.wezt";
    }
    assistant_ = std::make_unique<ai::Assistant>(dir);
    assistant_->setSessionHandler([this](const std::string& t, std::string& r) { return sessionCommandFromText(t, r); });
  }
  return *assistant_;
}

bool App::sessionCommandFromText(const std::string& text, std::string& reply) {
  std::string low;
  for (char c : text) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!low.empty() && (low.back() == '.' || low.back() == '!' || low.back() == ' ')) low.pop_back();
  struct Row { const char* phrase; Command cmd; const char* said; };
  static const Row rows[] = {
      {"play", Command::PlayStop, "Toggled playback."}, {"stop", Command::PlayStop, "Toggled playback."},
      {"undo", Command::Undo, "Undone."}, {"redo", Command::Redo, "Redone."},
      {"add a track", Command::AddTrack, "Added a track."}, {"add track", Command::AddTrack, "Added a track."},
      {"duplicate track", Command::DuplicateTrack, "Duplicated the selected track."},
      {"save", Command::SaveSession, "Saving."}, {"strip silence", Command::StripSilence, "Stripped silence."},
      {"go to start", Command::ReturnToStart, "At the start."}};
  for (const Row& r : rows) if (low == r.phrase) { execute(r.cmd); reply = status_.empty() ? r.said : status_; return true; }
  return musicCommandFromText(low, reply);
}

agent::PluginAgent& App::pluginAgent() {
  if (!pluginAgent_ || pluginAgentFor_ != session_.get()) {
    pluginAgent_ = std::make_unique<agent::PluginAgent>(*session_, &undo_);
    pluginAgentFor_ = session_.get();
    pluginAgent_->setStructuralRunner([this](const std::function<void()>& work) {
      const bool was = deviceRunning_;
      if (was) device_->stop();
      work();
      if (was) { std::string e; device_->start(&e); }
    });
  }
  return *pluginAgent_;
}

// "make the bass darker", "set the cutoff on lead to 800 Hz", "add a compressor to drums", "what plugins are there"
bool App::pluginCommandFromText(const std::string& low, std::string& reply) {
  auto mentionsTrack = [&] {
    for (auto t : session_->trackIds()) {
      std::string n = session_->track(t)->name;
      for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (n.empty()) continue;
      for (std::size_t at = low.find(n); at != std::string::npos; at = low.find(n, at + 1)) {
        const bool l = at == 0 || !std::isalnum(static_cast<unsigned char>(low[at - 1])), r = at + n.size() >= low.size() || !std::isalnum(static_cast<unsigned char>(low[at + n.size()]));
        if (l && r) return true;
      }
    }
    return false;
  };
  const bool listing = std::regex_search(low, std::regex("\\b(what|which|list|show)\\b.*\\bplug-?ins?\\b"));
  const bool adding = std::regex_search(low, std::regex("\\b(add|insert|put|load)\\b.*\\b(eq|equalizer|compressor|limiter|stereo width|synth|sampler)\\b"));
  const bool setting = std::regex_search(low, std::regex("\\b(set|change|turn)\\b.+\\bto\\b\\s*-?\\d"));
  const bool intent = !agent::parseIntent(low).moves.empty();
  if (!listing && !((adding || setting || intent) && mentionsTrack())) return false;
  reply = pluginAgent().command(low).message;
  return true;
}

bool App::musicCommandFromText(const std::string& low, std::string& reply) {
  auto has = [&](const char* w) { return low.find(w) != std::string::npos; };
  auto firstOf = [&](std::initializer_list<const char*> ws) -> std::string { for (auto* w : ws) if (has(w)) return w; return {}; };
  std::string dir = assistantDir_;
  if (dir.empty()) { const char* h = std::getenv("HOME"); if (!h) h = std::getenv("USERPROFILE"); dir = std::string(h ? h : ".") + "/.wezt"; }
  ai::Producer prod(*session_, dir + "/generated");
  const std::string style = firstOf({"pop", "rock", "lofi", "edm", "jazz", "cinematic", "rnb", "folk", "trap"});
  std::string mood = firstOf({"happy", "sad", "dreamy", "dark", "uplifting", "tense", "chill"});
  if (has("lo-fi")) mood = mood.empty() ? "chill" : mood;

  if (has("how do i") || has("how to") || has("how can i") || has("tutorial") || has("teach me")) {
    static const ai::Tutor tutor; reply = tutor.answer(low); return true;
  }
  if (pluginCommandFromText(low, reply)) return true;
  // "group the drum tracks"
  if (has("group")) {
    struct R { const char* w; ai::Role r; const char* label; };
    static const R roles[] = {{"drum", ai::Role::Drums, "Drums"}, {"vocal", ai::Role::Vocal, "Vocals"}, {"guitar", ai::Role::Guitar, "Guitars"},
                              {"key", ai::Role::Keys, "Keys"}, {"bass", ai::Role::Bass, "Bass"}, {"pad", ai::Role::Pad, "Pads"}};
    for (const R& r : roles) if (has(r.w)) {
      auto ids = prod.tracksByRole(r.r);
      if (r.r == ai::Role::Drums) for (auto t : prod.tracksByRole(ai::Role::Kick)) if (std::find(ids.begin(), ids.end(), t) == ids.end()) ids.push_back(t);
      if (ids.size() < 2) { reply = "I found " + std::to_string(ids.size()) + " " + r.label + " track(s) by name; a group needs at least two. Name the tracks (for example \"Kick\", \"Snare\") so I can tell what they are."; return true; }
      addGroup(r.label, ids);
      reply = "Grouped " + std::to_string(ids.size()) + " tracks as \"" + r.label + "\". Mute, solo and arm now act on all of them.";
      return true;
    }
  }
  // "set up sidechain compression between the kick and bass"
  if (has("sidechain") || has("side-chain") || has("duck")) {
    const bool kickBass = has("kick") || has("drum");
    const std::string key = kickBass ? (has("kick") ? "kick" : "drum") : "";
    const std::string target = has("bass") ? "bass" : has("pad") ? "pad" : has("key") ? "keys" : "";
    if (key.empty() || target.empty()) { reply = "Tell me which track triggers and which ducks, for example \"set up sidechain between the kick and bass\"."; return true; }
    reply = prod.sidechain(key, target).message;
    return true;
  }
  // "find a dreamy pad" (generates one with the built-in synth; it does not search a sound library)
  if ((has("find") || has("add") || has("make") || has("give me")) && !has("clash") && !has("frequenc")) {
    for (const char* kind : {"pad", "bass", "drums", "keys", "melody"}) if (has(kind) && !has("song")) {
      music::Key k; k.mode = music::modeForMood(mood.empty() ? "happy" : mood, music::styleInfo(style.empty() ? "pop" : style).defaultMode);
      reply = prod.addPart(kind, k, style.empty() ? "pop" : style, mood.empty() ? "happy" : mood, 8, 1, 0).message +
              " (I generate it with the built-in synth; I do not search a sound library.)";
      return true;
    }
  }
  if (has("clash") || has("muddy") || has("mud ")) { reply = prod.analyzeMix().message; return true; }
  if ((has("apply") || has("fix")) && (has("clash") || has("mix") || has("eq"))) { reply = prod.applyMixFixes().message; return true; }
  if (has("make a song") || has("build a song") || has("write a song") || has("compose")) {
    music::SongSpec sp; sp.style = style.empty() ? "pop" : style; sp.mood = mood.empty() ? "happy" : mood;
    reply = prod.generateSong(sp).message;
    return true;
  }
  if (has("chord progression") || has("suggest chords") || has("chords for")) {
    music::Key k; k.mode = music::modeForMood(mood.empty() ? "happy" : mood, music::styleInfo(style.empty() ? "pop" : style).defaultMode);
    music::ProgressionParams p; p.key = k; p.style = style.empty() ? "pop" : style; p.mood = mood.empty() ? "happy" : mood; p.bars = 8;
    std::string s = music::keyName(k) + ": "; bool f = true;
    for (auto& c : music::generateProgression(p)) { s += (f ? "" : " | ") + c.name(); f = false; }
    reply = s; return true;
  }
  if (has("master it") || has("one-click master") || has("master the")) {
    reply = prod.masterSession("streaming", dir + "/generated/master.wav").message; return true;
  }
  if (has("split") || has("stems") || has("separate")) {
    const auto t = selectedTrack();
    const session::Track* tr = session_->track(t);
    if (!tr) { reply = "Select the track with the song on it first, then ask me to split it into stems."; return true; }
    reply = prod.separateStems(tr->name).message; return true;
  }
  if (has("voice model") || has("clone my voice") || has("train my voice")) {
    const session::Track* tr = session_->track(selectedTrack());
    if (has("list") || has("which")) {
      std::string l; for (auto& n : prod.voiceModels()) l += (l.empty() ? "" : ", ") + n;
      reply = l.empty() ? "No voice models yet." : "Voice models: " + l; return true;
    }
    if (!tr) { reply = "Select the track with recordings of your voice, then say \"train a voice model from this track, I confirm this is my own voice\"."; return true; }
    reply = prod.trainVoice({tr->name}, "my voice", "", has("i confirm") || has("my own voice")).message; return true;
  }
  if (has("use my voice model") || has("sing it in my voice") || has("in my voice")) {
    const session::Track* tr = session_->track(selectedTrack());
    if (!tr) { reply = "Select the vocal track to re-voice first."; return true; }
    reply = prod.convertVoice(tr->name, "my voice").message; return true;
  }
  if (has("choir")) {
    const session::Track* tr = session_->track(selectedTrack());
    if (!tr) { reply = "Select the track with your recorded vocal first, then say \"make a choir\"."; return true; }
    reply = prod.makeChoir(tr->name, music::Key{}, 4).message + " (I used C major; use the connector or tell me the key for another.)"; return true;
  }
  if (has("my voice") || has("my vocal") || has("fit vocal") || has("sing it")) {
    const session::Track* tr = session_->track(selectedTrack());
    if (!tr) { reply = "Select the track with your recorded vocal first, then say \"fit my vocal to the melody\"."; return true; }
    reply = prod.fitVocal(tr->name).message; return true;
  }
  if (low.rfind("sing ", 0) == 0 || has("sing these lyrics") || has("sing the lyrics")) {
    std::string lyr = low.substr(low.rfind("sing ", 0) == 0 ? 5 : low.find("lyrics") + 6);
    const auto colon = lyr.find(':'); if (colon != std::string::npos) lyr = lyr.substr(colon + 1);
    while (!lyr.empty() && (lyr.front() == ' ' || lyr.front() == '"')) lyr.erase(lyr.begin());
    while (!lyr.empty() && (lyr.back() == ' ' || lyr.back() == '"')) lyr.pop_back();
    if (lyr.empty()) { reply = "Give me the words, for example: sing \"la la hold me tonight\"."; return true; }
    reply = prod.singLyrics(lyr, has("soprano") ? "soprano" : has("tenor") ? "tenor" : has("bass") ? "bass" : "alto", {}, static_cast<double>(tempo_), has("choir") ? 6 : 1).message;
    return true;
  }
  if (has("beat detective") || has("tighten") || (has("quantize") && !has("midi"))) {
    const session::Track* tr = session_->track(selectedTrack());
    if (!tr) { const auto d = prod.tracksByRole(ai::Role::Drums); if (!d.empty()) tr = session_->track(d.front()); }
    if (!tr) { reply = "Select the drum or rhythm track first, then say \"tighten the timing\"."; return true; }
    reply = prod.beatDetective(tr->name, 0, has("eighth") ? 0.5 : has("quarter") ? 1.0 : 0.25, has("loose") || has("half") ? 0.5 : 1.0).message; return true;
  }
  if (has("hum") || has("beatbox") || has("to midi")) {
    const session::Track* tr = session_->track(selectedTrack());
    if (!tr) { reply = "Select the track with the humming or beatboxing first."; return true; }
    reply = prod.audioToMidi(tr->name, "keys", 120, has("beatbox")).message; return true;
  }
  return false;
}

std::string App::assistantSay(const std::string& text) {
  chat_.push_back({true, text});
  const auto r = assistant().say(text);
  chat_.push_back({false, r.text});
  chatScroll_ = true;
  return r.text;
}

void App::drawAssistantWindow() {
  ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Assistant (runs on this computer)", &showAssistant_)) { ImGui::End(); return; }
  ai::Assistant& as = assistant();
  if (chat_.empty()) chat_.push_back({false, "Hi. I measure your music, learn how you mix and master, and can match a reference you give me. Everything stays on this computer. Say \"help\" to see what I can do."});
  const float footer = ImGui::GetFrameHeightWithSpacing() * 2.f + 8.f;
  ImGui::BeginChild("##chat", ImVec2(0, -footer), ImGuiChildFlags_Borders);
  for (auto& [mine, line] : chat_) {
    ImGui::PushStyleColor(ImGuiCol_Text, mine ? ImVec4(0.55f, 0.8f, 1.f, 1.f) : ImVec4(0.9f, 0.9f, 0.9f, 1.f));
    ImGui::TextWrapped("%s %s", mine ? "You:" : "Wezt:", line.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  if (chatScroll_) { ImGui::SetScrollHereY(1.f); chatScroll_ = false; }
  ImGui::EndChild();
  ImGui::SetNextItemWidth(-70.f);
  bool send = ImGui::InputText("##in", &chatInput_, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  send = ImGui::Button("Send", ImVec2(60.f, 0)) || send;
  if (send && !chatInput_.empty()) {
    const std::string t = chatInput_;
    chatInput_.clear();
    assistantSay(t);
    ImGui::SetKeyboardFocusHere(-1);
  }
  // optional local language model, loopback only
  ImGui::SetNextItemWidth(90.f);
  ImGui::InputInt("local model port", &llmPort_, 0, 0);
  ImGui::SameLine();
  if (ImGui::Checkbox("use it for open chat", &llmOn_)) {
    if (llmOn_) as.setModel(std::make_shared<ai::LocalHttpModel>("127.0.0.1", llmPort_));
    else as.setModel(nullptr);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu profiles", as.profiles().list().size());
  ImGui::End();
}

}  // namespace wezt::ui
