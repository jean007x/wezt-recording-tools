#pragma once
// A session: tracks with inserts and sends, buses, a master, all wired into one Graph, plus
// save/load and offline bounce. Edit the structure (add/remove/route) while audio is stopped;
// fader, pan, mute, solo, send level and arming can change any time.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "edit/memory_locations.h"
#include "edit/undo.h"
#include "engine/engine.h"
#include "engine/graph.h"
#include "io/wav.h"
#include "plugins/plugin_host.h"
#include "session/strip.h"
#include "session/track_source.h"

namespace wezt::session {

using TrackId = int;
using BusId = int;
constexpr BusId kMaster = 0;

// Builds a plugin from its saved descriptor (the app wires this to the plugin hosts). Returning
// null means "not available": the session keeps a bypassed placeholder with the saved state.
using InsertFactory = std::function<std::unique_ptr<plugins::IPluginInstance>(const plugins::PluginDescriptor&)>;

struct ParamLane { unsigned id = 0; AutomationLane lane; };   // values are the plugin's normalized 0..1

struct InsertSlot {
  Graph::NodeId node = 0;
  std::vector<ParamLane> paramLanes;   // automation of this plugin's parameters
  plugins::PluginDescriptor descriptor;
  plugins::IPluginInstance* instance = nullptr;  // null while offline
  std::vector<std::uint8_t> offlineState;
};

struct SendSlot {
  BusId bus = kMaster;
  Graph::NodeId node = 0;
  Gain* gain = nullptr;
  float levelDb = 0.f;
  bool preFader = false;
  bool muted = false;
};

struct Track {
  std::string name;
  bool mono = false;
  Graph::NodeId sourceNode = 0, stripNode = 0;
  TrackSource* source = nullptr;
  Strip* strip = nullptr;
  std::vector<InsertSlot> inserts;
  std::vector<SendSlot> sends;
  BusId output = kMaster;
  bool solo = false;
  int nextClipId = 1;
  // Playlists (alternate takes): the clips of the other playlists, kept here while one is live.
  std::string playlistName = "Main";
  std::vector<std::pair<std::string, std::vector<Clip>>> otherPlaylists;
};

struct Bus {
  std::string name;
  Graph::NodeId node = 0;
  Strip* strip = nullptr;
  BusId output = kMaster;  // buses other than the master feed another bus
};

// A VCA master: moves the level of its member tracks without passing audio itself. The VCA's gain
// (dB) is added to each member's fader, after fader automation.
struct Vca {
  std::string name;
  float gainDb = 0.f;
  std::vector<TrackId> members;
};
using VcaId = int;

struct BounceResult {
  bool ok = false;
  std::int64_t frames = 0;
  float peak = 0.f;
  std::string error;
};

class Session {
 public:
  Session(double sampleRate = 48000.0, std::size_t channels = 2, std::size_t maxBlock = 512);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  Graph& graph() { return graph_; }
  double sampleRate() const { return rate_; }
  std::size_t channels() const { return channels_; }
  std::size_t maxBlock() const { return maxBlock_; }
  // Give the processors the engine's context. Call after constructing the Engine on graph().
  void attach(const RtContext* ctx);

  // ---- structure ----
  TrackId addTrack(const std::string& name, bool mono = false, std::vector<unsigned> inputs = {0});
  bool removeTrack(TrackId t);
  // Copies a track with everything on it: clips (the audio is shared, not copied), fader, pan,
  // mute, automation, output, sends and inserts. Inserts are rebuilt through `factory` and given
  // the original's settings; without a factory (or if it fails) they become bypassed placeholders
  // that keep the settings. Not armed. Returns the new id, or -1.
  TrackId duplicateTrack(TrackId t, const InsertFactory& factory = {});
  BusId addBus(const std::string& name);
  bool removeBus(BusId b);
  bool setTrackOutput(TrackId t, BusId b);
  bool setBusOutput(BusId b, BusId target);  // false if it would make a loop
  Track* track(TrackId t);
  Bus* bus(BusId b);
  std::size_t trackCount() const;            // live tracks
  std::vector<TrackId> trackIds() const;
  std::vector<BusId> busIds() const;

  // ---- VCA masters ----
  VcaId addVca(const std::string& name, std::vector<TrackId> members = {});
  bool removeVca(VcaId v);
  bool setVcaGain(VcaId v, float db);
  bool setVcaMembers(VcaId v, std::vector<TrackId> members);
  Vca* vca(VcaId v);
  std::vector<VcaId> vcaIds() const;

  // ---- tempo and beats per bar (saved with the session; the UI keeps them in step, the click and the ruler use them) ----
  double tempo() const { return tempo_; }
  int beatsPerBar() const { return beatsPerBar_; }
  void setTempo(double bpm) { tempo_ = bpm < 20.0 ? 20.0 : (bpm > 400.0 ? 400.0 : bpm); }
  void setBeatsPerBar(int n) { beatsPerBar_ = n < 1 ? 1 : (n > 16 ? 16 : n); }

  // ---- memory locations and markers (saved with the session) ----
  edit::MemoryLocations& memory() { return memory_; }
  const edit::MemoryLocations& memory() const { return memory_; }

  // ---- inserts and sends ----
  // Adds a plugin at `position` in the track's insert chain (-1 = end). Plugin latency is
  // compensated automatically against the other tracks.
  // Plug-ins can announce a latency change at any time. pollPluginLatency() (control thread, cheap, any time) collects the
  // announcements and returns true while a change is waiting. applyPluginLatency() then re-computes the delay compensation;
  // call it only while the audio is stopped (the compensation delays are re-sized). Returns true when it changed something.
  bool pollPluginLatency();
  bool applyPluginLatency();
  // A plug-in that keeps producing NaN/inf is muted by the graph (see Graph::quarantined). These let the UI say so and let the
  // user switch it back on (e.g. after changing the offending setting). Control thread.
  struct PluginFault { TrackId track; std::size_t insert; std::string name; bool quarantined; std::uint64_t badBlocks; };
  std::vector<PluginFault> pluginFaults() const;      // inserts that produced bad samples at least once
  bool reenablePlugin(TrackId t, std::size_t insert); // clears the quarantine
  bool addInsert(TrackId t, std::unique_ptr<plugins::IPluginInstance> plugin, const plugins::PluginDescriptor& d, int position = -1);
  bool removeInsert(TrackId t, std::size_t index);
  // A track's volume (dB), pan (-1..1) or mute (0/1) lane: which = 0, 1, 2. Replaces the lane. Edit only while the transport is stopped
  // (the audio thread reads these lanes while playing). The Undoable form records a step that puts the old points back.
  bool setStripLane(TrackId t, int which, const std::vector<AutomationPoint>& points);
  bool setStripLaneUndoable(edit::UndoStack& undo, TrackId t, int which, const std::vector<AutomationPoint>& points, const std::string& label, const std::string& mergeKey = {});
  // Plugin parameter automation. Points are (frame, normalized value 0..1). Replaces the lane; no points removes it.
  // False when the track, insert or parameter does not exist or the plugin is offline.
  bool setParamAutomation(TrackId t, std::size_t insert, unsigned paramId, const std::vector<AutomationPoint>& points);
  const AutomationLane* paramAutomation(TrackId t, std::size_t insert, unsigned paramId) const;
  // Same as setParamAutomation, and records an undo step that puts the lane back as it was (or removes it again).
  // Steps with the same non-empty mergeKey in a row become one (a whole recording pass undoes at once).
  // The step refers to this Session by address: keep the Session alive as long as the UndoStack, or clear the stack first.
  bool setParamAutomationUndoable(edit::UndoStack& undo, TrackId t, std::size_t insert, unsigned paramId, const std::vector<AutomationPoint>& points, const std::string& label, const std::string& mergeKey = {});
  void publishParamAutomation(TrackId t);   // rebuilds what the audio thread reads (called after any change)
  int addSend(TrackId t, BusId bus, float levelDb = 0.f, bool preFader = false);
  bool setSendLevel(TrackId t, std::size_t index, float levelDb);
  bool setSendMute(TrackId t, std::size_t index, bool mute);
  bool removeSend(TrackId t, std::size_t index);

  // ---- mixing ----
  void setSolo(TrackId t, bool solo);
  bool anySolo() const;

  // ---- playlists (alternate takes) ----
  // A track has one live playlist (its clips play) and any number of others. Names are unique.
  std::vector<std::string> playlistNames(TrackId t) const;     // the live one first
  std::string livePlaylist(TrackId t) const;
  bool newPlaylist(TrackId t, const std::string& name, bool copyLive);   // becomes live
  bool switchPlaylist(TrackId t, const std::string& name);
  bool renamePlaylist(TrackId t, const std::string& from, const std::string& to);
  bool deletePlaylist(TrackId t, const std::string& name);              // not the live one
  // Comping: copies the part of another playlist between `from` and `to` (timeline frames) into the live one,
  // replacing what the live playlist has there.
  bool compFromPlaylist(TrackId t, const std::string& name, std::int64_t from, std::int64_t to);

  // ---- clips and recording ----
  // Loads a WAV into memory and puts it on the track at `start`. Returns the clip id, or -1.
  int addClipFromFile(TrackId t, const std::string& wavPath, std::int64_t start, std::string* error = nullptr);
  int addClip(TrackId t, Clip c);
  bool removeClip(TrackId t, int clipId);
  // After recording stopped: waits for the disks, then turns every finished take into a clip.
  // Returns the number of takes added.
  int commitTakes(std::string* error = nullptr);
  void setRecordFolder(const std::string& dir);  // default: <session folder>/Audio Files

  // ---- clip effects (offline, destructive on a copy) ----
  // Runs a clip's audio (the part it plays: offset..offset+length) through `fn`, writes the result as a new WAV in the audio folder and
  // points the clip at it (offset 0, same start and length; fades, gain and gain line stay). `fn` edits the buffer in place and may
  // return false with a message to refuse. Returns true on success.
  bool processClip(TrackId t, int clipId, const std::string& tag, const std::function<bool(AudioBuffer&, double sampleRate, std::string*)>& fn, std::string* error = nullptr, bool followLength = false);
  // followLength: the clip takes the length of what `fn` returns (time-stretch); otherwise it keeps its length.

  // ---- rendering a track ----
  std::string recordFolder() const { return recordFolderFor(); }
  // Consolidate: mixes the clips of a track that lie in [a, b) (gain, fades, gain line; muted clips are silent; no inserts or fader) into
  // one new WAV file in the audio folder and puts one clip in their place. Returns the clip id, or -1 with `error`.
  int consolidate(TrackId t, std::int64_t a, std::int64_t b, std::string* error = nullptr);
  // Renders one track alone, as it sounds (inserts, fader, pan, sends to the master bus and the master itself), over [a, b) plus a
  // tail, and puts the result on a new track. With hideSource the original track is muted. The audio device must not be running.
  // Returns the new track's id, or -1 with `error`.
  TrackId renderTrackToNewTrack(TrackId t, std::int64_t a, std::int64_t b, bool muteSource, std::string* error = nullptr);

  // ---- offline render ----
  // Renders [start, end) plus `tailFrames` (reverb/delay tails) of the whole mix to a WAV file,
  // faster than real time. The graph is re-prepared; do not call while the device runs.
  BounceResult bounce(const std::string& path, std::int64_t start, std::int64_t end, std::int64_t tailFrames = 0,
                      io::WavSampleType type = io::WavSampleType::Float32);

  // ---- save / load ----
  bool save(const std::string& path, std::string* error = nullptr);
  // Missing audio or plugins do not fail the load: they are listed in warnings() and play as
  // silence / bypass, and are kept so a later save does not lose them.
  static std::unique_ptr<Session> load(const std::string& path, const InsertFactory& factory, std::string* error);
  const std::vector<std::string>& warnings() const { return warnings_; }

 private:
  bool rewire();
  void unlinkAll();
  bool link(Graph::NodeId a, Graph::NodeId b);
  void updateSolo();
  void applyVca();
  std::string recordFolderFor() const;

  Graph graph_;
  double rate_;
  std::size_t channels_, maxBlock_;
  const RtContext* ctx_ = nullptr;
  std::vector<std::unique_ptr<Track>> tracks_;
  bool latencyDirty_ = false;
  std::vector<std::unique_ptr<Bus>> buses_;
  std::vector<std::unique_ptr<Vca>> vcas_;
  edit::MemoryLocations memory_;
  double tempo_ = 120.0;
  int beatsPerBar_ = 4;
  std::vector<std::pair<Graph::NodeId, Graph::NodeId>> edges_;  // everything we connected, so rewiring can undo it
  std::string recordDir_, sessionPath_;
  std::vector<std::string> warnings_;
};

}  // namespace wezt::session
