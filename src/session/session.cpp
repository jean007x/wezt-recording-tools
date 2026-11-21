#include "session/session.h"
#include "session/automated_effect.h"
#include "edit/playlists.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace wezt::session {
namespace fs = std::filesystem;

namespace {

// ---------- text encoding for the session file ----------
std::string enc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c <= 0x20 || c == '%' || c == '=' || c >= 0x7f) { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o.empty() ? "%" : o;  // a lone % stands for the empty string
}
std::string dec(const std::string& s) {
  if (s == "%") return {};
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      o += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else o += s[i];
  }
  return o;
}
std::string hex(const std::vector<std::uint8_t>& v) {
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(v.size() * 2 + 1);
  for (auto b : v) { o += d[b >> 4]; o += d[b & 15]; }
  return o.empty() ? "-" : o;
}
bool unhex(const std::string& s, std::vector<std::uint8_t>& v) {
  v.clear();
  if (s == "-") return true;
  if (s.size() % 2) return false;
  auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (std::size_t i = 0; i < s.size(); i += 2) {
    const int a = nib(s[i]), b = nib(s[i + 1]);
    if (a < 0 || b < 0) return false;
    v.push_back(static_cast<std::uint8_t>(a * 16 + b));
  }
  return true;
}
std::string fnum(double v) { char b[40]; std::snprintf(b, sizeof b, "%.9g", v); return b; }

using KV = std::map<std::string, std::string>;
struct Line { std::string kind; KV kv; std::vector<std::string> bare; };
Line parseLine(const std::string& l) {
  Line r;
  std::istringstream is(l);
  std::string tok;
  is >> r.kind;
  while (is >> tok) {
    const auto eq = tok.find('=');
    if (eq == std::string::npos) r.bare.push_back(tok);
    else r.kv[tok.substr(0, eq)] = tok.substr(eq + 1);
  }
  return r;
}
double num(const KV& kv, const char* k, double def) {
  auto it = kv.find(k);
  if (it == kv.end()) return def;
  char* end = nullptr;
  const double v = std::strtod(it->second.c_str(), &end);
  return end == it->second.c_str() || !std::isfinite(v) ? def : v;
}
std::string str(const KV& kv, const char* k) { auto it = kv.find(k); return it == kv.end() ? std::string() : dec(it->second); }

std::string safeName(std::string n) {
  for (char& c : n) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ' || c == '.')) c = '_';
  return n.empty() ? "Track" : n;
}

bool loadWav(const std::string& path, double rate, std::shared_ptr<const AudioBuffer>& out, std::int64_t* frames, std::string* err) {
  io::WavReader r;
  std::string e;
  if (!r.open(path, &e)) { if (err) *err = path + ": " + e; return false; }
  const auto& info = r.info();
  if (info.sampleRate != static_cast<unsigned>(rate)) {
    if (err) *err = path + ": sample rate " + std::to_string(info.sampleRate) + " differs from the session's " + std::to_string(static_cast<unsigned>(rate)) + " (convert it first)";
    return false;
  }
  if (info.frames * info.channels * 4ull > (2ull << 30)) { if (err) *err = path + ": too long to hold in memory (over 2 GiB)"; return false; }
  auto buf = std::make_shared<AudioBuffer>(info.channels, static_cast<std::size_t>(info.frames));
  std::vector<float*> p(info.channels);
  for (unsigned c = 0; c < info.channels; ++c) p[c] = buf->channel(c);
  const std::size_t got = r.read(0, static_cast<std::size_t>(info.frames), p.data());
  buf->setFrames(got);
  if (frames) *frames = static_cast<std::int64_t>(got);
  out = std::move(buf);
  return true;
}

}  // namespace

// ============================== structure ==============================

Session::Session(double sampleRate, std::size_t channels, std::size_t maxBlock) : rate_(sampleRate), channels_(channels), maxBlock_(maxBlock) {
  graph_.prepare(rate_, channels_, maxBlock_);
  auto b = std::make_unique<Bus>();
  b->name = "Master";
  auto strip = std::make_unique<Strip>(false);
  b->strip = strip.get();
  b->node = graph_.add(std::move(strip));
  graph_.setOutput(b->node);
  buses_.push_back(std::move(b));
}

void Session::attach(const RtContext* ctx) {
  ctx_ = ctx;
  for (auto& t : tracks_) if (t) { t->source->setContext(ctx); t->strip->setContext(ctx); }
  for (auto& b : buses_) if (b) b->strip->setContext(ctx);
}

Track* Session::track(TrackId t) { return t >= 0 && static_cast<std::size_t>(t) < tracks_.size() ? tracks_[static_cast<std::size_t>(t)].get() : nullptr; }
Bus* Session::bus(BusId b) { return b >= 0 && static_cast<std::size_t>(b) < buses_.size() ? buses_[static_cast<std::size_t>(b)].get() : nullptr; }
std::size_t Session::trackCount() const { return static_cast<std::size_t>(std::count_if(tracks_.begin(), tracks_.end(), [](auto& p) { return p != nullptr; })); }
std::vector<TrackId> Session::trackIds() const {
  std::vector<TrackId> v;
  for (std::size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i]) v.push_back(static_cast<TrackId>(i));
  return v;
}
std::vector<BusId> Session::busIds() const {
  std::vector<BusId> v;
  for (std::size_t i = 0; i < buses_.size(); ++i) if (buses_[i]) v.push_back(static_cast<BusId>(i));
  return v;
}

std::string Session::recordFolderFor() const {
  if (!recordDir_.empty()) return recordDir_;
  if (!sessionPath_.empty()) return (fs::path(sessionPath_).parent_path() / "Audio Files").string();
  return "Audio Files";
}
void Session::setRecordFolder(const std::string& dir) {
  recordDir_ = dir;
  for (auto& t : tracks_) if (t) t->source->setRecordFolder(recordFolderFor(), safeName(t->name));
}

TrackId Session::addTrack(const std::string& name, bool mono, std::vector<unsigned> inputs) {
  auto t = std::make_unique<Track>();
  t->name = name;
  t->mono = mono;
  auto src = std::make_unique<TrackSource>();
  auto strip = std::make_unique<Strip>(mono);
  t->source = src.get();
  t->strip = strip.get();
  src->setInputs(std::move(inputs));
  src->setRecordFolder(recordFolderFor(), safeName(name));
  src->setContext(ctx_);
  strip->setContext(ctx_);
  t->sourceNode = graph_.add(std::move(src));
  t->stripNode = graph_.add(std::move(strip));
  tracks_.push_back(std::move(t));
  rewire();
  return static_cast<TrackId>(tracks_.size() - 1);
}

TrackId Session::duplicateTrack(TrackId id, const InsertFactory& factory) {
  Track* src = track(id);
  if (!src) return -1;
  // Copy by value first: addTrack/rewire may not move tracks, but this keeps us safe from that.
  const std::string name = src->name + " dup";
  const TrackId nid = addTrack(name, src->mono, src->source->inputs());
  src = track(id);
  Track* dst = track(nid);
  if (!dst) return -1;
  dst->source->setMonitorMode(src->source->monitorMode());
  dst->source->setPlaylist(src->source->playlist());
  dst->nextClipId = src->nextClipId;
  dst->playlistName = src->playlistName;
  dst->otherPlaylists = src->otherPlaylists;
  Strip* a = src->strip; Strip* b = dst->strip;
  b->setGainDb(a->gainDb()); b->setPan(a->pan()); b->setMute(a->muted()); b->setAutomationOn(a->automationOn());
  auto copyLane = [](AutomationLane& to, const AutomationLane& from) { to.clear(); for (const auto& pt : from.points()) to.add(pt.frame, pt.value); };
  copyLane(b->volumeLane(), a->volumeLane());
  copyLane(b->panLane(), a->panLane());
  copyLane(b->muteLane(), a->muteLane());
  dst->output = src->output;
  for (std::size_t i = 0; i < src->inserts.size(); ++i) {
    const InsertSlot& in = src->inserts[i];
    const auto state = in.instance ? in.instance->saveState() : in.offlineState;
    std::unique_ptr<plugins::IPluginInstance> inst = factory ? factory(in.descriptor) : nullptr;
    const bool restored = inst && (state.empty() || inst->loadState(state));
    if (inst && !restored) warnings_.push_back("duplicate: plugin \"" + in.descriptor.name + "\" could not copy its settings");
    addInsert(nid, std::move(inst), in.descriptor);
    dst = track(nid);
    if (!dst->inserts.back().instance) dst->inserts.back().offlineState = state;
    if (dst->inserts.back().instance) for (const ParamLane& l : in.paramLanes) setParamAutomation(nid, dst->inserts.size() - 1, l.id, l.lane.points());
  }
  dst = track(nid);
  for (const SendSlot& s : src->sends) {
    const int k = addSend(nid, s.bus, s.levelDb, s.preFader);
    if (k >= 0 && s.muted) setSendMute(nid, static_cast<std::size_t>(k), true);
  }
  return nid;
}

BusId Session::addBus(const std::string& name) {
  auto b = std::make_unique<Bus>();
  b->name = name;
  auto strip = std::make_unique<Strip>(false);
  b->strip = strip.get();
  strip->setContext(ctx_);
  b->node = graph_.add(std::move(strip));
  buses_.push_back(std::move(b));
  rewire();
  return static_cast<BusId>(buses_.size() - 1);
}

bool Session::removeTrack(TrackId id) {
  Track* t = track(id);
  if (!t) return false;
  unlinkAll();
  for (auto& in : t->inserts) graph_.remove(in.node);
  for (auto& s : t->sends) graph_.remove(s.node);
  graph_.remove(t->sourceNode);
  graph_.remove(t->stripNode);
  tracks_[static_cast<std::size_t>(id)].reset();
  for (auto& v : vcas_) if (v) v->members.erase(std::remove(v->members.begin(), v->members.end(), id), v->members.end());
  applyVca();
  updateSolo();
  rewire();
  return true;
}

VcaId Session::addVca(const std::string& name, std::vector<TrackId> members) {
  auto v = std::make_unique<Vca>();
  v->name = name;
  for (TrackId t : members) if (track(t) && std::find(v->members.begin(), v->members.end(), t) == v->members.end()) v->members.push_back(t);
  vcas_.push_back(std::move(v));
  applyVca();
  return static_cast<VcaId>(vcas_.size() - 1);
}
bool Session::removeVca(VcaId id) {
  if (!vca(id)) return false;
  vcas_[static_cast<std::size_t>(id)].reset();
  applyVca();
  return true;
}
bool Session::setVcaGain(VcaId id, float db) {
  Vca* v = vca(id);
  if (!v) return false;
  v->gainDb = std::clamp(db, -100.f, 12.f);
  applyVca();
  return true;
}
bool Session::setVcaMembers(VcaId id, std::vector<TrackId> members) {
  Vca* v = vca(id);
  if (!v) return false;
  v->members.clear();
  for (TrackId t : members) if (track(t) && std::find(v->members.begin(), v->members.end(), t) == v->members.end()) v->members.push_back(t);
  applyVca();
  return true;
}
Vca* Session::vca(VcaId id) { return id >= 0 && static_cast<std::size_t>(id) < vcas_.size() ? vcas_[static_cast<std::size_t>(id)].get() : nullptr; }
std::vector<VcaId> Session::vcaIds() const {
  std::vector<VcaId> r;
  for (std::size_t i = 0; i < vcas_.size(); ++i) if (vcas_[i]) r.push_back(static_cast<VcaId>(i));
  return r;
}
void Session::applyVca() {
  for (auto& t : tracks_) if (t) {
    float db = 0.f;
    for (auto& v : vcas_) if (v && std::find(v->members.begin(), v->members.end(), static_cast<TrackId>(&t - tracks_.data())) != v->members.end()) db += v->gainDb;
    t->strip->setVcaDb(db);
  }
}

bool Session::removeBus(BusId id) {
  if (id == kMaster || !bus(id)) return false;
  unlinkAll();
  for (auto& t : tracks_) if (t) {
    if (t->output == id) t->output = kMaster;
    for (std::size_t i = t->sends.size(); i-- > 0;)
      if (t->sends[i].bus == id) { graph_.remove(t->sends[i].node); t->sends.erase(t->sends.begin() + static_cast<std::ptrdiff_t>(i)); }
  }
  for (auto& b : buses_) if (b && b->output == id) b->output = kMaster;
  graph_.remove(buses_[static_cast<std::size_t>(id)]->node);
  buses_[static_cast<std::size_t>(id)].reset();
  rewire();
  return true;
}

bool Session::setTrackOutput(TrackId t, BusId b) {
  Track* tr = track(t);
  if (!tr || !bus(b)) return false;
  const BusId old = tr->output;
  tr->output = b;
  if (!rewire()) { tr->output = old; rewire(); return false; }
  return true;
}

bool Session::setBusOutput(BusId id, BusId target) {
  Bus* b = bus(id);
  if (!b || id == kMaster || !bus(target) || id == target) return false;
  const BusId old = b->output;
  b->output = target;
  if (!rewire()) { b->output = old; rewire(); return false; }
  return true;
}

// ============================== wiring ==============================

void Session::unlinkAll() {
  for (auto& e : edges_) graph_.disconnect(e.first, e.second);
  edges_.clear();
}

bool Session::link(Graph::NodeId a, Graph::NodeId b) {
  if (!graph_.connect(a, b)) return false;
  edges_.emplace_back(a, b);
  return true;
}

// Rebuilds every connection from the structures above. Returns false (and leaves the graph
// without the offending connection) if something would make a loop.
bool Session::rewire() {
  unlinkAll();
  bool ok = true;
  for (auto& tp : tracks_) {
    if (!tp) continue;
    Track& t = *tp;
    Graph::NodeId prev = t.sourceNode;
    std::vector<plugins::IPluginInstance*> midiTargets;
    for (auto& in : t.inserts) {
      ok &= link(prev, in.node); prev = in.node;
      if (in.instance && in.instance->acceptsMidi()) midiTargets.push_back(in.instance);  // instruments on this track play its MIDI
    }
    if (t.source) t.source->setMidiTargets(std::move(midiTargets));
    publishParamAutomation(static_cast<TrackId>(&tp - &tracks_[0]));
    const Graph::NodeId preFader = prev;
    ok &= link(prev, t.stripNode);
    for (auto& s : t.sends) {
      Bus* b = bus(s.bus);
      if (!b) continue;
      ok &= link(s.preFader ? preFader : t.stripNode, s.node);
      ok &= link(s.node, b->node);
    }
    if (Bus* out = bus(t.output)) ok &= link(t.stripNode, out->node);
  }
  for (std::size_t i = 1; i < buses_.size(); ++i)
    if (buses_[i]) if (Bus* out = bus(buses_[i]->output)) ok &= link(buses_[i]->node, out->node);
  graph_.refreshLatency();
  return ok;
}

// ============================== inserts and sends ==============================

bool Session::pollPluginLatency() {
  for (auto& tp : tracks_)
    if (tp)
      for (auto& s : tp->inserts)
        if (s.instance && s.instance->takeLatencyChanged()) latencyDirty_ = true;
  return latencyDirty_;
}

std::vector<Session::PluginFault> Session::pluginFaults() const {
  std::vector<PluginFault> out;
  for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
    if (!tracks_[ti]) continue;
    const auto& ins = tracks_[ti]->inserts;
    for (std::size_t i = 0; i < ins.size(); ++i) {
      const auto bad = graph_.nonFiniteBlocks(ins[i].node);
      if (bad) out.push_back({static_cast<TrackId>(ti), i, ins[i].descriptor.name, graph_.quarantined(ins[i].node), bad});
    }
  }
  return out;
}

bool Session::reenablePlugin(TrackId id, std::size_t insert) {
  Track* t = track(id);
  if (!t || insert >= t->inserts.size()) return false;
  graph_.resetFault(t->inserts[insert].node);
  return true;
}

bool Session::applyPluginLatency() {
  if (!pollPluginLatency()) return false;
  latencyDirty_ = false;
  graph_.refreshLatency();
  return true;
}

bool Session::addInsert(TrackId id, std::unique_ptr<plugins::IPluginInstance> plugin, const plugins::PluginDescriptor& d, int position) {
  Track* t = track(id);
  if (!t) return false;
  InsertSlot s;
  s.descriptor = d;
  s.instance = plugin.get();
  if (plugin && plugin->supportsAutomation()) {   // effects and instruments: automation inside the block
    t->source->addSplitHost(plugin.get());
    s.node = graph_.add(std::make_unique<AutomatedEffect>(std::move(plugin), t->source));
  } else if (plugin) s.node = graph_.add(std::move(plugin));
  else s.node = graph_.add(std::make_unique<PassThrough>());
  const std::size_t at = position < 0 || static_cast<std::size_t>(position) > t->inserts.size() ? t->inserts.size() : static_cast<std::size_t>(position);
  t->inserts.insert(t->inserts.begin() + static_cast<std::ptrdiff_t>(at), std::move(s));
  rewire();
  return true;
}

bool Session::setParamAutomation(TrackId id, std::size_t insert, unsigned paramId, const std::vector<AutomationPoint>& points) {
  Track* t = track(id);
  if (!t || insert >= t->inserts.size() || !t->inserts[insert].instance) return false;
  auto& slot = t->inserts[insert];
  bool known = false;
  for (const auto& p : slot.instance->parameters()) if (p.id == paramId) known = true;
  if (!known) return false;
  for (const auto& p : points) if (!std::isfinite(p.value) || p.frame < 0) return false;
  slot.paramLanes.erase(std::remove_if(slot.paramLanes.begin(), slot.paramLanes.end(), [&](const ParamLane& l) { return l.id == paramId; }), slot.paramLanes.end());
  if (!points.empty()) {
    ParamLane l; l.id = paramId;
    for (const auto& p : points) l.lane.add(p.frame, std::clamp(p.value, 0.f, 1.f));
    slot.paramLanes.push_back(std::move(l));
  }
  publishParamAutomation(id);
  return true;
}

bool Session::setStripLane(TrackId id, int which, const std::vector<AutomationPoint>& points) {
  Track* t = track(id);
  if (!t || which < 0 || which > 2) return false;
  AutomationLane& l = which == 0 ? t->strip->volumeLane() : which == 1 ? t->strip->panLane() : t->strip->muteLane();
  l.clear();
  for (const auto& p : points) l.add(p.frame, which == 0 ? std::clamp(p.value, -90.f, 12.f) : which == 1 ? std::clamp(p.value, -1.f, 1.f) : (p.value >= 0.5f ? 1.f : 0.f));
  return true;
}

bool Session::setStripLaneUndoable(edit::UndoStack& undo, TrackId id, int which, const std::vector<AutomationPoint>& points, const std::string& label, const std::string& mergeKey) {
  Track* t = track(id);
  if (!t || which < 0 || which > 2) return false;
  const std::vector<AutomationPoint> before = (which == 0 ? t->strip->volumeLane() : which == 1 ? t->strip->panLane() : t->strip->muteLane()).points();
  if (!setStripLane(id, which, points)) return false;
  const std::vector<AutomationPoint> after = (which == 0 ? t->strip->volumeLane() : which == 1 ? t->strip->panLane() : t->strip->muteLane()).points();
  if (before == after) return true;
  undo.record({label, [this, id, which, before] { setStripLane(id, which, before); }, [this, id, which, after] { setStripLane(id, which, after); }, mergeKey});
  return true;
}

bool Session::setParamAutomationUndoable(edit::UndoStack& undo, TrackId id, std::size_t insert, unsigned paramId, const std::vector<AutomationPoint>& points, const std::string& label, const std::string& mergeKey) {
  std::vector<AutomationPoint> before; if (const AutomationLane* l = paramAutomation(id, insert, paramId)) before = l->points();
  if (!setParamAutomation(id, insert, paramId, points)) return false;
  if (before == points) return true;   // nothing changed: no step
  undo.record({label, [this, id, insert, paramId, before] { setParamAutomation(id, insert, paramId, before); }, [this, id, insert, paramId, points] { setParamAutomation(id, insert, paramId, points); }, mergeKey});
  return true;
}

const AutomationLane* Session::paramAutomation(TrackId id, std::size_t insert, unsigned paramId) const {
  const Track* t = id >= 0 && static_cast<std::size_t>(id) < tracks_.size() ? tracks_[static_cast<std::size_t>(id)].get() : nullptr;
  if (!t || insert >= t->inserts.size()) return nullptr;
  for (const auto& l : t->inserts[insert].paramLanes) if (l.id == paramId) return &l.lane;
  return nullptr;
}

void Session::publishParamAutomation(TrackId id) {
  Track* t = track(id);
  if (!t || !t->source) return;
  auto snap = std::make_shared<TrackSource::ParamAutomation>();
  for (auto& in : t->inserts) if (in.instance) for (auto& l : in.paramLanes) if (!l.lane.empty()) snap->lanes.push_back({in.instance, l.id, l.lane});
  t->source->setParamAutomation(std::move(snap));
}

bool Session::removeInsert(TrackId id, std::size_t index) {
  Track* t = track(id);
  if (!t || index >= t->inserts.size()) return false;
  unlinkAll();
  t->source->setParamAutomation(nullptr);   // before the plugin goes away
  if (t->inserts[index].instance) t->source->removeSplitHost(t->inserts[index].instance);
  graph_.remove(t->inserts[index].node);
  t->inserts.erase(t->inserts.begin() + static_cast<std::ptrdiff_t>(index));
  rewire();
  return true;
}

int Session::addSend(TrackId id, BusId b, float levelDb, bool pre) {
  Track* t = track(id);
  if (!t || !bus(b)) return -1;
  auto g = std::make_unique<Gain>(dbToLin(levelDb));
  SendSlot s;
  s.bus = b; s.gain = g.get(); s.levelDb = levelDb; s.preFader = pre;
  s.node = graph_.add(std::move(g));
  t->sends.push_back(s);
  if (!rewire()) {  // would loop (e.g. a bus sending to itself): undo
    unlinkAll();
    graph_.remove(t->sends.back().node);
    t->sends.pop_back();
    rewire();
    return -1;
  }
  return static_cast<int>(t->sends.size() - 1);
}

bool Session::setSendLevel(TrackId id, std::size_t i, float db) {
  Track* t = track(id);
  if (!t || i >= t->sends.size()) return false;
  t->sends[i].levelDb = db;
  t->sends[i].gain->setGain(t->sends[i].muted ? 0.f : dbToLin(db));
  return true;
}
bool Session::setSendMute(TrackId id, std::size_t i, bool m) {
  Track* t = track(id);
  if (!t || i >= t->sends.size()) return false;
  t->sends[i].muted = m;
  t->sends[i].gain->setGain(m ? 0.f : dbToLin(t->sends[i].levelDb));
  return true;
}
bool Session::removeSend(TrackId id, std::size_t i) {
  Track* t = track(id);
  if (!t || i >= t->sends.size()) return false;
  unlinkAll();
  graph_.remove(t->sends[i].node);
  t->sends.erase(t->sends.begin() + static_cast<std::ptrdiff_t>(i));
  rewire();
  return true;
}

// ============================== solo ==============================

bool Session::anySolo() const {
  for (auto& t : tracks_) if (t && t->solo) return true;
  return false;
}
void Session::updateSolo() {
  const bool any = anySolo();
  for (auto& t : tracks_) if (t) t->strip->setSilenced(any && !t->solo);
}
void Session::setSolo(TrackId id, bool solo) {
  if (Track* t = track(id)) { t->solo = solo; updateSolo(); }
}

// ============================== clips and takes ==============================

// ============================== playlists ==============================
std::vector<std::string> Session::playlistNames(TrackId id) const {
  const Track* t = id >= 0 && static_cast<std::size_t>(id) < tracks_.size() ? tracks_[static_cast<std::size_t>(id)].get() : nullptr;
  std::vector<std::string> n;
  if (!t) return n;
  n.push_back(t->playlistName);
  for (auto& o : t->otherPlaylists) n.push_back(o.first);
  return n;
}
std::string Session::livePlaylist(TrackId id) const {
  const Track* t = id >= 0 && static_cast<std::size_t>(id) < tracks_.size() ? tracks_[static_cast<std::size_t>(id)].get() : nullptr;
  return t ? t->playlistName : std::string();
}
bool Session::newPlaylist(TrackId id, const std::string& name, bool copyLive) {
  Track* t = track(id);
  if (!t || name.empty()) return false;
  for (auto& n : playlistNames(id)) if (n == name) return false;
  auto live = t->source->playlist();
  t->otherPlaylists.push_back({t->playlistName, live});
  t->playlistName = name;
  if (!copyLive) live.clear();
  t->source->setPlaylist(std::move(live));
  return true;
}
bool Session::switchPlaylist(TrackId id, const std::string& name) {
  Track* t = track(id);
  if (!t || name == t->playlistName) return t != nullptr && name == t->playlistName;
  auto it = std::find_if(t->otherPlaylists.begin(), t->otherPlaylists.end(), [&](const auto& x) { return x.first == name; });
  if (it == t->otherPlaylists.end()) return false;
  auto target = std::move(it->second);
  it->first = t->playlistName;           // the live one takes the vacated slot
  it->second = t->source->playlist();
  t->playlistName = name;
  t->source->setPlaylist(std::move(target));
  return true;
}
bool Session::renamePlaylist(TrackId id, const std::string& from, const std::string& to) {
  Track* t = track(id);
  if (!t || to.empty() || from == to) return false;
  for (auto& n : playlistNames(id)) if (n == to) return false;
  if (t->playlistName == from) { t->playlistName = to; return true; }
  for (auto& o : t->otherPlaylists) if (o.first == from) { o.first = to; return true; }
  return false;
}
bool Session::deletePlaylist(TrackId id, const std::string& name) {
  Track* t = track(id);
  if (!t || name == t->playlistName) return false;
  const auto it = std::remove_if(t->otherPlaylists.begin(), t->otherPlaylists.end(), [&](const auto& x) { return x.first == name; });
  if (it == t->otherPlaylists.end()) return false;
  t->otherPlaylists.erase(it, t->otherPlaylists.end());
  return true;
}
bool Session::compFromPlaylist(TrackId id, const std::string& name, std::int64_t from, std::int64_t to) {
  Track* t = track(id);
  if (!t || to <= from) return false;
  auto it = std::find_if(t->otherPlaylists.begin(), t->otherPlaylists.end(), [&](const auto& x) { return x.first == name; });
  if (it == t->otherPlaylists.end()) return false;
  t->source->setPlaylist(edit::compRange(t->source->playlist(), it->second, from, to));
  return true;
}

int Session::addClip(TrackId id, Clip c) {
  Track* t = track(id);
  if (!t) return -1;
  c.id = t->nextClipId++;
  auto list = t->source->playlist();
  list.push_back(std::move(c));
  t->source->setPlaylist(std::move(list));
  return t->nextClipId - 1;
}

int Session::addClipFromFile(TrackId id, const std::string& path, std::int64_t start, std::string* error) {
  if (!track(id)) { if (error) *error = "no such track"; return -1; }
  Clip c;
  c.file = path;
  std::int64_t frames = 0;
  if (!loadWav(path, rate_, c.audio, &frames, error)) return -1;
  c.start = std::max<std::int64_t>(0, start);
  c.length = frames;
  return addClip(id, std::move(c));
}

bool Session::removeClip(TrackId id, int clipId) {
  Track* t = track(id);
  if (!t) return false;
  auto list = t->source->playlist();
  const auto it = std::remove_if(list.begin(), list.end(), [&](const Clip& c) { return c.id == clipId; });
  if (it == list.end()) return false;
  list.erase(it, list.end());
  t->source->setPlaylist(std::move(list));
  return true;
}

int Session::commitTakes(std::string* error) {
  int added = 0;
  for (auto& tp : tracks_) {
    if (!tp) continue;
    if (!tp->source->waitIdle()) { if (error) *error = "recording of \"" + tp->name + "\" did not finish in time"; continue; }
    const std::string werr = tp->source->lastError();
    if (!werr.empty() && error) *error = werr;
    for (const Take& tk : tp->source->collectTakes()) {
      Clip c;
      c.file = tk.path;
      std::int64_t frames = 0;
      std::string e;
      if (!loadWav(tk.path, rate_, c.audio, &frames, &e)) { if (error) *error = e; continue; }
      c.start = tk.startFrame;
      c.length = frames;
      if (frames > 0 && addClip(static_cast<TrackId>(&tp - &tracks_[0]), std::move(c)) > 0) ++added;
    }
  }
  return added;
}

// ============================== bounce ==============================

BounceResult Session::bounce(const std::string& path, std::int64_t start, std::int64_t end, std::int64_t tail, io::WavSampleType type) {
  BounceResult r;
  if (end <= start || start < 0 || tail < 0) { r.error = "empty or invalid range"; return r; }
  io::WavWriter wav;
  std::string err;
  if (!wav.open(path, static_cast<unsigned>(channels_), static_cast<unsigned>(rate_), type, &err, "Wezt bounce", static_cast<std::uint64_t>(start))) {
    r.error = err;
    return r;
  }
  const RtContext* previous = ctx_;
  {
    Engine eng(graph_, maxBlock_, channels_, rate_, 0);
    attach(eng.context());
    eng.transport().locate(start);
    eng.transport().play();
    const std::int64_t total = end - start + tail;
    std::vector<std::vector<float>> out(channels_, std::vector<float>(maxBlock_));
    std::vector<float*> op;
    std::vector<const float*> cp;
    for (auto& v : out) { op.push_back(v.data()); cp.push_back(v.data()); }
    bool good = true;
    for (std::int64_t done = 0; done < total && good; ) {
      const unsigned n = static_cast<unsigned>(std::min<std::int64_t>(maxBlock_, total - done));
      eng.process(nullptr, op.data(), n);
      for (auto& v : out) for (unsigned i = 0; i < n; ++i) r.peak = std::max(r.peak, std::fabs(v[i]));
      if (!wav.write(cp.data(), n)) { r.error = "disk write failed (disk full?)"; good = false; }
      done += n;
    }
    eng.transport().stop();
    if (good) r.frames = total;
    r.ok = good;
  }
  attach(previous);
  std::string ferr;
  if (!wav.finalize(&ferr) && r.ok) { r.ok = false; r.error = ferr; }
  return r;
}

// ============================== save ==============================

bool Session::save(const std::string& path, std::string* error) {
  sessionPath_ = path;
  setRecordFolder(recordDir_);  // refresh default folder
  const fs::path dir = fs::path(path).parent_path();
  std::map<BusId, int> busNum, trackNum;
  int n = 0;
  for (std::size_t i = 0; i < buses_.size(); ++i) if (buses_[i]) busNum[static_cast<BusId>(i)] = n++;

  std::ostringstream o;
  o << "WEZT-SESSION 1\n";
  o << "rate=" << fnum(rate_) << " channels=" << channels_ << " block=" << maxBlock_ << " tempo=" << fnum(tempo_) << " beats=" << beatsPerBar_ << "\n";
  auto stripLine = [&](const char* kind, const std::string& name, const Strip& s) {
    o << kind << " name=" << enc(name) << " gainDb=" << fnum(s.gainDb()) << " pan=" << fnum(s.pan()) << " mute=" << s.muted()
      << " auto=" << s.automationOn();
  };
  auto lane = [&](const char* what, const AutomationLane& l) {
    if (l.empty()) return;
    o << "auto lane=" << what;
    for (auto& p : l.points()) o << " " << p.frame << ":" << fnum(p.value);
    o << "\n";
  };
  for (std::size_t i = 0; i < buses_.size(); ++i) {
    const Bus* b = buses_[i].get();
    if (!b) continue;
    stripLine(i == 0 ? "master" : "bus", b->name, *b->strip);
    if (i) o << " out=" << busNum[b->output];
    o << "\n";
    lane("vol", b->strip->volumeLane()); lane("pan", b->strip->panLane()); lane("mute", b->strip->muteLane());
  }
  for (auto& tp : tracks_) {
    if (!tp) continue;
    const Track& t = *tp;
    stripLine("track", t.name, *t.strip);
    o << " mono=" << t.mono << " solo=" << t.solo << " arm=" << t.source->armed() << " monitor=" << static_cast<int>(t.source->monitorMode())
      << " out=" << busNum[t.output] << " inputs=";
    for (std::size_t i = 0; i < t.source->inputs().size(); ++i) o << (i ? "," : "") << t.source->inputs()[i];
    o << "\n";
    lane("vol", t.strip->volumeLane()); lane("pan", t.strip->panLane()); lane("mute", t.strip->muteLane());
    o << "playlist name=" << enc(t.playlistName) << "\n";
    for (auto& in : t.inserts) {
      const auto state = in.instance ? in.instance->saveState() : in.offlineState;
      o << "insert format=" << static_cast<int>(in.descriptor.format) << " name=" << enc(in.descriptor.name) << " vendor=" << enc(in.descriptor.vendor)
        << " path=" << enc(in.descriptor.path) << " bits=" << in.descriptor.bitness << " state=" << hex(state) << "\n";
      for (const auto& pl : in.paramLanes) {   // plugin parameter automation: normalized 0..1
        if (pl.lane.empty()) continue;
        o << "plane id=" << pl.id;
        for (auto& p : pl.lane.points()) o << " " << p.frame << ":" << fnum(p.value);
        o << "\n";
      }
    }
    for (auto& s : t.sends) o << "send bus=" << busNum[s.bus] << " db=" << fnum(s.levelDb) << " pre=" << s.preFader << " mute=" << s.muted << "\n";
    auto writeClip = [&](const char* kind, const std::string& pl, const Clip& c) {
      std::string f = c.file;
      std::error_code ec;
      const fs::path rel = fs::relative(fs::path(f), dir, ec);
      if (!ec && !rel.empty() && *rel.begin() != "..") f = rel.generic_string();
      o << kind << " ";
      if (!pl.empty()) o << "pl=" << enc(pl) << " ";
      o << "file=" << enc(f) << " start=" << c.start << " offset=" << c.offset << " length=" << c.length << " gainDb=" << fnum(c.gainDb)
        << " fadeIn=" << c.fadeIn << " fadeOut=" << c.fadeOut << " name=" << enc(c.name) << " muted=" << c.muted << " locked=" << c.locked;
      if (!c.gainEnv.empty()) {
        o << " env=";
        for (std::size_t i = 0; i < c.gainEnv.points().size(); ++i) o << (i ? "," : "") << c.gainEnv.points()[i].frame << ":" << fnum(c.gainEnv.points()[i].value);
      }
      o << "\n";
    };
    for (auto& c : t.source->playlist()) writeClip("clip", "", c);
    for (auto& [pn, clips] : t.otherPlaylists) {
      o << "otherplaylist name=" << enc(pn) << "\n";
      for (auto& c : clips) writeClip("altclip", pn, c);
    }
  }
  {
    std::map<TrackId, int> order;
    int k = 0;
    for (std::size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i]) order[static_cast<TrackId>(i)] = k++;
    for (auto& v : vcas_) {
      if (!v) continue;
      o << "vca name=" << enc(v->name) << " db=" << fnum(v->gainDb) << " members=";
      bool first = true;
      for (TrackId t : v->members) if (order.count(t)) { o << (first ? "" : ",") << order[t]; first = false; }
      if (first) o << "-";
      o << "\n";
    }
  }
  for (auto& m : memory_.all()) {
    o << "mem number=" << m.number << " name=" << enc(m.name) << " comment=" << enc(m.comment) << " start=" << m.start << " end=" << m.end
      << " zoom=" << m.zoomPreset << " pre=" << m.preRoll << " post=" << m.postRoll << " tracks=";
    if (m.visibleTracks.empty()) o << "-";
    for (std::size_t i = 0; i < m.visibleTracks.size(); ++i) o << (i ? "," : "") << m.visibleTracks[i];
    o << "\n";
  }
  o << "end\n";

  // Write beside the target and rename, so a crash or full disk never leaves a half-written session.
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { if (error) *error = "cannot write " + tmp; return false; }
    const std::string text = o.str();
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    f.flush();
    if (!f) { if (error) *error = "write failed (disk full?): " + tmp; std::error_code ec; fs::remove(tmp, ec); return false; }
  }
  std::error_code ec;
  if (fs::exists(path, ec)) fs::copy_file(path, path + ".bak", fs::copy_options::overwrite_existing, ec);  // last good version
  fs::rename(tmp, path, ec);
  if (ec) { if (error) *error = "cannot replace " + path + ": " + ec.message(); return false; }
  return true;
}

// ============================== load ==============================

std::unique_ptr<Session> Session::load(const std::string& path, const InsertFactory& factory, std::string* error) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<Session> { if (error) *error = m; return nullptr; };
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail("cannot open " + path);
  std::string head;
  std::getline(f, head);
  if (head.rfind("WEZT-SESSION ", 0) != 0) return fail("not a Wezt session file");
  const long version = std::strtol(head.c_str() + 13, nullptr, 10);
  if (version < 1 || version > 1) return fail("this session was saved by a newer Wezt (format " + std::to_string(version) + ")");
  std::string line;
  std::getline(f, line);
  const Line cfg = parseLine("cfg " + line);
  const double rate = num(cfg.kv, "rate", 0);
  const double ch = num(cfg.kv, "channels", 0), blk = num(cfg.kv, "block", 0);
  if (rate < 8000 || rate > 768000 || ch < 1 || ch > 8 || blk < 16 || blk > 16384) return fail("session header is damaged");

  auto s = std::make_unique<Session>(rate, static_cast<std::size_t>(ch), static_cast<std::size_t>(blk));
  s->sessionPath_ = path;
  s->setTempo(num(cfg.kv, "tempo", 120.0));
  s->setBeatsPerBar(static_cast<int>(num(cfg.kv, "beats", 4)));
  const fs::path dir = fs::path(path).parent_path();
  std::vector<BusId> busIds{kMaster};
  std::vector<std::pair<BusId, int>> busOut;  // resolved once every bus exists
  TrackId cur = -1;
  std::vector<TrackId> loadedTracks;
  bool ended = false;
  int lineNo = 2;
  auto applyStrip = [&](Strip& st, const KV& kv) {
    st.setGainDb(static_cast<float>(num(kv, "gainDb", 0)));
    st.setPan(static_cast<float>(num(kv, "pan", 0)));
    st.setMute(num(kv, "mute", 0) != 0);
    st.setAutomationOn(num(kv, "auto", 1) != 0);
  };
  AutomationLane* currentLaneOwner[3] = {nullptr, nullptr, nullptr};  // vol, pan, mute of the last strip line
  auto parseOutBus = [&](const KV& kv, BusId def) {
    const int idx = static_cast<int>(num(kv, "out", def));
    return idx >= 0 && static_cast<std::size_t>(idx) < busIds.size() ? busIds[static_cast<std::size_t>(idx)] : kMaster;
  };
  while (std::getline(f, line)) {
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const Line l = parseLine(line);
    if (l.kind == "end") { ended = true; break; }
    if (l.kind == "master") {
      s->buses_[0]->name = str(l.kv, "name");
      applyStrip(*s->buses_[0]->strip, l.kv);
      Strip& st = *s->buses_[0]->strip;
      currentLaneOwner[0] = &st.volumeLane(); currentLaneOwner[1] = &st.panLane(); currentLaneOwner[2] = &st.muteLane();
    } else if (l.kind == "bus") {
      const BusId b = s->addBus(str(l.kv, "name"));
      busIds.push_back(b);
      applyStrip(*s->buses_[static_cast<std::size_t>(b)]->strip, l.kv);
      busOut.emplace_back(b, static_cast<int>(num(l.kv, "out", 0)));
      Strip& st = *s->buses_[static_cast<std::size_t>(b)]->strip;
      currentLaneOwner[0] = &st.volumeLane(); currentLaneOwner[1] = &st.panLane(); currentLaneOwner[2] = &st.muteLane();
    } else if (l.kind == "track") {
      for (auto& bo : busOut) {  // resolve bus routing now that all buses exist
        const int idx = bo.second;
        if (idx > 0 && static_cast<std::size_t>(idx) < busIds.size()) s->setBusOutput(bo.first, busIds[static_cast<std::size_t>(idx)]);
      }
      busOut.clear();
      std::vector<unsigned> in;
      std::istringstream is(l.kv.count("inputs") ? l.kv.at("inputs") : "0");
      std::string tok;
      while (std::getline(is, tok, ',')) in.push_back(static_cast<unsigned>(std::max(0l, std::strtol(tok.c_str(), nullptr, 10))));
      cur = s->addTrack(str(l.kv, "name"), num(l.kv, "mono", 0) != 0, in);
      loadedTracks.push_back(cur);
      Track& t = *s->track(cur);
      applyStrip(*t.strip, l.kv);
      s->setTrackOutput(cur, parseOutBus(l.kv, 0));
      t.source->setMonitorMode(static_cast<MonitorMode>(std::clamp(static_cast<int>(num(l.kv, "monitor", 0)), 0, 2)));
      if (num(l.kv, "arm", 0) != 0) t.source->arm(true);
      t.solo = num(l.kv, "solo", 0) != 0;
      currentLaneOwner[0] = &t.strip->volumeLane(); currentLaneOwner[1] = &t.strip->panLane(); currentLaneOwner[2] = &t.strip->muteLane();
    } else if (l.kind == "auto") {
      const std::string which = l.kv.count("lane") ? l.kv.at("lane") : "";
      AutomationLane* lane = which == "vol" ? currentLaneOwner[0] : which == "pan" ? currentLaneOwner[1] : which == "mute" ? currentLaneOwner[2] : nullptr;
      if (!lane) { s->warnings_.push_back("line " + std::to_string(lineNo) + ": unknown automation lane"); continue; }
      for (auto& b : l.bare) {
        const auto colon = b.find(':');
        if (colon == std::string::npos) continue;
        lane->add(std::strtoll(b.substr(0, colon).c_str(), nullptr, 10), static_cast<float>(std::strtod(b.c_str() + colon + 1, nullptr)));
      }
    } else if (cur >= 0 && l.kind == "insert") {
      plugins::PluginDescriptor d;
      d.name = str(l.kv, "name"); d.vendor = str(l.kv, "vendor"); d.path = str(l.kv, "path");
      d.format = static_cast<plugins::Format>(static_cast<int>(num(l.kv, "format", 0)));
      d.bitness = static_cast<int>(num(l.kv, "bits", 0));
      std::vector<std::uint8_t> state;
      if (!unhex(l.kv.count("state") ? l.kv.at("state") : "-", state)) { state.clear(); s->warnings_.push_back("plugin \"" + d.name + "\": saved settings are damaged"); }
      std::unique_ptr<plugins::IPluginInstance> inst = factory ? factory(d) : nullptr;
      if (inst && !state.empty() && !inst->loadState(state)) s->warnings_.push_back("plugin \"" + d.name + "\": could not restore its saved settings");
      const bool offline = inst == nullptr;
      if (offline) s->warnings_.push_back("plugin \"" + d.name + "\" is not available; the insert is bypassed and its settings are kept");
      s->addInsert(cur, std::move(inst), d);
      if (offline) s->track(cur)->inserts.back().offlineState = state;
    } else if (cur >= 0 && l.kind == "plane") {   // automation of the last insert's parameter (kept even when the plugin is offline)
      Track& t = *s->track(cur);
      if (t.inserts.empty()) { s->warnings_.push_back("line " + std::to_string(lineNo) + ": plugin automation without a plugin"); continue; }
      ParamLane pl; pl.id = static_cast<unsigned>(num(l.kv, "id", 0));
      for (auto& b : l.bare) {
        const auto colon = b.find(':');
        if (colon == std::string::npos) continue;
        const float v = static_cast<float>(std::strtod(b.c_str() + colon + 1, nullptr));
        if (std::isfinite(v)) pl.lane.add(std::strtoll(b.substr(0, colon).c_str(), nullptr, 10), std::clamp(v, 0.f, 1.f));
      }
      t.inserts.back().paramLanes.push_back(std::move(pl));
      s->publishParamAutomation(cur);
    } else if (l.kind == "vca") {
      std::vector<TrackId> members;
      std::istringstream ms(l.kv.count("members") ? l.kv.at("members") : "-");
      std::string tk;
      while (std::getline(ms, tk, ',')) {
        if (tk == "-") continue;
        const long idx = std::strtol(tk.c_str(), nullptr, 10);
        if (idx >= 0 && static_cast<std::size_t>(idx) < loadedTracks.size()) members.push_back(loadedTracks[static_cast<std::size_t>(idx)]);
      }
      const VcaId v = s->addVca(str(l.kv, "name"), members);
      s->setVcaGain(v, static_cast<float>(num(l.kv, "db", 0)));
    } else if (l.kind == "mem") {
      edit::MemoryLocation m;
      m.name = str(l.kv, "name"); m.comment = str(l.kv, "comment");
      m.start = static_cast<std::int64_t>(num(l.kv, "start", 0));
      m.end = static_cast<std::int64_t>(num(l.kv, "end", 0));
      m.zoomPreset = static_cast<int>(num(l.kv, "zoom", 0));
      m.preRoll = static_cast<std::int64_t>(num(l.kv, "pre", 0));
      m.postRoll = static_cast<std::int64_t>(num(l.kv, "post", 0));
      if (l.kv.count("tracks") && l.kv.at("tracks") != "-") {
        std::istringstream ts(l.kv.at("tracks"));
        std::string tk;
        while (std::getline(ts, tk, ',')) m.visibleTracks.push_back(static_cast<int>(std::strtol(tk.c_str(), nullptr, 10)));
      }
      s->memory_.add(std::move(m), static_cast<int>(num(l.kv, "number", 0)));
    } else if (cur >= 0 && l.kind == "send") {
      const int idx = static_cast<int>(num(l.kv, "bus", 0));
      const BusId b = idx >= 0 && static_cast<std::size_t>(idx) < busIds.size() ? busIds[static_cast<std::size_t>(idx)] : kMaster;
      const int si = s->addSend(cur, b, static_cast<float>(num(l.kv, "db", 0)), num(l.kv, "pre", 0) != 0);
      if (si >= 0 && num(l.kv, "mute", 0) != 0) s->setSendMute(cur, static_cast<std::size_t>(si), true);
    } else if (cur >= 0 && l.kind == "playlist") {
      s->track(cur)->playlistName = str(l.kv, "name").empty() ? "Main" : str(l.kv, "name");
    } else if (cur >= 0 && l.kind == "otherplaylist") {
      const std::string pn = str(l.kv, "name");
      Track* tk = s->track(cur);
      if (!pn.empty() && tk && std::none_of(tk->otherPlaylists.begin(), tk->otherPlaylists.end(), [&](const auto& x) { return x.first == pn; })) tk->otherPlaylists.push_back({pn, {}});
    } else if (cur >= 0 && (l.kind == "clip" || l.kind == "altclip")) {
      Clip c;
      std::string file = str(l.kv, "file");
      fs::path p(file);
      if (p.is_relative()) p = dir / p;
      c.file = file;
      c.start = static_cast<std::int64_t>(num(l.kv, "start", 0));
      c.offset = static_cast<std::int64_t>(num(l.kv, "offset", 0));
      c.length = static_cast<std::int64_t>(num(l.kv, "length", 0));
      c.gainDb = static_cast<float>(num(l.kv, "gainDb", 0));
      c.fadeIn = static_cast<std::int64_t>(num(l.kv, "fadeIn", 0));
      c.fadeOut = static_cast<std::int64_t>(num(l.kv, "fadeOut", 0));
      c.name = str(l.kv, "name");
      c.muted = num(l.kv, "muted", 0) != 0;
      c.locked = num(l.kv, "locked", 0) != 0;
      if (l.kv.count("env")) {
        std::istringstream es(l.kv.at("env"));
        std::string tk;
        while (std::getline(es, tk, ',')) {
          const auto colon = tk.find(':');
          if (colon != std::string::npos) c.gainEnv.add(std::strtoll(tk.c_str(), nullptr, 10), static_cast<float>(std::strtod(tk.c_str() + colon + 1, nullptr)));
        }
      }
      std::string e;
      if (!loadWav(p.string(), s->rate_, c.audio, nullptr, &e)) s->warnings_.push_back("audio missing or unusable (" + e + "); the clip will play as silence");
      if (c.start < 0 || c.offset < 0 || c.length < 0) { s->warnings_.push_back("line " + std::to_string(lineNo) + ": clip has invalid times, skipped"); continue; }
      if (l.kind == "altclip") {
        Track* tk = s->track(cur);
        const std::string pn = str(l.kv, "pl");
        auto it = tk ? std::find_if(tk->otherPlaylists.begin(), tk->otherPlaylists.end(), [&](const auto& x) { return x.first == pn; }) : decltype(tk->otherPlaylists.end()){};
        if (tk && it != tk->otherPlaylists.end()) { c.id = tk->nextClipId++; it->second.push_back(std::move(c)); }
        continue;
      }
      s->addClip(cur, std::move(c));
    }  // unknown lines are ignored, so newer files with extra keys still open
  }
  if (!ended) return fail("session file is incomplete (cut off at line " + std::to_string(lineNo) + ")");
  for (auto& bo : busOut) {
    const int idx = bo.second;
    if (idx > 0 && static_cast<std::size_t>(idx) < busIds.size()) s->setBusOutput(bo.first, busIds[static_cast<std::size_t>(idx)]);
  }
  s->updateSolo();
  s->setRecordFolder("");
  return s;
}

}  // namespace wezt::session
