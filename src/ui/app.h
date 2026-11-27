#pragma once
// The Wezt user interface (Dear ImGui). Everything here is toolkit-neutral except the draw calls:
// the app builds one frame of widgets in App::frame(); a thin backend (GLFW + OpenGL, or the CPU
// rasteriser used for screenshots) turns the draw data into pixels. The UI is immediate-mode, so it
// keeps no widget tree in memory and sleeps when nothing moves.
#include <array>
#include <map>
#include <set>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ai/assistant.h"
#include "audio/audio_device.h"
#include "edit/clip_tools.h"
#include "edit/piano_roll.h"
#include "agent/plugin_agent.h"
#include "edit/undo.h"
#include "midi/midi_input.h"
#include "music/midi_edit.h"
#include "session/midi_player.h"
#include "music/midi.h"
#include "audio/audio_device.h"
#include "engine/engine.h"
#include "immersive/adm.h"
#include "immersive/layout.h"
#include "immersive/loudness.h"
#include "immersive/renderer.h"
#include "instruments/sampler.h"
#include "plugins/plugin_library.h"
#include "session/session.h"
#include "ui/commands.h"
#include "ui/intro_sound.h"
#include "util/prefs.h"

namespace wezt::ui {

enum class Tool { Zoomer, Trimmer, Selector, Grabber, Scrubber, Pencil, Smart };
enum class EditMode { Shuffle, Slip, Spot, Grid };
enum class MainView { Edit, Mix };

// Min/max pairs per 256-frame bucket, so a waveform of any zoom is drawn from a few KB.
struct PeakCache {
  static constexpr std::size_t kBucket = 256;
  std::vector<float> lo, hi;     // channel 0
  std::vector<float> lo2, hi2;   // channel 1 of a stereo file (empty for mono)
};

struct ObjectPos { float x = 0.f, y = 1.f, z = 0.f, size = 0.f; };
inline constexpr float kGridBeats[5] = {1.f, 0.5f, 0.25f, 0.125f, 4.f};
inline constexpr const char* kGridNames[5] = {"1/4 note", "1/8 note", "1/16 note", "1/32 note", "1 bar"};
inline constexpr double kNudgeMs[5] = {1.0, 10.0, 100.0, 1000.0, 5000.0};
inline constexpr const char* kNudgeNames[5] = {"1 ms", "10 ms", "100 ms", "1 sec", "5 sec"};
struct Marker { std::int64_t frame = 0; std::string name; };
// A track group: while it is active, mute / solo / record-arm / fader moves on one member apply to all.
struct TrackGroup { char letter = 'a'; std::string name; std::vector<session::TrackId> members; bool active = true; };

// What the program around the app provides so a plug-in can have its own window: the app asks for a native window (it never touches the windowing
// system itself), puts the plug-in's editor inside it, and asks about it each frame.
struct EditorHost {
  virtual ~EditorHost() = default;
  virtual int open(const std::string& title, int width, int height, void** nativeParent) = 0;   // returns a window id (>= 0), or -1
  virtual void resize(int id, int width, int height) = 0;
  virtual bool wasClosed(int id) = 0;     // the user closed it
  virtual void close(int id) = 0;
};

class App {
 public:
  App();
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Opens the platform audio device and starts it. False with a message when there is none.
  bool startAudio(std::string* message);
  void stopAudio();
  // Builds a small example session (tones, a sampler) so a first launch has something to show.
  void loadDemo();

  void frame();                      // call once per ImGui frame, between NewFrame() and Render()
  bool animating() const;            // true while something moves (playing, meters, dragging)
  bool wantsQuit() const { return quit_; }
  // ---- settings (File > Settings...) ----
  const Prefs& prefs() const { return prefs_; }
  void showSettings(bool on) { showSettings_ = on; }
  bool takeIntroReplay() { const bool r = introReplay_; introReplay_ = false; return r; }   // the window asked to play the intro again
  bool setIntroSound(const std::string& path, std::string* message);   // "" = built-in; checks the file can be read, then saves
  void setIntroEnabled(bool on);
  void setIntroLogoBlack(bool black);
  static void loadFonts(float scale = 1.f);   // embedded Roboto Medium; call before the first frame
  static void applyTheme();          // colours and spacing; call once after the ImGui context exists
  void setStatus(const std::string& s) { status_ = s; }

  void execute(Command c);
  // Test and snapshot access
  void setView(MainView v) { view_ = v; }
  void showSampler(bool on) { showSampler_ = on; }
  void showImmersive(bool on) { showImmersive_ = on; }
  session::Session& session() { return *session_; }
  // The agent-facing plugin control (the same service the MCP tools use), bound to the current session.
  agent::PluginAgent& pluginAgent();
  Engine& engine() { return *engine_; }
  Keymap& keymap() { return keymap_; }
  const std::string& status() const { return status_; }
  void setTool(Tool t) { tool_ = t; }
  void setEditMode(EditMode m) { mode_ = m; }
  Tool tool() const { return tool_; }
  EditMode editMode() const { return mode_; }
  MainView view() const { return view_; }
  session::TrackId selectedTrack() const { return selTrack_; }
  void selectTrack(session::TrackId t) { selTrack_ = t; }
  // Clip selection (for the edit commands)
  void selectClip(session::TrackId t, int clipId) { selTrack_ = t; selClip_ = clipId; }
  void setRange(std::int64_t a, std::int64_t b) { selStart_ = a; selEnd_ = b; }
  // Where the track lanes were drawn last frame (for tests that drive the timeline with the mouse).
  void showClipFx(bool on) { showClipFx_ = on; }
  void showBigCounter(bool on) { showBigCounter_ = on; }
  struct TimelineGeometry { float x0 = 0, lanesTop = 0, laneH = 0, framesPerPixel = 1; double scroll = 0; int firstRow = 0; };
  const TimelineGeometry& timelineGeometry() const { return geo_; }
  std::int64_t selectionStart() const { return selStart_; }
  std::int64_t selectionEnd() const { return selEnd_; }
  instruments::Sampler* samplerOn(session::TrackId t);
  // One mono object channel per track, with its position, into an ADM BWF file (renders each track alone).
  bool exportAdm(const std::string& path, std::string* message);
  // Renders the mix and measures it: integrated and short-term loudness, true peak.
  bool analyzeMix(std::string* message);
  bool isImplemented(Command c) const;
  // Markers, groups and display state (kept with the app, not in the session file yet)
  int addMarker(std::int64_t frame, const std::string& name = {});
  std::size_t markerCount() const { return markers_.size(); }
  bool musicCommandFromText(const std::string& lowered, std::string& reply);
  int addGroup(const std::string& name, const std::vector<session::TrackId>& members);
  std::size_t groupCount() const { return groups_.size(); }
  void setGroupActive(int index, bool on) { if (index >= 0 && index < static_cast<int>(groups_.size())) groups_[static_cast<std::size_t>(index)].active = on; }
  void setMuteGrouped(session::TrackId t, bool on);
  void setSoloGrouped(session::TrackId t, bool on);
  void setArmGrouped(session::TrackId t, bool on);
  // Input monitoring: Auto -> Input -> Off -> Auto.
  void cycleMonitor(session::TrackId t);
  // Special Paste: `repeats` copies in a row, `gapSeconds` apart; merge keeps what is underneath instead of replacing it.
  // Capture Clip: the selected range on the selected track becomes a clip of its own.
  bool captureClip();
  // Clip colour: 0 = the track's colour, 1..8 = palette. The Clip FX window has swatches; Ctrl+Alt+K cycles.
  int selectedClipColor() const;
  bool setClipColor(session::TrackId t, int clipId, int color);
  // MIDI output: the keyboard's notes and the MIDI parts go to a hardware device (the output chooser in the Piano Roll). The test hook takes any device.
  bool openMidiOut(std::size_t index);
  // Real-Time Properties of a part: transpose, velocity, length and quantize applied when it plays.
  bool setPartRealTime(int part, const music::RealTime& rt) { if (part < 0 || part >= static_cast<int>(midiParts_.size())) return false; midiParts_[static_cast<std::size_t>(part)].rt = rt; return true; }
  void closeMidiOut();
  void setMidiOutput(std::unique_ptr<midi::IMidiOutput> dev);
  bool hasMidiOutput() const { return hwOut_ != nullptr; }
  bool pasteSpecial(int repeats, double gapSeconds, bool merge);
  // Time operations across the whole session (all tracks' clips, automation and markers): open a gap of `len` frames at `at`, or remove [a, b) and close it.
  const std::vector<Marker>& markerList() const { return markers_; }
  bool insertTime(std::int64_t at, std::int64_t len);
  bool cutTime(std::int64_t a, std::int64_t b);
  // Spot: moves a clip so it starts exactly at `newStart` (frames).
  bool spotClip(session::TrackId t, int clipId, std::int64_t newStart);
  // Loads the selected timeline clip (the part it plays) as the sampler's sample, replacing the current one.
  bool loadSelectedClipIntoSampler(instruments::Sampler* s);
  // Records the audio input into the sampler on `samplerTrack`; stop it (the button, or by stopping the transport) and the take becomes the sample.
  bool startSamplerRecording(session::TrackId samplerTrack);
  void stopSamplerRecording();
  bool samplerRecording() const { return sampRecActive_; }
  // Time-stretches the sampler's sample from `sourceBpm` to the project tempo (offline, pitch unchanged). The patch still refers to the original file.
  bool sampleFollowTempo(instruments::Sampler* s, double sourceBpm);
  // Captures a keyboard note into the selected MIDI part while the transport records. False when not recording or no part is selected.
  bool recordMidiNote(bool down, int pitch, int velocity);
  void setTrackHidden(session::TrackId t, bool hidden) { if (hidden) hidden_.insert(t); else hidden_.erase(t); }
  bool trackPinned(session::TrackId t) const { return pinned_.count(t) != 0; }
  void togglePinned(session::TrackId t) { if (!pinned_.erase(t)) pinned_.insert(t); }
  bool trackHidden(session::TrackId t) const { return hidden_.count(t) != 0; }
  void nudgeSelectedClip(int direction);              // by the nudge value in the toolbar
  void storeZoomPreset(int slot);                     // 0..4
  bool recallZoomPreset(int slot);
  const std::string& sessionName() const { return sessionName_; }

  // ---- undo / redo (every clip edit, including mouse drags, becomes one step) ----
  bool undoEdit();
  bool redoEdit();
  const edit::UndoStack& undoStack() const { return undo_; }
  void stripSilenceSelected();
  // ---- memory locations window and the local assistant ----
  void showMemory(bool on) { showMemory_ = on; }
  void showClips(bool on) { showClips_ = on; }
  void showVoiceBank(bool on) { showVoiceBank_ = on; }
  void showPluginParams(bool on) { showPlugParams_ = on; }
  void setEditorHost(EditorHost* h) { editorHost_ = h; }
  void openPluginEditor(int track, int insert) { openEditorWindow(track, insert); }
  std::size_t openEditorCount() const { return editors_.size(); }
  void selectPluginParam(int track, int insert, int param) { ppTrack_ = track; ppInsert_ = insert; ppParam_ = param; }
  void showPianoRoll(bool on) { showPianoRoll_ = on; }
  void showScore(bool on) { showScore_ = on; }

  // ---- MIDI parts: note lists edited in the piano roll and the score, saved next to the session ----
  // `liveTrack` names the track whose instruments play this part live from the sequencer (empty = the
  // part only plays when rendered). `liveOwned` marks a track Wezt made for it (it follows the part's instrument).
  struct MidiPart { std::string name, instrument = "keys"; music::MidiClip clip; music::RealTime rt; std::string liveTrack; bool liveOwned = false; std::uint64_t liveHash = 0; };
  const std::vector<MidiPart>& midiParts() const { return midiParts_; }
  int selectedMidiPart() const { return selPart_; }
  void selectMidiPart(int i) {
    if (i < -1 || i >= static_cast<int>(midiParts_.size())) return;
    selPart_ = i;
    roll_.load(i >= 0 ? midiParts_[static_cast<std::size_t>(i)].clip : music::MidiClip{});
    int hi = 0, lo = 127;                       // show the part's range in the piano roll
    for (std::size_t k = 0; k < roll_.size(); ++k) { hi = std::max(hi, roll_.note(k).pitch); lo = std::min(lo, roll_.note(k).pitch); }
    rollTopPitch_ = roll_.size() ? std::min(127, (hi + lo) / 2 + 14) : 84;
    rollScrollBeat_ = 0.f;
  }
  int newMidiPart(const std::string& name, const std::string& instrument = "keys");
  int importMidiFile(const std::string& path, std::string* error = nullptr);    // adds one part per track with notes
  bool exportMidiPart(int index, const std::string& path);
  bool removeMidiPart(int index);
  // Applies `f` to the selected part's notes as one undo step.
  bool editMidiPart(int index, const std::string& label, const std::function<void(edit::PianoRoll&)>& f);
  // Plays the part with the built-in instrument and puts the result on a new audio track.
  bool renderMidiPart(int index);
  // Live MIDI: the part plays through the instruments on `trackName` (a track with a Wezt Synth, the
  // sampler, or any instrument plug-in) in step with the transport. Empty name = stop playing live.
  bool setMidiPartTarget(int index, const std::string& trackName);
  // Makes a track with a Wezt Synth for the part (named "<part> (live)") and plays the part on it.
  bool playMidiPartLive(int index);
  // Names of the tracks that have an instrument and so can play a part.
  std::vector<std::string> midiTargetTracks() const;
  // Click-a-key preview: starts/stops a note on the selected part's track (making a synth track if the part has none).
  bool previewNote(int pitch, bool down, int velocity = 100);
  int previewingKey() const { return previewKey_; }   // the key sounding from a click or preview, or -1
  void syncLiveMidi();
  void watchAudioDevice();   // the sound device was unplugged or taken over: say so, and reconnect when it is back
  bool openHardwareMidi(std::size_t index);   // a connected MIDI keyboard plays the selected part's instrument
  void closeHardwareMidi();
  std::string hardwareMidiName() const { return hwMidi_ ? hwMidi_->name() : std::string(); }
  void pollHardwareMidi();
  // Sings the lyrics over the part's notes (formant synthesizer, no trained model) onto a new audio track.
  bool singMidiPart(int index, const std::string& lyrics, const std::string& voice, int choirVoices, std::string* message = nullptr, double humanize = 0.7, double room = 0.15, const std::string& character = {}, const std::string& bank = {});
  edit::PianoRoll& pianoRoll() { return roll_; }
  bool saveMidiParts(const std::string& sessionPath) const;     // writes <session>.midi (removes it when there are no parts)
  bool loadMidiParts(const std::string& sessionPath);
  // One row per clip of every track's live playlist, in timeline order. Used by the Clips window.
  struct ClipRow { session::TrackId track; int clip; std::string trackName, name; std::int64_t start, length; float gainDb; bool muted, locked; };
  std::vector<ClipRow> clipRows() const;
  // Clips window actions (also what the buttons call)
  bool renameClip(session::TrackId t, int clip, const std::string& name);
  bool deleteClip(session::TrackId t, int clip);
  bool muteClip(session::TrackId t, int clip, bool on);
  bool setClipGainDb(session::TrackId t, int clip, float db);
  bool locateClip(session::TrackId t, int clip);
  void showAssistant(bool on) { showAssistant_ = on; }
  void setAssistantDir(const std::string& dir);          // default: ~/.wezt
  ai::Assistant& assistant();
  std::string assistantSay(const std::string& text);     // what typing in the panel does

 private:
  // frame pieces
  void handleKeys();
  void drawMenu();
  void drawTransport();
  void drawEdit();
  void drawMix();
  void drawSamplerWindow();
  void drawImmersiveWindow();
  void drawFileDialog();
  void drawStatus();
  void drawTitle();
  void drawOverview();
  void drawSidebar(float height);
  void drawBottomTabs();
  void drawAssistantWindow();
  void drawMemoryWindow();
  void drawClipsWindow();
  void drawPianoRollWindow();
  void drawVoiceBankWindow();
  void drawPluginParamsWindow();
  void drawPluginBrowser();
  void openEditorWindow(int track, int insert);
  void serviceEditors();   // each frame: resize, and close what the user closed or what no longer exists
  std::string voiceDir() const;
  void drawScoreWindow();
  void watchEdits();
  // MIDI recording: while the transport records, notes from the keyboard are captured into the selected MIDI part (beats from the timeline
  // start, so the part plays them back in step) and committed as one undo step when recording stops.
  void commitRecordedMidi();
  void commitRecordedTakes();   // recorded audio becomes clips once the transport has stopped
  bool wasRecording_ = false, takesPending_ = false;
  int midiRecPart_ = -1; std::map<int, std::pair<double, int>> midiHeld_; std::vector<music::Note> midiTaken_; bool midiWasRecording_ = false;
  bool sessionCommandFromText(const std::string& text, std::string& reply);
  template <class F> void forGroup(session::TrackId t, F f);
  std::int64_t nudgeFrames() const;

  // helpers
  std::int64_t position() const;
  std::int64_t gridFrames() const;
  std::int64_t snap(std::int64_t f) const;
  std::int64_t endOfSession() const;
  const PeakCache& peaksFor(const std::shared_ptr<const AudioBuffer>& a);
  void addTrackQuick(bool mono);
  void splitSelectedClip();
  void duplicateSelectedClip();
  void copySelectedClip(bool cut);
  void pasteClip();
  void openSpot();
  void drawTimeOpsWindow();
  bool showTimeOps_ = false; double timeOpsLen_ = 1.0;
  void drawSpecialPasteWindow();
  void drawSpotWindow();
  bool showSpecialPaste_ = false, showSpot_ = false;
  int spotClip_ = -1, specialRepeats_ = 2; session::TrackId spotTrack_ = 0; double spotSeconds_ = 0.0, specialGap_ = 0.0; bool specialMerge_ = false;
  // The clips last copied or cut, with starts relative to the copied range (or 0 for a single clip). They share the audio data,
  // so nothing is duplicated in memory. clipboardLen_ is the length of the copied span.
  std::vector<session::Clip> clipboard_;
  std::int64_t clipboardLen_ = 0;
  void deleteSelectedClip();
  void fadeSelectedClip();
  void muteSelectedClip();
  void drawBigCounter();
  void drawClipFxWindow();
  bool showClipFx_ = false;
  float clipFxGainDb_ = 0.f, clipFxStretchPct_ = 100.f, clipFxSrcBpm_ = 120.f, clipFxSemis_ = 0.f; int clipFxMode_ = 0;
  float sampSrcBpm_ = 120.f;
  void groupTracksCommand();
  TimelineGeometry geo_;
  bool online_ = false;   // follow MIDI transport messages from the connected MIDI port
  bool showBigCounter_ = false, snapToGrid_ = false;
  std::int64_t prevSelStart_ = 0, prevSelEnd_ = 0, lastSelStart_ = 0, lastSelEnd_ = 0;
  void trackSelectionHistory();
  void trimSelectedClip(int how);   // 0 = to the selected range, 1 = start to the playhead, 2 = end to the playhead
  void insertSilenceAtSelection();
  void repeatSelection();
  void healSelectedClip();
  bool zoomedToSession_ = false;
  float prevFramesPerPixel_ = 0; double prevScroll_ = 0;
  void zoomTo(std::int64_t a, std::int64_t b);
  float meterLevel(const void* key, float peak);
  void openFileAction(const char* what);

  std::unique_ptr<session::Session> session_;
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<audio::AudioDevice> device_;
  std::unique_ptr<audio::NullDevice> nullDevice_;
  Keymap keymap_;
  std::string status_ = "Ready";
  std::size_t quarantineShown_ = 0;   // how many muted plug-ins the user has already been told about
  void watchPluginFaults();
  bool quit_ = false, deviceRunning_ = false, deviceLost_ = false;
  std::string defaultOutId_, defaultInId_;   // the system defaults the open stream was made for
  double defaultCheckAt_ = 0;
  double deviceRetryAt_ = 0.0;

  Tool tool_ = Tool::Selector;
  EditMode mode_ = EditMode::Slip;
  MainView view_ = MainView::Edit;
  session::TrackId selTrack_ = 0;
  int selClip_ = -1;
  std::int64_t selStart_ = 0, selEnd_ = 0;
  float framesPerPixel_ = 200.f;
  double scrollFrames_ = 0.0;
  float tempo_ = 120.f;
  int beatsPerBar_ = 4;
  const void* tempoSession_ = nullptr;   // the session whose tempo/beats the two fields above were last synced with
  bool followPlayhead_ = true, showTransportBar_ = true, metronome_ = false;
  bool dragging_ = false;
  float dragLaneTop_ = 0, dragLaneH_ = 1;   // the lane under a pencil drag
  std::int64_t pencilLast_ = -1;
  int dragKind_ = 0;               // 0 none, 1 move clip, 2 trim start, 3 trim end, 4 select range
  std::int64_t dragOrigin_ = 0, dragClipStart_ = 0, dragClipOffset_ = 0, dragClipLength_ = 0;
  bool timecode_ = true;
  std::vector<Marker> markers_;
  std::vector<TrackGroup> groups_;
  bool allGroupActive_ = false;
  std::set<session::TrackId> hidden_, pinned_;
  int firstTrack_ = 0, trackLevel_ = 1, gridIdx_ = 2, nudgeIdx_ = 3;
  float waveScale_ = 1.f, viewWidthPx_ = 1000.f;
  bool showWaves_ = true, showMixFloat_ = false, showVolAuto_ = false; int autoLane_ = 0;   // autoLane_: 0 = volume, 1 = pan   // showVolAuto_: draw/edit volume automation on the timeline
  int volDrag_ = -1; int volDragTrack_ = -1;
  struct ZoomPreset { bool set = false; float fpp = 200.f; double scroll = 0.0; } zoomPresets_[5];
  std::string sessionName_ = "Untitled Session";

  std::unordered_map<const AudioBuffer*, PeakCache> peaks_;
  std::unordered_map<const void*, float> meters_;
  std::unordered_map<const session::Strip*, std::array<float, 2>> levels_;  // smoothed peak per strip, refreshed once per frame
  void updateMeters();

  // windows
  bool showSampler_ = false, showImmersive_ = false, showKeymap_ = false, showNewTrack_ = false;
  std::string newTrackName_ = "Audio";
  bool newTrackMono_ = true;
  std::string dialogKind_, dialogPath_;
  std::string dialogMessage_;
  bool dialogOpen_ = false;

  // undo, memory, assistant
  edit::UndoStack undo_;
  std::unique_ptr<agent::PluginAgent> pluginAgent_;
  const session::Session* pluginAgentFor_ = nullptr;
  bool pluginCommandFromText(const std::string& low, std::string& reply);
  std::map<session::TrackId, std::vector<session::Clip>> snap_;
  bool snapInit_ = false;
  Prefs prefs_;
  bool showSettings_ = false, introReplay_ = false, browsing_ = false;
  std::string browseDir_, introSoundMsg_, introPathEdit_;
  std::unique_ptr<IntroPlayer> introPreview_;
  void drawSettings();
  bool showMemory_ = false, showAssistant_ = false, showClips_ = false, showPianoRoll_ = false, showVoiceBank_ = false, showPlugParams_ = false, showScore_ = false;
  std::vector<MidiPart> midiParts_;
  int selPart_ = -1;
  edit::PianoRoll roll_;
  // piano roll view and drag state
  float rollPxPerBeat_ = 64.f, rollRowH_ = 14.f, rollScrollBeat_ = 0.f; int rollTopPitch_ = 96, rollGridIdx_ = 2; bool rollDraw_ = true;
  int rollDrag_ = 0; double rollDragBeat_ = 0; int rollDragPitch_ = 0; edit::PianoRoll rollStart_; float rollBandX_ = 0, rollBandY_ = 0;
  struct OpenEditor { int window; plugins::IPluginInstance* instance; std::string title; };
  std::vector<OpenEditor> editors_;
  EditorHost* editorHost_ = nullptr;
  bool showPluginBrowser_ = false, pbScanned_ = false, pbSafe_ = false;   // the plug-in browser window
  std::vector<plugins::LibraryEntry> pbEntries_;
  std::string pbFilter_, pbMessage_;
  int ppTrack_ = -1, ppInsert_ = 0, ppParam_ = -1, ppDrag_ = -1; bool ppWrite_ = false, ppWasPlaying_ = false;   // Plugin Parameters window
  int rollChar_ = 0; std::string rollBank_;   // voice character (0 = none) and voice bank name (empty = synthesizer)
  std::string bankName_ = "my voice", bankStatus_; int bankTrack_[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1}; int bankCons_[22] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; bool bankConsent_ = false;
  std::string rollLyrics_; int rollVoice_ = 2; int rollChoir_ = 1; float rollHuman_ = 0.7f, rollRoom_ = 0.15f;
  std::unique_ptr<midi::IMidiOutput> hwOut_; std::int64_t outLastPos_ = 0; bool outWasPlaying_ = false, outSounding_ = false;
  struct OutSeq { std::shared_ptr<const session::MidiSequence> seq; std::uint64_t hash = 0; };
  std::vector<OutSeq> outSeq_;
  void pumpMidiOut();
  void finishSamplerRecording();
  int sampRecWait_ = 0;
  bool sampRecActive_ = false; session::TrackId sampRecTrack_ = 0, sampRecOwner_ = 0; std::vector<session::TrackId> sampRecAside_;
  void midiOutAllOff();
  std::unique_ptr<midi::IMidiInput> hwMidi_; std::string hwTrack_; bool hwTried_ = false; double hwActivityAt_ = -1.0;
  int previewKey_ = -1; double previewOffAt_ = -1; std::string previewTrack_; float lastLiveTempo_ = 0.f;
  bool addAudioTrack(const std::string& name, const AudioBuffer& audio);
  int scoreClef_ = -1; int scoreBeatsPerBar_ = 4; int scoreKeyRoot_ = 0, scoreKeyMode_ = 0;
  std::unique_ptr<ai::Assistant> assistant_;
  std::string assistantDir_;
  std::vector<std::pair<bool, std::string>> chat_;   // {fromUser, text}
  std::string chatInput_, playlistInput_;
  bool chatScroll_ = false;
  int llmPort_ = 8080;
  bool llmOn_ = false;

  // sampler editor
  int selZone_ = 0;
  std::string samplerPath_;
  int sliceCount_ = 8;
  float keyboardVelocity_ = 100.f;
  int heldKey_ = -1;
  int lfoTarget_[2] = {0, 1};
  int lfoTab_ = 0, sliceMode_ = 0, startKey_ = 48, snapMode_ = 1, dragMarker_ = -1;
  float sliceSens_ = 0.8f, zoomH_ = 1.f, zoomV_ = 1.f, viewStart_ = 0.f;
  bool chromatic_ = true, playToEnd_ = true, showZones_ = false;
  std::shared_ptr<const AudioBuffer> sliceSource_;
  double sliceRate_ = 48000.0;
  std::vector<std::int64_t> snapCuts_;
  const AudioBuffer* snapFor_ = nullptr;
  void samplerReslice(instruments::Sampler* s);
  void samplerRemapKeys(instruments::Sampler* s);
  void samplerSetMode(instruments::Sampler* s, instruments::SamplerMode m);
  void drawZoneWindow(instruments::Sampler* s);

  // immersive
  immersive::SpeakerLayout layout_ = immersive::makeLayout("7.1.4");
  std::string layoutName_ = "7.1.4";
  std::unique_ptr<immersive::ObjectRenderer> panner_;
  std::map<session::TrackId, ObjectPos> objects_;
  std::string admPath_ = "mix_adm.wav";
  std::string loudnessText_;
};

}  // namespace wezt::ui
