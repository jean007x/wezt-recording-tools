// UI logic without a window: commands, keyboard dispatch through Dear ImGui's input queue, clip editing,
// track add / remove / duplicate, and drawing every window for a few frames (checks nothing crashes
// or trips ImGui's asserts).
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <memory>

#include <imgui.h>
#include <imgui_internal.h>

#include "io/wav.h"
#include "dsp/effect_plugins.h"
#include "instruments/builtin.h"
#include "ui/app.h"

using namespace wezt;
using namespace wezt::ui;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void frames(App& app, int n = 2) {
  ImGuiIO& io = ImGui::GetIO();
  for (int i = 0; i < n; ++i) { io.DeltaTime = 1.f / 60.f; ImGui::NewFrame(); app.frame(); ImGui::Render(); }
}
static void press(App& app, ImGuiKey key, bool ctrl = false, bool shift = false, bool alt = false) {
  ImGuiIO& io = ImGui::GetIO();
  // on macOS Dear ImGui swaps Ctrl and Cmd: a real Cmd press arrives as Super and is read as Ctrl, so the test sends Super there
  const ImGuiKey ctrlKey = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
  if (ctrl) io.AddKeyEvent(ctrlKey, true);
  if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
  if (alt) io.AddKeyEvent(ImGuiMod_Alt, true);
  io.AddKeyEvent(key, true);
  frames(app, 1);
  io.AddKeyEvent(key, false);
  if (ctrl) io.AddKeyEvent(ctrlKey, false);
  if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
  if (alt) io.AddKeyEvent(ImGuiMod_Alt, false);
  frames(app, 1);
}

namespace {
// A stand-in for the plug-in window machinery: records what the app asks of it.
struct FakeEditorHost : EditorHost {
  int nextId = 0, opened = 0, closedCalls = 0, lastW = 0, lastH = 0; bool userClosed = false; int tag = 0;
  int open(const std::string&, int, int, void** parent) override { *parent = &tag; ++opened; return nextId++; }
  void resize(int, int w, int h) override { lastW = w; lastH = h; }
  bool wasClosed(int) override { return userClosed; }
  void close(int) override { ++closedCalls; }
};
// A plug-in with a window: it wants 300x200, then asks for 400x250.
struct WindowedPlugin : plugins::IPluginInstance {
  bool open = false, resizePending = false; int closes = 0;
  void process(const AudioBuffer&, AudioBuffer&) override {}
  std::size_t parameterCount() const override { return 0; }
  std::vector<std::uint8_t> saveState() const override { return {}; }
  bool loadState(const std::vector<std::uint8_t>&) override { return true; }
  bool editorSupported() const override { return true; }
  bool openEditor(void* parent, int* w, int* h, std::string*) override { if (!parent) return false; open = true; *w = 300; *h = 200; return true; }
  void closeEditor() override { if (open) ++closes; open = false; }
  bool editorResized(int* w, int* h) override { if (!resizePending) return false; resizePending = false; *w = 400; *h = 250; return true; }
};
}  // namespace

int main() {
  const std::filesystem::path cfg = std::filesystem::temp_directory_path() / "wezt_ui_test_config";
  std::filesystem::remove_all(cfg);
#if defined(_WIN32)
  _putenv_s("WEZT_CONFIG_DIR", cfg.string().c_str());
#else
  setenv("WEZT_CONFIG_DIR", cfg.string().c_str(), 1);
#endif
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1600.f, 900.f);
  unsigned char* px; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  App::applyTheme();
  {
    App app;
    auto& s = app.session();
    frames(app);
    CHECK(s.trackCount() == 1);

    // ---- + / - / duplicate (the Logic-style buttons) through commands
    app.execute(Command::AddTrack);
    CHECK(s.trackCount() == 2);
    const auto added = app.selectedTrack();
    app.execute(Command::DuplicateTrack);
    CHECK(s.trackCount() == 3 && app.selectedTrack() != added);
    app.execute(Command::RemoveTrack);
    CHECK(s.trackCount() == 2);
    app.execute(Command::RemoveTrack);
    CHECK(s.trackCount() == 1);
    app.execute(Command::RemoveTrack);
    CHECK(s.trackCount() == 1);   // the last track stays
    CHECK(app.status().find("at least one") != std::string::npos);

    // ---- keyboard: Pro Tools keys reach the commands
    auto& tr = app.engine().transport();
    CHECK(!tr.playing());
    press(app, ImGuiKey_Space);
    CHECK(tr.playing());
    press(app, ImGuiKey_Space);
    CHECK(!tr.playing());
    press(app, ImGuiKey_F5); CHECK(app.tool() == Tool::Zoomer);
    press(app, ImGuiKey_F8); CHECK(app.tool() == Tool::Grabber);
    press(app, ImGuiKey_F7); CHECK(app.tool() == Tool::Selector);
    press(app, ImGuiKey_F4); CHECK(app.editMode() == EditMode::Grid);
    press(app, ImGuiKey_F1); CHECK(app.editMode() == EditMode::Shuffle);
    press(app, ImGuiKey_Equal, true); CHECK(app.view() == MainView::Mix);
    press(app, ImGuiKey_Equal, true); CHECK(app.view() == MainView::Edit);
    const std::size_t before = s.trackCount();
    press(app, ImGuiKey_D, false, true, true);               // Alt+Shift+D: duplicate track
    CHECK(s.trackCount() == before + 1);
    press(app, ImGuiKey_Equal, true, true);                  // Ctrl+Shift+=: add track (our own key)
    CHECK(s.trackCount() == before + 2);
    press(app, ImGuiKey_Space, true);                        // Ctrl+Space: record needs an armed track
    CHECK(!tr.recording() && app.status().find("Arm") != std::string::npos);
    press(app, ImGuiKey_Z, true);                            // Ctrl+Z with nothing recorded yet: says so, changes nothing
    CHECK(app.status().find("Nothing to undo") != std::string::npos);
    app.execute(Command::CommitTrack);                       // still not built: said so
    CHECK(app.status().find("not available") != std::string::npos);
    // typing in a text field must not trigger shortcuts
    io.AddInputCharacter('x');
    app.setView(MainView::Edit);
    CHECK(app.view() == MainView::Edit);
    CHECK(!app.isImplemented(Command::CommitTrack) && app.isImplemented(Command::PlayStop) && app.isImplemented(Command::Undo));

    // ---- clip editing
    session::Session& ss = app.session();
    const auto t = ss.trackIds().front();
    app.selectTrack(t);
    auto buf = std::make_shared<AudioBuffer>(1, 48000);
    for (std::size_t i = 0; i < 48000; ++i) buf->channel(0)[i] = std::sin(0.05f * static_cast<float>(i));
    session::Clip c; c.audio = buf; c.start = 10000; c.length = 48000;
    const int id = ss.addClip(t, c);
    frames(app, 2);                                          // each settled edit is one undo step
    app.selectClip(t, id);
    app.setRange(30000, 30000);
    app.execute(Command::SeparateClip);
    frames(app, 2);
    auto list = ss.track(t)->source->playlist();
    CHECK(list.size() == 2);
    std::sort(list.begin(), list.end(), [](const session::Clip& a, const session::Clip& b) { return a.start < b.start; });
    CHECK(list[0].start == 10000 && list[0].length == 20000 && list[1].start == 30000 && list[1].length == 28000 && list[1].offset == 20000);
    app.execute(Command::Duplicate);
    frames(app, 2);
    CHECK(ss.track(t)->source->playlist().size() == 3);
    app.execute(Command::CreateFades);
    frames(app, 2);
    bool fades = false;
    for (const auto& k : ss.track(t)->source->playlist()) fades = fades || (k.fadeIn > 0 && k.fadeOut > 0);
    CHECK(fades);
    app.execute(Command::DeleteSelection);
    frames(app, 2);
    CHECK(ss.track(t)->source->playlist().size() == 2);
    {   // undo walks back through the edits one by one, redo forward again
      auto n = [&] { return ss.track(t)->source->playlist().size(); };
      auto anyFade = [&] { bool f = false; for (const auto& k : ss.track(t)->source->playlist()) f = f || k.fadeIn > 0; return f; };
      CHECK(app.undoStack().canUndo());
      app.execute(Command::Undo); frames(app, 2);
      CHECK(n() == 3 && anyFade());                  // the delete is undone
      app.execute(Command::Undo); frames(app, 2);
      CHECK(n() == 3 && !anyFade());                 // then the fades
      app.execute(Command::Undo); frames(app, 2);
      CHECK(n() == 2);                               // then the duplicate
      app.execute(Command::Redo); frames(app, 2);
      CHECK(n() == 3 && !anyFade());
      app.execute(Command::Redo); app.execute(Command::Redo); frames(app, 2);
      CHECK(n() == 2);                               // fades and delete again
      CHECK(!app.undoStack().canRedo() && app.undoStack().undoLabel() == "Edit clips");
      app.execute(Command::Redo);
      CHECK(app.status().find("Nothing to redo") != std::string::npos);
    }
    app.execute(Command::DeleteSelection);   // nothing selected any more
    CHECK(app.status().find("Select a clip") != std::string::npos);
    {   // strip silence on a clip with two bursts: it becomes two clips, and undo brings the one clip back
      auto b = std::make_shared<AudioBuffer>(1, 48000);
      for (std::size_t i = 0; i < 48000; ++i) b->channel(0)[i] = ((i > 8000 && i < 14000) || (i > 30000 && i < 40000)) ? 0.5f * std::sin(0.3f * static_cast<float>(i)) : 0.f;
      session::Clip sc; sc.audio = b; sc.start = 200000; sc.length = 48000;
      const std::size_t base = ss.track(t)->source->playlist().size();
      const int sid = ss.addClip(t, sc);
      frames(app, 2);
      app.selectClip(t, sid);
      app.execute(Command::StripSilence); frames(app, 2);
      CHECK(ss.track(t)->source->playlist().size() == base + 2);
      app.execute(Command::Undo); frames(app, 2);
      CHECK(ss.track(t)->source->playlist().size() == base + 1);
      app.selectClip(t, -1);
      app.execute(Command::StripSilence);
      CHECK(app.status().find("Select a clip") != std::string::npos);
    }
    {   // memory locations window and the assistant panel draw and work
      app.session().memory().add(edit::MemoryLocation{0, "verse", "", 48000, 48000, 0, {}, 0, 0});
      app.showMemory(true);
      const std::string dir = "wezt_ui_test_assistant";
      std::filesystem::remove_all(dir);
      app.setAssistantDir(dir);
      app.showAssistant(true);
      frames(app, 3);
      CHECK(app.assistantSay("remember that I like it warm").find("remember") != std::string::npos);
      CHECK(app.assistantSay("add a track").find("Added") != std::string::npos);
      CHECK(app.assistantSay("undo").size() > 0);
      {  // text commands from the feature list
        const std::string r = app.assistantSay("suggest a chord progression, sad and dreamy");
        CHECK(r.find('|') != std::string::npos);
        CHECK(app.assistantSay("how do I sidechain?").find("1.") != std::string::npos);
        const std::size_t before = app.session().trackCount();
        app.assistantSay("find a dreamy pad");
        CHECK(app.session().trackCount() == before + 1);
        CHECK(app.assistantSay("group the drum tracks").find("group") != std::string::npos);
        CHECK(app.assistantSay("find frequency clashes").size() > 0);
      }
      CHECK(app.assistantSay("analyze \"/no/such/file.wav\"").find("could not open") != std::string::npos);
      frames(app, 3);
      app.showAssistant(false); app.showMemory(false);
      std::filesystem::remove_all(dir);
    }
    // transport helpers
    app.execute(Command::GoToEnd);
    app.execute(Command::ReturnToStart);
    app.execute(Command::ZoomSession);

    // ---- draw everything (edit, mix, sampler, immersive, keymap) with data on screen
    app.loadDemo();
    for (auto v : {MainView::Edit, MainView::Mix}) { app.setView(v); frames(app, 3); }
    {   // MIDI parts, piano roll and score
      const int part = app.newMidiPart("Test melody", "keys");
      CHECK(part >= 0 && app.midiParts().size() == 1);
      CHECK(app.editMidiPart(part, "Add notes", [](edit::PianoRoll& r) { r.addNote(60, 0, 1, 100, 0.25); r.addNote(64, 1, 1, 100, 0.25); r.addNote(67, 2, 2, 100, 0.25); }));
      CHECK(app.midiParts()[0].clip.notes.size() == 3);
      app.showPianoRoll(true); app.showScore(true);
      frames(app, 3);                                   // both windows draw
      CHECK(app.editMidiPart(part, "Transpose", [](edit::PianoRoll& r) { r.selectAll(); r.transposeSelected(12); }));
      CHECK(app.midiParts()[0].clip.notes[0].pitch == 72);
      CHECK(app.undoEdit() && app.midiParts()[0].clip.notes[0].pitch == 60);   // one undo step restores the notes
      CHECK(app.redoEdit() && app.midiParts()[0].clip.notes[0].pitch == 72);
      frames(app, 2);
      const std::string mf = "wezt_ui_part.mid";
      CHECK(app.exportMidiPart(part, mf));
      std::string e;
      CHECK(app.importMidiFile(mf, &e) == 1 && app.midiParts().size() == 2 && app.midiParts()[1].clip.notes.size() == 3);
      {  // singing from lyrics: onto a new track; bad voice and empty lyrics are refused
        const std::size_t t0 = app.session().trackCount();
        std::string msg;
        CHECK(app.singMidiPart(part, "la la la", "tenor", 1, &msg) && app.session().trackCount() == t0 + 1 && !msg.empty());
        CHECK(!app.singMidiPart(part, "", "tenor", 1, &msg) && !app.singMidiPart(part, "la", "robot", 1, &msg));
        app.session().removeTrack(app.session().trackIds().back());
        CHECK(app.singMidiPart(part, "la la la", "alto", 1, &msg, 0.7, 0.15, "gritty_rock") && app.session().trackCount() == t0 + 1);   // a character
        app.session().removeTrack(app.session().trackIds().back());
        CHECK(!app.singMidiPart(part, "la", "alto", 1, &msg, 0.7, 0.15, "robot king") && msg.find("character") != std::string::npos);
        CHECK(!app.singMidiPart(part, "la", "alto", 1, &msg, 0.7, 0.15, "", "no such bank") && msg.find("voice bank") != std::string::npos);   // a bank that does not exist
        {  // Plugin Parameters window: with no plugin, with a plugin, with an automation lane, while playing
          app.showPluginParams(true); frames(app, 2);
          const auto tid = app.session().trackIds().back();
          plugins::PluginDescriptor d = dsp::EqPlugin::descriptor();
          app.session().addInsert(tid, instruments::makeBuiltin(d), d);
          frames(app, 2);
          CHECK(app.session().setParamAutomation(tid, app.session().track(tid)->inserts.size() - 1, dsp::EqPlugin::HighGain, {{0, 0.5f}, {96000, 0.9f}}));
          frames(app, 3);
          app.engine().transport().play(); frames(app, 3); app.engine().transport().stop(); frames(app, 3);
          app.session().removeInsert(tid, app.session().track(tid)->inserts.size() - 1);
          frames(app, 2); app.showPluginParams(false);
        }
        app.showVoiceBank(true); frames(app, 3); app.showVoiceBank(false);                                                                       // the window draws
        const std::string reply = app.assistantSay("sing \"la la la\"");
        CHECK(reply.find("no melody") != std::string::npos || reply.find("Sang") != std::string::npos || reply.find("Added") != std::string::npos);
        const std::size_t t1 = app.session().trackCount();
        if (t1 > t0) app.session().removeTrack(app.session().trackIds().back());
      }
      std::string liveName;
      {  // live MIDI: the part plays through a synth track in step with the transport; the key preview reaches it
        auto trackNamed = [&](const std::string& n) -> session::Track* { for (auto id : app.session().trackIds()) if (auto* t = app.session().track(id)) if (t->name == n) return t; return nullptr; };
        const std::size_t t0 = app.session().trackCount();
        const std::size_t nNotes = app.midiParts()[static_cast<std::size_t>(part)].clip.notes.size();
        CHECK(nNotes >= 2);
        CHECK(app.playMidiPartLive(part) && app.session().trackCount() == t0 + 1);
        liveName = app.midiParts()[static_cast<std::size_t>(part)].liveTrack;
        CHECK(!liveName.empty() && app.midiParts()[static_cast<std::size_t>(part)].liveOwned);
        bool listed = false; for (auto& n : app.midiTargetTracks()) listed |= n == liveName;
        CHECK(listed);
        frames(app, 2);
        session::Track* tr = trackNamed(liveName);
        CHECK(tr && tr->source->midiSequence() && tr->source->midiSequence()->events.size() == 2 * nNotes);
        CHECK(app.playMidiPartLive(part) && app.session().trackCount() == t0 + 1);          // already live: no second track
        CHECK(app.editMidiPart(part, "Delete note", [](edit::PianoRoll& r) { r.select(0, false); r.deleteSelected(); }));
        frames(app, 2);
        CHECK(tr->source->midiSequence()->events.size() == 2 * (nNotes - 1));              // the sequence follows the edit
        CHECK(app.previewNote(64, true) && app.previewNote(64, false));                     // click-a-key reaches the track's instrument
        CHECK(!app.previewNote(200, true));
        {  // a real mouse press on the piano-roll keyboard column: sounds a key, slides to another, stops on release
          app.showScore(false); frames(app, 2);   // the Score window opens at the same spot and would take the click
          ImGuiWindow* w = ImGui::FindWindowByName("Piano Roll");
          CHECK(w != nullptr);
          if (w) {
            ImGuiIO& io = ImGui::GetIO();
            const float x = w->Pos.x + 30.f, y0 = w->Pos.y + 160.f;   // below the toolbar rows (the MIDI keyboard chooser has its own row)
            io.AddMousePosEvent(x, y0); frames(app, 2);
            CHECK(app.previewingKey() == -1);
            io.AddMouseButtonEvent(0, true); frames(app, 2);
            const int k0 = app.previewingKey();
            std::printf("mouse press on the keys sounded key %d\n", k0);
            CHECK(k0 >= 0 && k0 <= 127);
            io.AddMousePosEvent(x, y0 + 60.f); frames(app, 2);
            CHECK(app.previewingKey() >= 0 && app.previewingKey() < k0);   // slid down the keyboard to a lower key
            io.AddMouseButtonEvent(0, false); frames(app, 2);
            CHECK(app.previewingKey() == -1);                              // released: the note is off
            io.AddMousePosEvent(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + 130.f); frames(app, 1);
          }
        }
        CHECK(!app.setMidiPartTarget(part, "no such track"));
        app.execute(Command::Undo); frames(app, 2);
        CHECK(tr->source->midiSequence()->events.size() == 2 * nNotes);                    // undo restores it
      }
      const std::size_t tracksBefore = app.session().trackCount();
      CHECK(app.renderMidiPart(part) && app.session().trackCount() == tracksBefore + 1);
      CHECK(app.saveMidiParts("wezt_ui_part_session.wezt") && app.loadMidiParts("wezt_ui_part_session.wezt"));
      CHECK(app.midiParts().size() == 4);               // loaded on top of the two (the real open clears them first)
      CHECK(app.midiParts()[2].liveTrack == liveName && app.midiParts()[2].liveOwned);   // the binding is saved with the parts
      for (auto id : app.session().trackIds()) if (auto* t = app.session().track(id)) if (t->name == liveName) { app.session().removeTrack(id); break; }
      CHECK(app.setMidiPartTarget(0, ""));
      CHECK(app.removeMidiPart(3) && app.removeMidiPart(2) && app.removeMidiPart(1) && app.removeMidiPart(0) && app.midiParts().empty());
      frames(app, 2);                                   // windows handle "no part"
      app.showPianoRoll(false); app.showScore(false);
      std::filesystem::remove(mf); std::filesystem::remove("wezt_ui_part_session.wezt.midi");
      app.session().removeTrack(app.session().trackIds().back());
      app.selectTrack(app.session().trackIds().front());
    }
    {   // clips list and playlists (on a scratch track, so the demo's own tracks stay as they are)
      app.setView(MainView::Edit);
      auto& se = app.session();
      const auto tt = se.addTrack("ClipsTest", true, {});
      auto silent = std::make_shared<AudioBuffer>(1, 48000);
      for (int i = 0; i < 3; ++i) { session::Clip c; c.audio = silent; c.start = 48000 * (10 + i); c.length = 24000; c.locked = i == 2; se.addClip(tt, c); }
      const auto rows = app.clipRows();
      CHECK(rows.size() >= 3);
      for (std::size_t i = 1; i < rows.size(); ++i) CHECK(rows[i - 1].start <= rows[i].start);   // timeline order
      int mine = -1; for (auto& r : rows) if (r.track == tt) { mine = r.clip; break; }
      CHECK(mine > 0);
      CHECK(app.renameClip(tt, mine, "Hook") && app.muteClip(tt, mine, true) && app.setClipGainDb(tt, mine, -3.f));
      bool found = false;
      for (auto& r : app.clipRows()) if (r.track == tt && r.clip == mine) found = r.name == "Hook" && r.muted && std::fabs(r.gainDb + 3.f) < 1e-4f;
      CHECK(found);
      CHECK(app.locateClip(tt, mine) && app.selectedTrack() == tt);
      CHECK(se.newPlaylist(tt, "Alt", true));
      app.showClips(true);
      frames(app, 3);                                   // window draws with playlists present
      CHECK(se.switchPlaylist(tt, "Main"));
      frames(app, 2);
      int locked = -1; for (auto& r : app.clipRows()) if (r.track == tt && r.locked) locked = r.clip;
      CHECK(locked > 0 && !app.deleteClip(tt, locked));  // locked clips are not deleted
      CHECK(app.deleteClip(tt, mine));
      CHECK(!app.deleteClip(9999, 1));
      app.showClips(false);
      se.removeTrack(tt);
      app.selectTrack(se.trackIds().front());
    }
    app.showSampler(true);
    app.selectTrack(app.session().trackIds().back());
    frames(app, 3);
    CHECK(app.samplerOn(app.selectedTrack()) != nullptr);
    {   // the other modes and the LFO / matrix pages draw too, and the demo is sliced from C2
      instruments::Sampler* sm = app.samplerOn(app.selectedTrack());
      CHECK(sm->zones().size() >= 8 && sm->zones()[1].rootKey == 49 && sm->params().mode == instruments::SamplerMode::Slice);
      for (auto m : {instruments::SamplerMode::Classic, instruments::SamplerMode::OneShot, instruments::SamplerMode::Slice}) {
        instruments::SamplerParams q = sm->params(); q.mode = m; q.lfo[1].sync = true; q.lfo[1].on = false; sm->setParams(q);
        frames(app, 2);
      }
    }
    app.showImmersive(true);
    frames(app, 3);
    app.setView(MainView::Edit);
    frames(app, 3);
    // the sampler is part of the session: save, reload, and it is still there with its zone
    const std::string path = "wezt_ui_test.wezt";
    std::string err;
    CHECK(app.session().save(path, &err));
    auto loaded = session::Session::load(path, [](const plugins::PluginDescriptor& d) -> std::unique_ptr<plugins::IPluginInstance> {
      if (d.format == plugins::Format::Builtin) return std::make_unique<instruments::Sampler>();
      return nullptr;
    }, &err);
    CHECK(loaded != nullptr);
    if (loaded) {
      int samplers = 0;
      for (auto tid : loaded->trackIds()) for (auto& in : loaded->track(tid)->inserts) if (dynamic_cast<instruments::Sampler*>(in.instance)) ++samplers;
      CHECK(samplers == 1);
    }
    std::remove(path.c_str());
    // export objects as ADM from the demo, and measure the mix
    app.execute(Command::OpenImmersivePanner);
    frames(app, 2);
    std::string msg;
    const std::string adm = "wezt_ui_test_adm.wav";
    CHECK(app.exportAdm(adm, &msg));
    io::WavReader rd;
    CHECK(rd.open(adm, &err) && rd.info().channels == app.session().trackCount() && rd.info().frames > 48000 * 5);
    const std::size_t nch = rd.info().channels;
    std::vector<double> energy(nch, 0.0);
    {
      std::vector<std::vector<float>> d(nch, std::vector<float>(4096));
      std::vector<float*> p(nch);
      for (std::size_t k = 0; k < nch; ++k) p[k] = d[k].data();
      for (std::uint64_t at = 0; at + 4096 < rd.info().frames; at += 4096) {
        rd.read(at, 4096, p.data());
        for (std::size_t k = 0; k < nch; ++k) for (float v : d[k]) energy[k] += static_cast<double>(v) * v;
      }
    }
    for (std::size_t k = 0; k < nch; ++k) std::printf("track %zu energy %g\n", k, energy[k]);
    std::size_t loud = 0;
    for (double e : energy) if (e > 1.0) ++loud;
    CHECK(loud >= 4 && loud <= 6);   // drums, bass, pad and sweep carry audio; the sampler track may hold a few audition notes; the two test tracks are empty
    CHECK(energy[1] == 0.0 && energy[2] == 0.0 && energy[0] > 1.0 && energy[3] > 1.0 && energy[4] > 1.0 && energy[5] > 1.0);
    rd.close();
    std::remove(adm.c_str());
    CHECK(app.analyzeMix(&msg) && msg.find("LUFS") != std::string::npos);
    std::printf("%s\n", msg.c_str());
    CHECK(!app.exportAdm("/no/such/dir/x.wav", &msg) && !msg.empty());
  }
  {  // a plug-in with its own window: opened through the host, resized when it asks, closed when the user closes it or the insert goes away
    App app;
    FakeEditorHost host;
    app.setEditorHost(&host);
    auto& s = app.session();
    frames(app);
    const auto t = app.selectedTrack();
    auto plugin = std::make_unique<WindowedPlugin>();
    WindowedPlugin* wp = plugin.get();
    plugins::PluginDescriptor d; d.name = "Windowed"; d.format = plugins::Format::Builtin;
    CHECK(s.addInsert(t, std::move(plugin), d));
    app.openPluginEditor(t, 0);
    CHECK(host.opened == 1 && wp->open && app.openEditorCount() == 1 && host.lastW == 300 && host.lastH == 200);
    app.openPluginEditor(t, 0);                       // already open: no second window
    CHECK(host.opened == 1 && app.openEditorCount() == 1);
    wp->resizePending = true; frames(app);
    CHECK(host.lastW == 400 && host.lastH == 250);
    host.userClosed = true; frames(app);
    CHECK(app.openEditorCount() == 0 && host.closedCalls == 1 && wp->closes == 1 && !wp->open);
    host.userClosed = false;
    app.openPluginEditor(t, 0);                       // and again, then the insert is removed while its window is open
    CHECK(app.openEditorCount() == 1);
    CHECK(s.removeInsert(t, 0));
    frames(app);
    CHECK(app.openEditorCount() == 0 && host.closedCalls == 2);
  }
  {  // cut, copy and paste: a clip, and a range covering parts of clips
    App app;
    frames(app);
    auto& s = app.session();
    const auto t1 = app.selectedTrack();
    auto buf = std::make_shared<AudioBuffer>(1, 4800);
    session::Clip c; c.audio = buf; c.start = 10000; c.length = 4800; c.name = "clip";
    const int id = s.addClip(t1, c);
    auto list = [&] { return s.track(t1)->source->playlist(); };
    const std::size_t base = list().size();
    app.selectClip(t1, id);
    app.execute(Command::Copy);
    CHECK(list().size() == base);                      // copy leaves the clip where it is
    app.execute(Command::Paste);                       // at the playhead (0)
    CHECK(list().size() == base + 1);

    app.setRange(10000, 12400);                        // a range: the first half of the original clip
    app.execute(Command::Cut);
    bool rightPiece = false;
    for (const auto& k : list()) if (k.start == 12400 && k.length == 2400 && k.offset == 2400) rightPiece = true;
    CHECK(rightPiece);                                 // what lies outside the range stays, trimmed
    CHECK(list().size() == base + 1);
    app.setRange(0, 0);
    app.execute(Command::Paste);                       // 2400 frames land at 0 over the first pasted clip (overwrite)
    bool pasted = false, rest = false;
    for (const auto& k : list()) {
      if (k.start == 0 && k.length == 2400) pasted = true;
      if (k.start == 2400 && k.length == 2400 && k.offset == 2400) rest = true;
    }
    CHECK(pasted && rest);
    CHECK(list().size() == base + 2);

    app.setRange(0, 15000);                            // a range over several clips copies all of them
    app.execute(Command::Copy);
    app.execute(Command::AddTrack);
    app.setRange(0, 0);
    app.execute(Command::Paste);
    CHECK(s.track(app.selectedTrack())->source->playlist().size() == 3);
  }
  {  // clip editing commands: mute, trim, insert silence, repeat, heal
    App app;
    frames(app);
    auto& s = app.session();
    const auto t1 = app.selectedTrack();
    auto buf = std::make_shared<AudioBuffer>(1, 20000);
    session::Clip c; c.audio = buf; c.start = 1000; c.offset = 0; c.length = 8000; c.name = "c";
    const int id = s.addClip(t1, c);
    auto list = [&] { return s.track(t1)->source->playlist(); };
    auto find = [&](int i) -> session::Clip { for (const auto& k : list()) if (k.id == i) return k; session::Clip none; return none; };
    app.selectClip(t1, id);
    app.execute(Command::MuteClips);
    CHECK(find(id).muted);
    app.execute(Command::MuteClips);
    CHECK(!find(id).muted);
    app.setRange(3000, 6000);
    app.execute(Command::TrimClipToSelection);
    CHECK(find(id).start == 3000 && find(id).length == 3000 && find(id).offset == 2000);
    app.setRange(0, 0);
    app.execute(Command::TrimStartToInsertion);          // the playhead is at 0: nothing before it to cut away
    CHECK(find(id).start == 3000 && find(id).length == 3000);
    app.setRange(4000, 5000);                            // make room inside the clip
    app.execute(Command::InsertSilence);
    CHECK(list().size() == 2);                           // separated at 4000, the second part moved 1000 later
    bool moved = false;
    for (const auto& k : list()) if (k.start == 5000 && k.length == 2000 && k.offset == 3000) moved = true;
    CHECK(moved);
    app.setRange(0, 0);
    app.selectClip(t1, id);
    app.execute(Command::RepeatSelection);               // the first part (3000..4000) repeats right after itself
    bool rep = false;
    for (const auto& k : list()) if (k.start == 4000 && k.length == 1000 && k.id != id) rep = true;
    CHECK(rep);
    CHECK(list().size() == 3);
    // heal: two pieces that follow on exactly become one again
    session::Clip a = c; a.start = 30000; a.offset = 0; a.length = 3000;
    session::Clip b = c; b.start = 33000; b.offset = 3000; b.length = 2000;
    const int ia = s.addClip(t1, a); s.addClip(t1, b);
    app.selectClip(t1, ia);
    app.execute(Command::HealSeparation);
    CHECK(find(ia).length == 5000);
  }
  {  // recording in the app: once the transport has stopped the take is on the track as a clip, with a status line and an undo step
    App app;
    frames(app);
    auto& s = app.session();
    const auto dir = std::filesystem::temp_directory_path() / "wezt_ui_rec";
    std::filesystem::remove_all(dir);
    s.setRecordFolder((dir / "Audio Files").string());
    const auto t = s.addTrack("Vox UI", true, {0});
    s.track(t)->source->arm(true);
    const std::size_t before = s.track(t)->source->playlist().size();
    audio::AudioConfig cfg; cfg.sampleRate = 48000; cfg.bufferFrames = 128; cfg.inChannels = 2; cfg.outChannels = 2;
    app.engine().deviceAboutToStart(cfg);
    app.engine().transport().locate(1000);
    app.engine().transport().record();
    std::vector<float> in0(128, 0.25f), in1(128, 0.25f), o0(128), o1(128);
    const float* ins[2] = {in0.data(), in1.data()};
    float* outs[2] = {o0.data(), o1.data()};
    for (int i = 0; i < 20; ++i) app.engine().process(ins, outs, 128);
    frames(app, 2);                                   // the app sees recording
    app.engine().transport().stop();
    app.engine().process(ins, outs, 128);
    frames(app, 3);                                   // ... and the stop: the take is committed
    CHECK(s.track(t)->source->playlist().size() == before + 1);
    std::filesystem::remove_all(dir);
  }
  {  // drawing plug-in automation with the mouse in the Plugin Parameters graph: click adds a point, undo removes it
    App app;
    auto& s = app.session();
    frames(app);
    const auto t = app.selectedTrack();
    plugins::PluginDescriptor d; d.name = "Wezt EQ"; d.format = plugins::Format::Builtin; d.bitness = 64;
    CHECK(s.addInsert(t, instruments::makeBuiltin(d), d));
    const unsigned pid = s.track(t)->inserts[0].instance->parameters().back().id;
    CHECK(s.setParamAutomation(t, 0, pid, {{0, 0.2f}, {96000, 0.8f}}));
    app.selectPluginParam(t, 0, static_cast<int>(pid));
    app.showPluginParams(true);
    frames(app, 3);
    ImGuiWindow* w = ImGui::FindWindowByName("Plugin Parameters");
    CHECK(w != nullptr);
    if (w) {
      ImGuiIO& io = ImGui::GetIO();
      auto count = [&] { const auto* l = s.paramAutomation(t, 0, pid); return l ? l->points().size() : std::size_t(0); };
      CHECK(count() == 2);
      io.AddMousePosEvent(w->Pos.x + w->Size.x * 0.7f, w->Pos.y + w->Size.y - 50.f); frames(app, 2);
      io.AddMouseButtonEvent(0, true); frames(app, 2);
      io.AddMouseButtonEvent(0, false); frames(app, 2);
      std::printf("points after one click in the graph: %zu\n", count());
      CHECK(count() == 3);
      app.execute(Command::Undo); frames(app, 2);
      CHECK(count() == 2);                                  // the drawn point is one undo step
    }
    app.showPluginParams(false);

    {  // File > Settings: the intro switch and the intro sound are saved in the settings file
      CHECK(app.prefs().introEnabled && app.prefs().introSoundPath.empty());
      app.showSettings(true); frames(app, 3);                 // the window draws
      app.setIntroEnabled(false);
      CHECK(!loadPrefs().introEnabled);
      app.setIntroEnabled(true);
      CHECK(loadPrefs().introEnabled);
      CHECK(app.prefs().introLogoBlack);
      app.setIntroLogoBlack(false);
      CHECK(!loadPrefs().introLogoBlack);
      app.setIntroLogoBlack(true);
      std::string m;
      CHECK(!app.setIntroSound("/definitely/not/here.wav", &m) && !m.empty() && app.prefs().introSoundPath.empty());
      CHECK(!app.setIntroSound("/definitely/not/here.flac", &m) && m.find("WAV or MP3") != std::string::npos);
      const std::string wav = (cfg / "mine.wav").string();
      {
        io::WavWriter ww; std::string e;
        CHECK(ww.open(wav, 1, 44100, io::WavSampleType::Int16, &e));
        std::vector<float> x(22050, 0.25f); const float* p = x.data();
        CHECK(ww.write(&p, x.size()) && ww.finalize(&e));
      }
      CHECK(app.setIntroSound(wav, &m) && m.rfind("Using your file", 0) == 0);
      CHECK(loadPrefs().introSoundPath == wav);
      frames(app, 3);
      CHECK(app.setIntroSound("", &m) && loadPrefs().introSoundPath.empty());
      CHECK(!app.takeIntroReplay());
      app.showSettings(false); frames(app, 2);
    }
  }
  std::filesystem::remove_all(cfg);
  ImGui::DestroyContext();
  std::printf(failures ? "FAILED\n" : "ui tests passed\n");
  return failures;
}
