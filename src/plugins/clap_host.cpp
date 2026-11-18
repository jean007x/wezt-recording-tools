#include "plugins/clap_host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

#include <clap/clap.h>

#include "plugins/param_ring.h"
#include "util/json.h"
#include "wezt_agent.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace wezt::plugins {
namespace {

// ---- dynamic library -------------------------------------------------------------------------
struct Lib {
  void* handle = nullptr;
  Lib() = default;
  Lib(const Lib&) = delete;
  Lib& operator=(const Lib&) = delete;
  ~Lib() { close(); }

  static fs::path binaryPath(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {  // macOS bundle: Contents/MacOS/<name>
      const fs::path candidate = p / "Contents" / "MacOS" / p.stem();
      if (fs::exists(candidate, ec)) return candidate;
    }
    return p;
  }

  bool open(const std::string& path, std::string* err) {
    const fs::path bin = binaryPath(path);
#ifdef _WIN32
    handle = LoadLibraryW(bin.wstring().c_str());
    if (!handle && err) *err = "cannot load " + bin.string() + " (error " + std::to_string(GetLastError()) + ")";
#else
    handle = dlopen(bin.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle && err) {
      const char* m = dlerror();
      *err = std::string("cannot load ") + bin.string() + ": " + (m ? m : "unknown error");
    }
#endif
    return handle != nullptr;
  }
  void* sym(const char* name) const {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
  }
  void close() {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
    handle = nullptr;
  }
};

// ---- clap_entry init/deinit, once per library rather than once per instance --------------------
// A library that is loaded twice (two instances, or a scan while an instance exists) is the same
// dlopen handle and the same clap_entry. CLAP says init() is called once and deinit() once, so the
// calls are counted per library: the second user must not re-initialise it, and the first user to
// leave must not tear it down under the others.
std::mutex& entryMutex() { static std::mutex m; return m; }
std::map<std::string, int>& entryCounts() { static std::map<std::string, int> c; return c; }

std::string entryKey(const std::string& path) {
  std::error_code ec;
  const fs::path abs = fs::absolute(path, ec);
  return ec ? path : abs.lexically_normal().string();
}
bool entryInit(const clap_plugin_entry_t* e, const std::string& key, const std::string& path) {
  std::lock_guard<std::mutex> lock(entryMutex());
  int& n = entryCounts()[key];
  if (n == 0 && !e->init(path.c_str())) { entryCounts().erase(key); return false; }
  ++n;
  return true;
}
void entryDeinit(const clap_plugin_entry_t* e, const std::string& key) {
  std::lock_guard<std::mutex> lock(entryMutex());
  const auto it = entryCounts().find(key);
  if (it == entryCounts().end()) return;
  if (--it->second == 0) {
    e->deinit();
    entryCounts().erase(it);
  }
}

// ---- minimal host ----------------------------------------------------------------------------
// host_data points at this instance's flag: the plug-in says its latency changed (any thread), the control thread collects it.
void hostLatencyChanged(const clap_host_t* h) { if (h && h->host_data) static_cast<std::atomic<bool>*>(h->host_data)->store(true, std::memory_order_release); }
const clap_host_latency_t kHostLatency = {hostLatencyChanged};
const void* hostGetExtension(const clap_host_t*, const char* id) {
  if (id && std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &kHostLatency;
  return nullptr;   // no other host extensions yet
}
void hostNoop(const clap_host_t*) {}

clap_host_t makeHost() {
  clap_host_t h = {};
  h.clap_version = CLAP_VERSION_INIT;
  h.host_data = nullptr;
  h.name = "Wezt Recording Tools";
  h.vendor = "Wezt";
  h.url = "";
  h.version = "0.2.0";
  h.get_extension = hostGetExtension;
  h.request_restart = hostNoop;
  h.request_process = hostNoop;
  h.request_callback = hostNoop;
  return h;
}

// ---- event lists -----------------------------------------------------------------------------
union AnyEvent {
  clap_event_header_t header;
  clap_event_param_value_t param;
  clap_event_note_t note;
  clap_event_midi_t midi;
  AnyEvent() { std::memset(this, 0, sizeof *this); }
};
struct InEvents {
  std::vector<AnyEvent> events;  // reserved up front so process() never allocates
  clap_input_events_t iface{};
  InEvents() {
    events.reserve(1024);
    iface.ctx = this;
    iface.size = [](const clap_input_events_t* l) { return static_cast<std::uint32_t>(static_cast<InEvents*>(l->ctx)->events.size()); };
    iface.get = [](const clap_input_events_t* l, std::uint32_t i) -> const clap_event_header_t* {
      auto* self = static_cast<InEvents*>(l->ctx);
      return i < self->events.size() ? &self->events[i].header : nullptr;
    };
  }
};
bool outTryPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }  // plugin output events ignored for now

}  // namespace

std::vector<ClapClassInfo> describeClap(const std::string& path, std::string* error) {
  Lib lib;
  if (!lib.open(path, error)) return {};
  auto* entry = static_cast<const clap_plugin_entry_t*>(lib.sym("clap_entry"));
  if (!entry) { if (error) *error = "not a CLAP plugin (no clap_entry)"; return {}; }
  if (!clap_version_is_compatible(entry->clap_version)) { if (error) *error = "unsupported CLAP version"; return {}; }
  const std::string key = entryKey(path);
  if (!entryInit(entry, key, path)) { if (error) *error = "plugin init failed"; return {}; }
  std::vector<ClapClassInfo> out;
  if (auto* f = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID))) {
    for (std::uint32_t i = 0; i < f->get_plugin_count(f); ++i)
      if (const auto* d = f->get_plugin_descriptor(f, i))
        out.push_back({d->id ? d->id : "", d->name ? d->name : "", d->vendor ? d->vendor : "", d->version ? d->version : ""});
  }
  entryDeinit(entry, key);
  return out;
}

struct ClapInstance::Impl {
  Lib lib;  // declared first: destroyed last
  const clap_plugin_entry_t* entry = nullptr;
  std::string key;            // identifies the library for the init/deinit count
  bool entryInited = false;   // true only after a successful init(): deinit() is owed exactly then
  const clap_plugin_t* plugin = nullptr;
  std::atomic<bool> latencyChanged{false};
  clap_host_t host = makeHost();
  const clap_plugin_params_t* params = nullptr;
  const clap_plugin_state_t* state = nullptr;
  const clap_plugin_latency_t* latency = nullptr;
  const clap_plugin_audio_ports_t* ports = nullptr;
  std::string name;
  std::vector<clap_param_info_t> infos;
  // MIDI in: from the note-ports extension. CLAP dialect = note events; MIDI dialect = raw MIDI bytes.
  bool midiIn = false, clapDialect = false, midiDialect = false;
  std::array<MidiEvent, kMaxMidiPerBlock> midi;   // this block's events (audio thread)
  std::size_t midiN = 0;
  std::array<std::uint16_t, 128> held{};          // per key, a bit per channel that has the note on

  // Main (first) port widths drive the engine's stereo I/O; every declared port still gets a buffer,
  // because the CLAP spec requires the counts passed to process() to match the plugin's ports.
  unsigned inCh = 0, outCh = 0;
  std::vector<unsigned> inPortCh, outPortCh;
  int maxFrames = 0;
  bool active = false, processing = false;
  std::vector<float> inStore, outStore;
  std::vector<float*> inPtrs, outPtrs;  // flat: all channels of port 0, then port 1, ...
  std::vector<clap_audio_buffer_t> inBufs, outBufs;
  InEvents events;
  clap_output_events_t outEvents{};
  ParamRing<512> ring;

  void deactivate() {
    if (!active) return;
    if (processing) plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    processing = active = false;
  }
};

ClapInstance::ClapInstance() : impl_(std::make_unique<Impl>()) {}

ClapInstance::~ClapInstance() {
  if (!impl_) return;
  auto& d = *impl_;
  d.deactivate();
  if (d.plugin) d.plugin->destroy(d.plugin);
  if (d.entry && d.entryInited) entryDeinit(d.entry, d.key);
}

std::unique_ptr<ClapInstance> ClapInstance::load(const std::string& path, std::string* error, const std::string& pluginId) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<ClapInstance> {
    if (error) *error = m;
    return nullptr;
  };
  std::unique_ptr<ClapInstance> inst(new ClapInstance());
  auto& d = *inst->impl_;

  std::string err;
  if (!d.lib.open(path, &err)) return fail(err);
  d.entry = static_cast<const clap_plugin_entry_t*>(d.lib.sym("clap_entry"));
  if (!d.entry) return fail("not a CLAP plugin (no clap_entry): " + path);
  if (!clap_version_is_compatible(d.entry->clap_version)) return fail("unsupported CLAP version in " + path);
  d.key = entryKey(path);
  if (!entryInit(d.entry, d.key, path)) return fail("plugin entry init failed: " + path);
  d.entryInited = true;

  auto* factory = static_cast<const clap_plugin_factory_t*>(d.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!factory || factory->get_plugin_count(factory) == 0) return fail("no plugins in " + path);

  const clap_plugin_descriptor_t* chosen = nullptr;
  for (std::uint32_t i = 0; i < factory->get_plugin_count(factory); ++i) {
    const auto* desc = factory->get_plugin_descriptor(factory, i);
    if (desc && (pluginId.empty() || pluginId == desc->id)) { chosen = desc; break; }
  }
  if (!chosen) return fail("plugin id not found: " + pluginId);

  d.host.host_data = &d.latencyChanged;   // the host struct is part of the instance, so its address is stable
  d.plugin = factory->create_plugin(factory, &d.host, chosen->id);
  if (!d.plugin) return fail(std::string("create_plugin failed for ") + chosen->id);
  if (!d.plugin->init(d.plugin)) {
    d.plugin->destroy(d.plugin);
    d.plugin = nullptr;
    return fail(std::string("plugin init failed: ") + chosen->id);
  }
  d.name = chosen->name ? chosen->name : chosen->id;

  auto ext = [&](const char* id) { return d.plugin->get_extension(d.plugin, id); };
  d.params = static_cast<const clap_plugin_params_t*>(ext(CLAP_EXT_PARAMS));
  d.state = static_cast<const clap_plugin_state_t*>(ext(CLAP_EXT_STATE));
  d.latency = static_cast<const clap_plugin_latency_t*>(ext(CLAP_EXT_LATENCY));
  d.ports = static_cast<const clap_plugin_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS));

  if (auto* np = static_cast<const clap_plugin_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS)))
    if (np->count && np->get && np->count(d.plugin, true) > 0) {
      clap_note_port_info_t ni = {};
      if (np->get(d.plugin, 0, true, &ni)) {
        d.clapDialect = (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
        d.midiDialect = (ni.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
        d.midiIn = d.clapDialect || d.midiDialect;
      }
    }

  if (d.params) {
    const std::uint32_t n = d.params->count(d.plugin);
    for (std::uint32_t i = 0; i < n; ++i) {
      clap_param_info_t info = {};
      if (d.params->get_info(d.plugin, i, &info)) d.infos.push_back(info);
    }
  }

  // Every audio port the plugin declares. No ports extension: assume one stereo in and one stereo out.
  auto portList = [&](bool isInput) {
    std::vector<unsigned> v;
    if (!d.ports) { v.push_back(2); return v; }
    const std::uint32_t n = d.ports->count(d.plugin, isInput);
    for (std::uint32_t i = 0; i < n; ++i) {
      clap_audio_port_info_t pi = {};
      v.push_back(d.ports->get(d.plugin, i, isInput, &pi) ? pi.channel_count : 2u);
    }
    return v;
  };
  d.inPortCh = portList(true);
  d.outPortCh = portList(false);
  d.inCh = d.inPortCh.empty() ? 0 : d.inPortCh[0];
  d.outCh = d.outPortCh.empty() ? 0 : d.outPortCh[0];
  if (d.outCh == 0) return fail("plugin has no audio output: " + d.name);
  d.outEvents.ctx = nullptr;
  d.outEvents.try_push = outTryPush;
  return inst;
}

void ClapInstance::prepare(double sampleRate, std::size_t maxFrames) {
  auto& d = *impl_;
  d.deactivate();
  d.maxFrames = static_cast<int>(maxFrames);
  auto build = [&](const std::vector<unsigned>& ports, std::vector<float>& store, std::vector<float*>& flat,
                   std::vector<clap_audio_buffer_t>& bufs) {
    std::size_t total = 0;
    for (unsigned c : ports) total += c;
    store.assign(total * maxFrames, 0.f);
    flat.clear();
    for (std::size_t i = 0; i < total; ++i) flat.push_back(store.data() + i * maxFrames);
    bufs.assign(ports.size(), clap_audio_buffer_t{});
    std::size_t off = 0;
    for (std::size_t k = 0; k < ports.size(); ++k) {
      bufs[k].data32 = flat.data() + off;
      bufs[k].channel_count = ports[k];
      off += ports[k];
    }
  };
  build(d.inPortCh, d.inStore, d.inPtrs, d.inBufs);
  build(d.outPortCh, d.outStore, d.outPtrs, d.outBufs);

  if (!d.plugin->activate(d.plugin, sampleRate, 1, static_cast<std::uint32_t>(maxFrames))) return;
  d.active = true;
  d.processing = d.plugin->start_processing(d.plugin);
  if (!d.processing) {  // plugin refused to start: leave inactive so process() outputs silence
    d.plugin->deactivate(d.plugin);
    d.active = false;
  }
}

void ClapInstance::process(const AudioBuffer& in, AudioBuffer& out) {
  auto& d = *impl_;
  const std::uint32_t frames = static_cast<std::uint32_t>(out.frames);
  if (!d.active || frames == 0 || static_cast<int>(frames) > d.maxFrames) {
    out.clear();
    return;
  }

  d.events.events.clear();
  ParamRing<512>::Item it;
  while (d.ring.pop(it) && d.events.events.size() < d.events.events.capacity()) {
    if (it.index >= d.infos.size()) continue;
    clap_event_param_value_t e = {};
    e.header.size = sizeof(e);
    e.header.time = 0;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.header.flags = 0;
    e.param_id = d.infos[it.index].id;
    e.cookie = d.infos[it.index].cookie;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = it.value;
    AnyEvent ae;
    ae.param = e;
    d.events.events.push_back(ae);
  }

  // MIDI from the sequencer or key preview, in time order, sample-accurate
  if (d.midiIn && d.midiN) {
    std::stable_sort(d.midi.begin(), d.midi.begin() + static_cast<std::ptrdiff_t>(d.midiN), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
    auto push = [&](const AnyEvent& e) { if (d.events.events.size() < d.events.events.capacity()) d.events.events.push_back(e); };
    auto note = [&](std::uint32_t time, bool on, int ch, int key, int vel) {
      if (d.clapDialect) {
        AnyEvent e;
        e.note.header.size = sizeof(clap_event_note_t); e.note.header.time = time; e.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.note.header.type = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF; e.note.header.flags = 0;
        e.note.note_id = -1; e.note.port_index = 0; e.note.channel = static_cast<std::int16_t>(ch); e.note.key = static_cast<std::int16_t>(key);
        e.note.velocity = on ? std::clamp(vel, 1, 127) / 127.0 : 0.0;
        push(e);
      } else {
        AnyEvent e;
        e.midi.header.size = sizeof(clap_event_midi_t); e.midi.header.time = time; e.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.midi.header.type = CLAP_EVENT_MIDI; e.midi.header.flags = 0; e.midi.port_index = 0;
        e.midi.data[0] = static_cast<std::uint8_t>((on ? 0x90 : 0x80) | (ch & 15)); e.midi.data[1] = static_cast<std::uint8_t>(key & 127); e.midi.data[2] = static_cast<std::uint8_t>(on ? std::clamp(vel, 1, 127) : 0);
        push(e);
      }
    };
    for (std::size_t k = 0; k < d.midiN; ++k) {
      const MidiEvent& m = d.midi[k];
      const std::uint32_t time = std::min<std::uint32_t>(m.offset, frames - 1);
      const int ch = m.channel & 15, key = m.a & 127;
      switch (m.type) {
        case MidiEvent::NoteOn:
          if (m.b > 0) { d.held[static_cast<std::size_t>(key)] = static_cast<std::uint16_t>(d.held[static_cast<std::size_t>(key)] | (1u << ch)); note(time, true, ch, key, m.b); break; }
          [[fallthrough]];
        case MidiEvent::NoteOff:
          d.held[static_cast<std::size_t>(key)] = static_cast<std::uint16_t>(d.held[static_cast<std::size_t>(key)] & ~(1u << ch));
          note(time, false, ch, key, 0);
          break;
        case MidiEvent::Controller:
        case MidiEvent::PitchBend:
          if (!d.midiDialect) break;   // note-only plug-ins have no use for controllers
          {
            AnyEvent e;
            e.midi.header.size = sizeof(clap_event_midi_t); e.midi.header.time = time; e.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.midi.header.type = CLAP_EVENT_MIDI; e.midi.port_index = 0;
            if (m.type == MidiEvent::Controller) { e.midi.data[0] = static_cast<std::uint8_t>(0xB0 | ch); e.midi.data[1] = static_cast<std::uint8_t>(m.a & 127); e.midi.data[2] = static_cast<std::uint8_t>(std::clamp<int>(m.b, 0, 127)); }
            else { const int v = std::clamp<int>(m.a, 0, 16383); e.midi.data[0] = static_cast<std::uint8_t>(0xE0 | ch); e.midi.data[1] = static_cast<std::uint8_t>(v & 127); e.midi.data[2] = static_cast<std::uint8_t>(v >> 7); }
            push(e);
          }
          break;
        case MidiEvent::AllNotesOff:   // release exactly the notes we started
          for (int kk = 0; kk < 128; ++kk)
            for (int c = 0; c < 16; ++c)
              if (d.held[static_cast<std::size_t>(kk)] & (1u << c)) note(time, false, c, kk, 0);
          std::fill(d.held.begin(), d.held.end(), 0);
          break;
      }
    }
  }
  d.midiN = 0;

  for (unsigned c = 0; c < d.inCh; ++c) {
    float* dst = d.inPtrs[c];
    if (c < in.channels && in.frames >= frames) std::copy(in.channel(c), in.channel(c) + frames, dst);
    else std::fill(dst, dst + frames, 0.f);
  }

  clap_process_t p = {};
  p.steady_time = -1;
  p.frames_count = frames;
  p.transport = nullptr;
  p.audio_inputs = d.inBufs.empty() ? nullptr : d.inBufs.data();  // extra input ports (side-chains) stay silent
  p.audio_inputs_count = static_cast<std::uint32_t>(d.inBufs.size());
  p.audio_outputs = d.outBufs.data();
  p.audio_outputs_count = static_cast<std::uint32_t>(d.outBufs.size());
  p.in_events = &d.events.iface;
  p.out_events = &d.outEvents;

  if (d.plugin->process(d.plugin, &p) == CLAP_PROCESS_ERROR) {
    out.clear();
    return;
  }
  for (std::size_t c = 0; c < out.channels; ++c) {
    const std::size_t src = std::min<std::size_t>(c, d.outCh - 1);
    std::copy(d.outPtrs[src], d.outPtrs[src] + frames, out.channel(c));
  }
}

bool ClapInstance::takeLatencyChanged() { return impl_->latencyChanged.exchange(false, std::memory_order_acq_rel); }

std::size_t ClapInstance::latencySamples() const {
  return impl_->latency ? impl_->latency->get(impl_->plugin) : 0;
}

bool ClapInstance::acceptsMidi() const { return impl_->midiIn; }
void ClapInstance::handleMidi(const MidiEvent* events, std::size_t count) {
  auto& d = *impl_;
  d.midiN = 0;
  if (!d.midiIn) return;
  for (std::size_t i = 0; i < count && d.midiN < d.midi.size(); ++i) d.midi[d.midiN++] = events[i];
}

std::size_t ClapInstance::parameterCount() const { return impl_->infos.size(); }
std::string ClapInstance::name() const { return impl_->name; }

std::vector<ParamInfo> ClapInstance::parameters() const {
  std::vector<ParamInfo> out;
  for (const auto& i : impl_->infos) {
    const double span = i.max_value - i.min_value;
    ParamInfo p{i.id, i.name, span != 0 ? (i.default_value - i.min_value) / span : 0.0, (i.flags & CLAP_PARAM_IS_HIDDEN) != 0};
    p.min = i.min_value; p.max = i.max_value;
    p.group = i.module;
    if (i.flags & CLAP_PARAM_IS_STEPPED) p.steps = std::max(1, static_cast<int>(span + 0.5));
    out.push_back(std::move(p));
  }
  return out;
}

std::string ClapInstance::parameterText(unsigned id, double normalized) const {
  auto& d = *impl_;
  if (!d.params || !d.params->value_to_text) return {};
  for (const auto& i : d.infos) {
    if (i.id != id) continue;
    const double plain = i.min_value + std::clamp(normalized, 0.0, 1.0) * (i.max_value - i.min_value);
    char buf[128] = {0};
    return d.params->value_to_text(d.plugin, id, plain, buf, sizeof buf) ? std::string(buf) : std::string();
  }
  return {};
}

namespace {
// Calls a wezt.agent/1 function with a growing buffer (the plugin returns the size it needs).
template <class F>
bool agentText(F call, std::string& out) {
  std::vector<char> buf(16 * 1024);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::uint32_t need = call(buf.data(), static_cast<std::uint32_t>(buf.size()));
    if (need == WEZT_AGENT_FAILED) { out.assign(buf.data(), strnlen(buf.data(), buf.size())); return false; }
    if (need < buf.size()) { out.assign(buf.data(), need); return true; }
    if (need > 4u * 1024 * 1024) { out = "the plugin's answer is too large"; return false; }
    buf.assign(static_cast<std::size_t>(need) + 1, 0);
  }
  out = "the plugin's answer changed size while it was read";
  return false;
}
}  // namespace

std::vector<AiToolInfo> ClapInstance::aiTools() const {
  auto& d = *impl_;
  std::vector<AiToolInfo> tools;
  if (!d.plugin) return tools;
  const auto* ext = static_cast<const wezt_plugin_agent_t*>(d.plugin->get_extension(d.plugin, WEZT_AGENT_EXT_ID));
  if (!ext || !ext->list_tools) return tools;
  std::string text;
  if (!agentText([&](char* o, std::uint32_t c) { return ext->list_tools(d.plugin, o, c); }, text)) return tools;
  json::Value v;
  if (!json::parse(text, v) || !v.isArray()) return tools;
  for (const auto& t : v.items()) {
    if (!t.isObject() || t.getString("name").empty()) continue;
    AiToolInfo a;
    a.name = t.getString("name");
    a.description = t.getString("description");
    if (auto* sc = t.get("input_schema")) a.inputSchemaJson = sc->dump();
    tools.push_back(std::move(a));
    if (tools.size() >= 64) break;  // a plugin cannot flood the agent's tool list
  }
  return tools;
}

std::string ClapInstance::callAiTool(const std::string& name, const std::string& argsJson, bool& ok) {
  auto& d = *impl_;
  ok = false;
  if (!d.plugin) return "plugin is not loaded";
  const auto* ext = static_cast<const wezt_plugin_agent_t*>(d.plugin->get_extension(d.plugin, WEZT_AGENT_EXT_ID));
  if (!ext || !ext->call_tool) return "this plugin has no AI tools";
  std::string out;
  ok = agentText([&](char* o, std::uint32_t c) { return ext->call_tool(d.plugin, name.c_str(), argsJson.c_str(), o, c); }, out);
  return out;
}

double ClapInstance::parameterNormalized(unsigned id) const {
  auto& d = *impl_;
  if (!d.params) return 0.0;
  for (const auto& i : d.infos) {
    if (i.id != id) continue;
    double v = 0;
    if (!d.params->get_value(d.plugin, id, &v)) return 0.0;
    const double span = i.max_value - i.min_value;
    return span != 0 ? (v - i.min_value) / span : 0.0;
  }
  return 0.0;
}

bool ClapInstance::setParameterNormalized(unsigned id, double normalized) {
  auto& d = *impl_;
  for (std::size_t k = 0; k < d.infos.size(); ++k) {
    if (d.infos[k].id != id) continue;
    const double n = std::clamp(normalized, 0.0, 1.0);
    const double plain = d.infos[k].min_value + n * (d.infos[k].max_value - d.infos[k].min_value);
    return d.ring.push({static_cast<std::uint32_t>(k), plain});
  }
  return false;
}

bool ClapInstance::automateParameter(unsigned id, double normalized) {   // audio thread: infos is read-only after load
  auto& d = *impl_;
  for (std::size_t k = 0; k < d.infos.size(); ++k) {
    if (d.infos[k].id != id) continue;
    const double n = std::clamp(normalized, 0.0, 1.0);
    return d.ring.tryPush({static_cast<std::uint32_t>(k), d.infos[k].min_value + n * (d.infos[k].max_value - d.infos[k].min_value)});
  }
  return false;
}

std::vector<std::uint8_t> ClapInstance::saveState() const {
  auto& d = *impl_;
  if (!d.state) return {};
  std::vector<std::uint8_t> bytes;
  clap_ostream_t os = {};
  os.ctx = &bytes;
  os.write = [](const clap_ostream_t* s, const void* buf, std::uint64_t n) -> std::int64_t {
    auto* v = static_cast<std::vector<std::uint8_t>*>(s->ctx);
    const auto* p = static_cast<const std::uint8_t*>(buf);
    v->insert(v->end(), p, p + n);
    return static_cast<std::int64_t>(n);
  };
  if (!d.state->save(d.plugin, &os)) return {};
  return bytes;
}

bool ClapInstance::loadState(const std::vector<std::uint8_t>& state) {
  auto& d = *impl_;
  if (!d.state || state.empty()) return false;
  struct Src { const std::vector<std::uint8_t>* v; std::size_t pos; } src{&state, 0};
  clap_istream_t is = {};
  is.ctx = &src;
  is.read = [](const clap_istream_t* s, void* buf, std::uint64_t n) -> std::int64_t {
    auto* x = static_cast<Src*>(s->ctx);
    const std::size_t left = x->v->size() - x->pos;
    const std::size_t take = std::min<std::size_t>(left, n);
    std::memcpy(buf, x->v->data() + x->pos, take);
    x->pos += take;
    return static_cast<std::int64_t>(take);
  };
  return d.state->load(d.plugin, &is);
}

}  // namespace wezt::plugins
