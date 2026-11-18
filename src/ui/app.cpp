#include "instruments/builtin.h"
#include "ui/app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "io/wav.h"

namespace wezt::ui {
namespace fs = std::filesystem;
using session::Clip;
using session::TrackId;

namespace {
ImGuiKey toImGui(Key k) {
  const int v = static_cast<int>(k), a = static_cast<int>(Key::A), d0 = static_cast<int>(Key::D0), f1 = static_cast<int>(Key::F1), kp0 = static_cast<int>(Key::Kp0);
  if (k >= Key::A && k <= Key::Z) return static_cast<ImGuiKey>(ImGuiKey_A + (v - a));
  if (k >= Key::D0 && k <= Key::D9) return static_cast<ImGuiKey>(ImGuiKey_0 + (v - d0));
  if (k >= Key::F1 && k <= Key::F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (v - f1));
  if (k >= Key::Kp0 && k <= Key::Kp9) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (v - kp0));
  switch (k) {
    case Key::KpEnter: return ImGuiKey_KeypadEnter;
    case Key::Space: return ImGuiKey_Space;
    case Key::Enter: return ImGuiKey_Enter;
    case Key::Escape: return ImGuiKey_Escape;
    case Key::Tab: return ImGuiKey_Tab;
    case Key::Backquote: return ImGuiKey_GraveAccent;
    case Key::Equals: return ImGuiKey_Equal;
    case Key::LeftBracket: return ImGuiKey_LeftBracket;
    case Key::RightBracket: return ImGuiKey_RightBracket;
    case Key::Backspace: return ImGuiKey_Backspace;
    case Key::Delete: return ImGuiKey_Delete;
    case Key::Left: return ImGuiKey_LeftArrow;
    case Key::Right: return ImGuiKey_RightArrow;
    case Key::Up: return ImGuiKey_UpArrow;
    case Key::Down: return ImGuiKey_DownArrow;
    case Key::Home: return ImGuiKey_Home;
    case Key::End: return ImGuiKey_End;
    default: return ImGuiKey_None;
  }
}
}  // namespace

App::App() {
  session_ = std::make_unique<session::Session>(48000.0, 2, 512);
  engine_ = std::make_unique<Engine>(session_->graph(), 512, 2, 48000.0, 60);
  session_->attach(engine_->context());
  keymap_ = Keymap::protoolsDefaults();
  selTrack_ = session_->addTrack("Audio 1", true);
  panner_ = std::make_unique<immersive::ObjectRenderer>(layout_, immersive::SpeakerLayout{}, 0);
}

App::~App() { stopAudio(); }

bool App::startAudio(std::string* message) {
  device_ = audio::createPlatformDevice();
  if (!device_) {
    if (message) *message = "No audio backend in this build (WASAPI on Windows, CoreAudio on macOS): running without sound.";
    return false;
  }
  audio::AudioConfig cfg;
  cfg.sampleRate = 48000; cfg.bufferFrames = 256; cfg.inChannels = 2; cfg.outChannels = 2;
  std::string err;
  if (!device_->open(cfg, engine_.get(), &err) || !device_->start(&err)) {
    if (message) *message = "Audio device: " + err;
    device_.reset();
    return false;
  }
  deviceRunning_ = true;
  if (message) *message = device_->backendName() + " running" + (device_->notice().empty() ? "" : " (" + device_->notice() + ")");
  return true;
}

// A stream whose device vanished stops calling us. Tell the user, then keep trying (every 2 s) to open the system default
// device again, so unplugging and re-plugging headphones, or waking the computer, just works.
void App::watchAudioDevice() {
  if (!device_) return;
  const double now = ImGui::GetTime();
  if (deviceRunning_ && device_->lost()) {
    device_->stop();
    deviceRunning_ = false;
    deviceLost_ = true;
    deviceRetryAt_ = now + 1.0;
    engine_->transport().stop();
    setStatus("Audio device lost (unplugged or taken over by another program). Looking for it again...");
  }
  if (deviceLost_ && now >= deviceRetryAt_) {
    deviceRetryAt_ = now + 2.0;
    audio::AudioConfig cfg;
    cfg.sampleRate = static_cast<unsigned>(session_->sampleRate()); cfg.bufferFrames = 256; cfg.inChannels = 2; cfg.outChannels = 2;
    std::string err;
    if (device_->open(cfg, engine_.get(), &err) && device_->start(&err)) {
      deviceRunning_ = true;
      deviceLost_ = false;
      setStatus("Audio device is back: " + device_->backendName() + " running");
    }
  }
}

void App::stopAudio() {
  if (device_ && deviceRunning_) device_->stop();
  deviceRunning_ = false;
}

// ------------------------------------------------------------------------------------------
std::int64_t App::position() const { return engine_->transport().position(); }
std::int64_t App::gridFrames() const { return static_cast<std::int64_t>(session_->sampleRate() * 60.0 / static_cast<double>(std::max(20.f, tempo_)) * static_cast<double>(kGridBeats[gridIdx_ % 5])); }
std::int64_t App::nudgeFrames() const { return std::max<std::int64_t>(1, static_cast<std::int64_t>(session_->sampleRate() * kNudgeMs[nudgeIdx_ % 5] / 1000.0)); }
std::int64_t App::snap(std::int64_t f) const {
  if (mode_ != EditMode::Grid) return std::max<std::int64_t>(0, f);
  const std::int64_t g = std::max<std::int64_t>(1, gridFrames());
  return std::max<std::int64_t>(0, (f + g / 2) / g * g);
}
std::int64_t App::endOfSession() const {
  std::int64_t e = 0;
  for (auto id : session_->trackIds()) for (const auto& c : session_->track(id)->source->playlist()) e = std::max(e, c.start + c.length);
  return e;
}

bool App::animating() const {
  if (engine_->transport().playing() || dragging_) return true;
  for (const auto& kv : meters_) if (kv.second > 0.002f) return true;
  return false;
}

float App::meterLevel(const void* key, float peak) {
  float& v = meters_[key];
  v = std::max(peak, v * 0.90f);
  if (v < 0.0005f) v = 0.f;
  return v;
}

instruments::Sampler* App::samplerOn(TrackId t) {
  session::Track* tr = session_->track(t);
  if (!tr) return nullptr;
  for (auto& in : tr->inserts) if (auto* s = dynamic_cast<instruments::Sampler*>(in.instance)) return s;
  return nullptr;
}

// The session's structure may only change while audio is stopped, so edits pause the device briefly.
#define WEZT_PAUSED(body) do { const bool _was = deviceRunning_; if (_was) device_->stop(); body; std::string _e; if (_was) device_->start(&_e); } while (0)

void App::addTrackQuick(bool mono) {
  const std::string name = (mono ? "Audio " : "Stereo ") + std::to_string(session_->trackCount() + 1);
  TrackId t = -1;
  WEZT_PAUSED(t = session_->addTrack(name, mono, mono ? std::vector<unsigned>{0} : std::vector<unsigned>{0, 1}));
  selTrack_ = t;
  setStatus("Added track \"" + name + "\"");
}

bool App::isImplemented(Command c) const {
  switch (c) {
    case Command::PlayStop: case Command::Record: case Command::LoopPlayback: case Command::ReturnToStart: case Command::GoToEnd:
    case Command::Rewind: case Command::FastForward:
    case Command::ModeShuffle: case Command::ModeSlip: case Command::ModeSpot: case Command::ModeGrid:
    case Command::ToolZoomer: case Command::ToolTrimmer: case Command::ToolSelector: case Command::ToolGrabber: case Command::ToolSmart:
    case Command::CycleEditModes: case Command::CycleEditTools:
    case Command::Clear: case Command::DeleteSelection: case Command::Duplicate: case Command::SeparateClip: case Command::CreateFades:
    case Command::ToggleMixEdit: case Command::ShowTransport: case Command::ZoomIn: case Command::ZoomOut: case Command::ZoomSession:
    case Command::ZoomSelection: case Command::FillWindowWithSelection: case Command::ShowAllClips:
    case Command::NewTrack: case Command::AddTrack: case Command::RemoveTrack: case Command::DuplicateTrack:
    case Command::NewSession: case Command::OpenSession: case Command::SaveSession: case Command::SaveSessionAs: case Command::BounceMix:
    case Command::ImportAudio: case Command::OpenSampler: case Command::OpenImmersivePanner: case Command::SelectAll:
    case Command::Undo: case Command::Redo: case Command::StripSilence: case Command::ShowMemoryLocations:
      return true;
    default: return false;
  }
}

void App::execute(Command c) {
  auto& tr = engine_->transport();
  auto cycleTool = [&] {
    static const Tool order[] = {Tool::Trimmer, Tool::Selector, Tool::Grabber, Tool::Smart};
    int i = 0;
    for (int k = 0; k < 4; ++k) if (order[k] == tool_) i = k + 1;
    tool_ = order[i % 4];
  };
  if (!isImplemented(c)) { setStatus(std::string(commandName(c)) + " is not available yet"); return; }
  switch (c) {
    case Command::PlayStop: if (tr.playing()) tr.stop(); else tr.play(); break;
    case Command::Record: {
      bool any = false;
      for (auto id : session_->trackIds()) any = any || session_->track(id)->source->armed();
      if (!any) { setStatus("Arm a track (R) before recording"); break; }
      if (tr.recording()) tr.stop(); else tr.record();
      break;
    }
    case Command::LoopPlayback:
      if (tr.looping()) tr.clearLoop();
      else if (selEnd_ > selStart_) tr.setLoop(selStart_, selEnd_);
      else setStatus("Select a range on the timeline to loop it");
      break;
    case Command::ReturnToStart: tr.locate(0); scrollFrames_ = 0; break;
    case Command::GoToEnd: tr.locate(endOfSession()); break;
    case Command::Rewind: tr.locate(std::max<std::int64_t>(0, position() - static_cast<std::int64_t>(session_->sampleRate() * 2))); break;
    case Command::FastForward: tr.locate(position() + static_cast<std::int64_t>(session_->sampleRate() * 2)); break;
    case Command::ModeShuffle: mode_ = EditMode::Shuffle; break;
    case Command::ModeSlip: mode_ = EditMode::Slip; break;
    case Command::ModeSpot: mode_ = EditMode::Spot; break;
    case Command::ModeGrid: mode_ = EditMode::Grid; break;
    case Command::CycleEditModes: mode_ = static_cast<EditMode>((static_cast<int>(mode_) + 1) % 4); break;
    case Command::ToolZoomer: tool_ = Tool::Zoomer; break;
    case Command::ToolTrimmer: tool_ = Tool::Trimmer; break;
    case Command::ToolSelector: tool_ = Tool::Selector; break;
    case Command::ToolGrabber: tool_ = Tool::Grabber; break;
    case Command::ToolSmart: tool_ = Tool::Smart; break;
    case Command::CycleEditTools: cycleTool(); break;
    case Command::Clear: case Command::DeleteSelection: deleteSelectedClip(); break;
    case Command::Duplicate: duplicateSelectedClip(); break;
    case Command::SeparateClip: splitSelectedClip(); break;
    case Command::Undo: if (!undoEdit()) setStatus("Nothing to undo"); break;
    case Command::Redo: if (!redoEdit()) setStatus("Nothing to redo"); break;
    case Command::StripSilence: stripSilenceSelected(); break;
    case Command::ShowMemoryLocations: showMemory_ = !showMemory_; break;
    case Command::CreateFades: fadeSelectedClip(); break;
    case Command::SelectAll: selStart_ = 0; selEnd_ = endOfSession(); break;
    case Command::ToggleMixEdit: view_ = view_ == MainView::Edit ? MainView::Mix : MainView::Edit; break;
    case Command::ShowTransport: showTransportBar_ = !showTransportBar_; break;
    case Command::ZoomIn: framesPerPixel_ = std::max(2.f, framesPerPixel_ * 0.5f); break;
    case Command::ZoomOut: framesPerPixel_ = std::min(200000.f, framesPerPixel_ * 2.f); break;
    case Command::ZoomSession: case Command::ShowAllClips: zoomTo(0, std::max<std::int64_t>(endOfSession(), 48000)); break;
    case Command::ZoomSelection: case Command::FillWindowWithSelection:
      if (selEnd_ > selStart_) zoomTo(selStart_, selEnd_); else setStatus("No selection to zoom to");
      break;
    case Command::NewTrack: showNewTrack_ = true; break;
    case Command::AddTrack: addTrackQuick(true); break;
    case Command::DuplicateTrack: {
      TrackId n = -1;
      WEZT_PAUSED(n = session_->duplicateTrack(selTrack_, [](const plugins::PluginDescriptor& d) -> std::unique_ptr<plugins::IPluginInstance> { return instruments::makeBuiltin(d); }));
      if (n >= 0) { selTrack_ = n; setStatus("Duplicated track"); } else setStatus("Nothing to duplicate");
      break;
    }
    case Command::RemoveTrack: {
      if (session_->trackCount() <= 1) { setStatus("A session needs at least one track"); break; }
      const TrackId gone = selTrack_;
      WEZT_PAUSED(session_->removeTrack(gone));
      objects_.erase(gone);
      const auto ids = session_->trackIds();
      selTrack_ = ids.empty() ? -1 : ids.back();
      setStatus("Removed track");
      break;
    }
    case Command::NewSession: openFileAction("new"); break;
    case Command::OpenSession: openFileAction("open"); break;
    case Command::SaveSession: case Command::SaveSessionAs: openFileAction("save"); break;
    case Command::BounceMix: openFileAction("bounce"); break;
    case Command::ImportAudio: openFileAction("import"); break;
    case Command::OpenSampler: showSampler_ = true; break;
    case Command::OpenImmersivePanner: showImmersive_ = true; break;
    default: break;
  }
}

void App::zoomTo(std::int64_t a, std::int64_t b) {
  const float width = std::max(200.f, ImGui::GetIO().DisplaySize.x - 260.f);
  framesPerPixel_ = std::clamp(static_cast<float>(std::max<std::int64_t>(b - a, 100)) / width, 2.f, 200000.f);
  scrollFrames_ = static_cast<double>(a);
}

// ---- clip editing -----------------------------------------------------------------------
static Clip* findClip(std::vector<Clip>& list, int id) { for (auto& c : list) if (c.id == id) return &c; return nullptr; }

void App::deleteSelectedClip() {
  session::Track* t = session_->track(selTrack_);
  if (!t || selClip_ < 0) { setStatus("Select a clip first"); return; }
  if (session_->removeClip(selTrack_, selClip_)) { selClip_ = -1; setStatus("Clip deleted"); }
}

void App::splitSelectedClip() {
  session::Track* t = session_->track(selTrack_);
  if (!t) return;
  auto list = t->source->playlist();
  const std::int64_t at = (selEnd_ > selStart_ || selStart_ > 0) ? selStart_ : position();   // insertion point or start of the range
  Clip* c = nullptr;
  if (selClip_ >= 0) c = findClip(list, selClip_);
  if (!c) for (auto& k : list) if (at > k.start && at < k.start + k.length) { c = &k; break; }
  if (!c || at <= c->start || at >= c->start + c->length) { setStatus("Place the cursor inside a clip to separate it"); return; }
  Clip right = *c;
  right.id = t->nextClipId++;
  right.start = at;
  right.offset = c->offset + (at - c->start);
  right.length = c->start + c->length - at;
  right.fadeIn = 0;
  c->length = at - c->start;
  c->fadeOut = 0;
  list.push_back(right);
  t->source->setPlaylist(std::move(list));
  selClip_ = right.id;
  setStatus("Clip separated");
}

void App::duplicateSelectedClip() {
  session::Track* t = session_->track(selTrack_);
  if (!t || selClip_ < 0) { setStatus("Select a clip first"); return; }
  auto list = t->source->playlist();
  Clip* c = findClip(list, selClip_);
  if (!c) return;
  Clip n = *c;
  n.id = t->nextClipId++;
  n.start = c->start + c->length;
  list.push_back(n);
  t->source->setPlaylist(std::move(list));
  selClip_ = n.id;
  setStatus("Clip duplicated");
}

void App::fadeSelectedClip() {
  session::Track* t = session_->track(selTrack_);
  if (!t || selClip_ < 0) { setStatus("Select a clip first"); return; }
  auto list = t->source->playlist();
  Clip* c = findClip(list, selClip_);
  if (!c) return;
  const std::int64_t f = std::min<std::int64_t>(static_cast<std::int64_t>(session_->sampleRate() * 0.01), c->length / 2);
  c->fadeIn = f; c->fadeOut = f;
  t->source->setPlaylist(std::move(list));
  setStatus("10 ms fades added");
}

// ---- files ------------------------------------------------------------------------------
void App::openFileAction(const char* what) {
  dialogKind_ = what;
  dialogOpen_ = true;
  dialogMessage_.clear();
  if (dialogKind_ == "save" || dialogKind_ == "open") dialogPath_ = "session.wezt";
  else if (dialogKind_ == "bounce") dialogPath_ = "bounce.wav";
  else if (dialogKind_ == "import") dialogPath_ = "";
  else dialogPath_.clear();
}

const PeakCache& App::peaksFor(const std::shared_ptr<const AudioBuffer>& a) {
  auto it = peaks_.find(a.get());
  if (it != peaks_.end()) return it->second;
  PeakCache pc;
  if (a && a->channels > 0) {
    const std::size_t nb = (a->frames + PeakCache::kBucket - 1) / PeakCache::kBucket;
    pc.lo.assign(nb, 0.f); pc.hi.assign(nb, 0.f);
    const std::size_t chans = std::min<std::size_t>(2, a->channels);
    if (chans > 1) { pc.lo2.assign(nb, 0.f); pc.hi2.assign(nb, 0.f); }
    for (std::size_t ch = 0; ch < chans; ++ch) {
      const float* d = a->channel(ch);
      std::vector<float>& L = ch == 0 ? pc.lo : pc.lo2;
      std::vector<float>& H = ch == 0 ? pc.hi : pc.hi2;
      for (std::size_t b = 0; b < nb; ++b) {
        float lo = 0.f, hi = 0.f;
        const std::size_t e = std::min(a->frames, (b + 1) * PeakCache::kBucket);
        for (std::size_t i = b * PeakCache::kBucket; i < e; ++i) { lo = std::min(lo, d[i]); hi = std::max(hi, d[i]); }
        L[b] = lo; H[b] = hi;
      }
    }
  }
  return peaks_.emplace(a.get(), std::move(pc)).first->second;
}

void App::drawFileDialog() {
  if (showNewTrack_) { ImGui::OpenPopup("New Track"); showNewTrack_ = false; }
  if (ImGui::BeginPopupModal("New Track", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::InputText("Name", &newTrackName_);
    ImGui::Checkbox("Mono", &newTrackMono_);
    if (ImGui::Button("Create")) {
      TrackId t = -1;
      WEZT_PAUSED(t = session_->addTrack(newTrackName_.empty() ? "Audio" : newTrackName_, newTrackMono_, newTrackMono_ ? std::vector<unsigned>{0} : std::vector<unsigned>{0, 1}));
      selTrack_ = t;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (dialogOpen_) { ImGui::OpenPopup("File"); dialogOpen_ = false; }
  if (ImGui::BeginPopupModal("File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const std::string& k = dialogKind_;
    ImGui::TextUnformatted(k == "new" ? "Start a new session (unsaved changes are lost)" : k == "open" ? "Open session" : k == "save" ? "Save session as" : k == "bounce" ? "Bounce the mix to a WAV file" : "Import an audio file at the cursor");
    if (k != "new") { ImGui::SetNextItemWidth(420); ImGui::InputText("Path", &dialogPath_); }
    if (!dialogMessage_.empty()) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "%s", dialogMessage_.c_str());
    if (ImGui::Button("OK")) {
      std::string err;
      bool ok = false;
      auto nameOf = [](const std::string& path) { std::string n = path; const auto sl = n.find_last_of("/\\"); if (sl != std::string::npos) n = n.substr(sl + 1); const auto d = n.find_last_of('.'); if (d != std::string::npos && d > 0) n = n.substr(0, d); return n.empty() ? std::string("Untitled Session") : n; };
      if (k == "new") {
        WEZT_PAUSED({
          engine_->transport().stop();
          session_ = std::make_unique<session::Session>(48000.0, 2, 512);
          engine_ = std::make_unique<Engine>(session_->graph(), 512, 2, 48000.0, 60);
          session_->attach(engine_->context());
          peaks_.clear(); objects_.clear();
          selTrack_ = session_->addTrack("Audio 1", true);
          markers_.clear(); groups_.clear(); hidden_.clear(); firstTrack_ = 0; sessionName_ = "Untitled Session"; midiParts_.clear(); selPart_ = -1; roll_.load({});
          if (device_ && _was) { device_->stop(); std::string e2; device_->open(audio::AudioConfig{}, engine_.get(), &e2); }
        });
        ok = true;
      } else if (k == "save") { ok = session_->save(dialogPath_, &err); if (ok) { sessionName_ = nameOf(dialogPath_); saveMidiParts(dialogPath_); } }
      else if (k == "open") {
        auto loaded = session::Session::load(dialogPath_, [](const plugins::PluginDescriptor& d) -> std::unique_ptr<plugins::IPluginInstance> { return instruments::makeBuiltin(d); }, &err);
        if (loaded) {
          WEZT_PAUSED({
            engine_->transport().stop();
            session_ = std::move(loaded);
            engine_ = std::make_unique<Engine>(session_->graph(), 512, 2, session_->sampleRate(), 60);
            session_->attach(engine_->context());
            peaks_.clear(); objects_.clear();
            const auto ids = session_->trackIds();
            selTrack_ = ids.empty() ? -1 : ids.front();
            markers_.clear(); groups_.clear(); hidden_.clear(); firstTrack_ = 0; sessionName_ = nameOf(dialogPath_);
            if (device_ && _was) { device_->stop(); std::string e2; device_->open(audio::AudioConfig{}, engine_.get(), &e2); }
          });
          ok = true;
          midiParts_.clear(); selPart_ = -1; roll_.load({});
          loadMidiParts(dialogPath_);
          if (!session_->warnings().empty()) setStatus(session_->warnings().front());
        }
      } else if (k == "bounce") {
        bool wasRunning = deviceRunning_;
        if (wasRunning) device_->stop();
        const auto r = session_->bounce(dialogPath_, 0, std::max<std::int64_t>(endOfSession(), 1), static_cast<std::int64_t>(session_->sampleRate()));
        if (wasRunning) { std::string e2; device_->start(&e2); }
        ok = r.ok; err = r.error;
        if (ok) setStatus("Bounced " + std::to_string(r.frames) + " frames, peak " + std::to_string(r.peak).substr(0, 5));
      } else if (k == "import") {
        ok = session_->addClipFromFile(selTrack_, dialogPath_, position(), &err) >= 0;
        if (ok) setStatus("Imported " + dialogPath_);
      }
      if (ok) ImGui::CloseCurrentPopup(); else dialogMessage_ = err.empty() ? "Failed" : err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

// ---- keyboard -----------------------------------------------------------------------------
void App::handleKeys() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;
  std::uint8_t mods = 0;
  if (io.KeyCtrl) mods |= kCtrl;     // Cmd on macOS (Dear ImGui swaps them there)
  if (io.KeyShift) mods |= kShift;
  if (io.KeyAlt) mods |= kAlt;
  if (io.KeySuper) mods |= kStart;   // Windows key; Control on macOS
  for (int k = 1; k <= static_cast<int>(Key::End); ++k) {
    const ImGuiKey ik = toImGui(static_cast<Key>(k));
    if (ik == ImGuiKey_None || !ImGui::IsKeyPressed(ik, false)) continue;
    // a chord with keys we do not map (e.g. Shift+F4) also tries the version without extra Shift
    if (auto cmd = keymap_.lookup({static_cast<Key>(k), mods})) { execute(*cmd); return; }
  }
}

// ---- top level ------------------------------------------------------------------------------
void App::updateMeters() {
  auto feed = [&](session::Strip* st) {
    if (!st) return;
    auto& l = levels_[st];
    l[0] = meterLevel(reinterpret_cast<const char*>(st) + 1, st->meter().takePeak(0));
    l[1] = meterLevel(reinterpret_cast<const char*>(st) + 2, st->meter().takePeak(1));
  };
  for (auto id : session_->trackIds()) feed(session_->track(id)->strip);
  for (auto b : session_->busIds()) feed(session_->bus(b)->strip);
}

void App::frame() {
  watchAudioDevice();
  updateMeters();
  syncLiveMidi();
  for (session::TrackId id : session_->trackIds()) if (instruments::Sampler* sm = samplerOn(id)) sm->setTempo(static_cast<double>(tempo_));
  handleKeys();
  watchEdits();
  drawMenu();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float menuH = ImGui::GetFrameHeight();
  float top = menuH;
  constexpr ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  auto strip = [&](const char* name, float h, bool pad, auto&& body) {
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad ? ImVec2(8.f, 6.f) : ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));   // the default 32 px minimum would grow the thin strips over their neighbours
    ImGui::Begin(name, nullptr, kFixed);
    ImGui::PopStyleVar(3);
    body();
    ImGui::End();
    top += h;
  };
  strip("##title", 22.f, false, [&] { drawTitle(); });
  if (showTransportBar_) strip("##transport", 104.f, false, [&] { drawTransport(); });
  strip("##overview", 26.f, false, [&] { drawOverview(); });
  const float bottomH = 30.f + 22.f;
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, std::max(60.f, vp->Size.y - top - bottomH)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
  ImGui::Begin("##main", nullptr, kFixed);
  ImGui::PopStyleVar();
  if (view_ == MainView::Edit) {
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##side", ImVec2(176.f, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    drawSidebar(h);
    ImGui::EndChild();
    ImGui::SameLine(0, 2);
    ImGui::BeginChild("##editarea", ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawEdit();
    ImGui::EndChild();
  } else {
    ImGui::SetCursorPos(ImVec2(6.f, 6.f));
    drawMix();
  }
  ImGui::End();
  top = vp->Size.y - bottomH;
  strip("##tabs", 30.f, false, [&] { drawBottomTabs(); });
  if (showMixFloat_ && view_ == MainView::Edit) {
    ImGui::SetNextWindowSize(ImVec2(980.f, 560.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 260.f, vp->Pos.y + 190.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(("Mix: " + sessionName_).c_str(), &showMixFloat_)) drawMix();
    ImGui::End();
  }
  if (showSampler_) drawSamplerWindow();
  if (showImmersive_) drawImmersiveWindow();
  if (showMemory_) drawMemoryWindow();
  if (showClips_) drawClipsWindow();
  if (showVoiceBank_) drawVoiceBankWindow();
  if (showPlugParams_) drawPluginParamsWindow();
  if (showPianoRoll_) drawPianoRollWindow();
  if (showScore_) drawScoreWindow();
  if (showAssistant_) drawAssistantWindow();
  if (showKeymap_) {
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Keyboard shortcuts", &showKeymap_)) {
      ImGui::TextWrapped("Default keys follow the published Pro Tools shortcut table (Windows layout; Cmd replaces Ctrl on macOS). Dim entries are not implemented yet.");
      if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        for (const auto& info : allCommands()) {
          std::string keys;
          for (const Chord& c : keymap_.chordsFor(info.id)) keys += (keys.empty() ? "" : "   ") + Keymap::describe(c);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (!isImplemented(info.id)) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.f));
          ImGui::TextUnformatted(info.name);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(keys.empty() ? "-" : keys.c_str());
          if (!isImplemented(info.id)) ImGui::PopStyleColor();
        }
        ImGui::EndTable();
      }
    }
    ImGui::End();
  }
  drawFileDialog();
  drawStatus();
}

void App::drawStatus() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - 22.f));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, 22.f));
  ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::TextUnformatted(status_.c_str());
  const std::string right = std::string(deviceRunning_ ? device_->backendName() : "no audio device") + "   " + std::to_string(static_cast<int>(session_->sampleRate())) + " Hz";
  ImGui::SameLine(vp->Size.x - ImGui::CalcTextSize(right.c_str()).x - 16.f);
  ImGui::TextDisabled("%s", right.c_str());
  ImGui::End();
}

void App::drawMenu() {
  if (!ImGui::BeginMainMenuBar()) return;
  auto item = [&](Command c, bool selected = false) {
    std::string keys;
    const auto ch = keymap_.chordsFor(c);
    if (!ch.empty()) keys = Keymap::describe(ch.front());
    if (ImGui::MenuItem(commandName(c), keys.c_str(), selected, isImplemented(c))) execute(c);
  };
  if (ImGui::BeginMenu("File")) {
    item(Command::NewSession); item(Command::OpenSession); item(Command::SaveSession); ImGui::Separator();
    item(Command::ImportAudio); item(Command::BounceMix); ImGui::Separator();
    if (ImGui::MenuItem("Quit")) quit_ = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    item(Command::Undo); item(Command::Redo); ImGui::Separator();
    item(Command::Cut); item(Command::Copy); item(Command::Paste); item(Command::Clear); ImGui::Separator();
    item(Command::Duplicate); item(Command::SeparateClip); item(Command::CreateFades); item(Command::StripSilence); item(Command::SelectAll);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Track")) {
    item(Command::NewTrack); item(Command::AddTrack); item(Command::RemoveTrack); item(Command::DuplicateTrack);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    item(Command::ToggleMixEdit); item(Command::ShowTransport); ImGui::Separator();
    item(Command::ZoomIn); item(Command::ZoomOut); item(Command::ZoomSession); item(Command::ZoomSelection); ImGui::Separator();
    item(Command::OpenSampler); item(Command::OpenImmersivePanner); item(Command::ShowMemoryLocations);
    if (ImGui::MenuItem("Piano Roll", nullptr, showPianoRoll_)) showPianoRoll_ = !showPianoRoll_;
    if (ImGui::MenuItem("Score", nullptr, showScore_)) showScore_ = !showScore_;
    if (ImGui::MenuItem("Clips and Playlists", nullptr, showClips_)) showClips_ = !showClips_;
    if (ImGui::MenuItem("My Voice Bank (record vowels)", nullptr, showVoiceBank_)) showVoiceBank_ = !showVoiceBank_;
    if (ImGui::MenuItem("Plugin Parameters and Automation", nullptr, showPlugParams_)) showPlugParams_ = !showPlugParams_;
    if (ImGui::MenuItem("Assistant (local AI)", nullptr, showAssistant_)) showAssistant_ = !showAssistant_;
    if (ImGui::MenuItem("Keyboard shortcuts...")) showKeymap_ = true;
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

}  // namespace wezt::ui
