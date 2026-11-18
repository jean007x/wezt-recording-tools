// A tiny CLAP instrument used as a test subject for Wezt's MIDI delivery. Written from the public CLAP
// headers. It exports two plug-ins from one library: one that takes CLAP note events and one that
// takes only raw MIDI bytes, so both dialects of the host are exercised. Eight sine voices; a note-off
// (or MIDI CC 123) silences a voice at once.
#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
const clap_plugin_descriptor_t kDescNote = {CLAP_VERSION_INIT, "com.wezt.test.synth", "Wezt Test Synth", "Wezt", "", "", "", "1.0.0", "Test instrument (note events)", kFeatures};
const clap_plugin_descriptor_t kDescMidi = {CLAP_VERSION_INIT, "com.wezt.test.synth.midi", "Wezt Test Synth (MIDI)", "Wezt", "", "", "", "1.0.0", "Test instrument (MIDI bytes)", kFeatures};

struct Voice { bool on = false; int key = 0; double phase = 0, inc = 0, amp = 0; };
struct Synth {
  clap_plugin_t plugin;
  const clap_host_t* host;
  bool midiOnly = false;
  double rate = 48000;
  Voice v[8];
  int notesSeen = 0, midiSeen = 0;
};
Synth* self(const clap_plugin_t* p) { return static_cast<Synth*>(p->plugin_data); }

void noteOn(Synth* s, int key, double vel) {
  Voice* pick = nullptr;
  for (auto& x : s->v) if (!x.on) { pick = &x; break; }
  if (!pick) pick = &s->v[0];
  pick->on = true; pick->key = key; pick->phase = 0; pick->amp = 0.5 * vel;
  pick->inc = 440.0 * std::pow(2.0, (key - 69) / 12.0) / s->rate;
}
void noteOff(Synth* s, int key) { for (auto& x : s->v) if (x.on && x.key == key) x.on = false; }

void handle(Synth* s, const clap_event_header_t* h) {
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (h->type == CLAP_EVENT_NOTE_ON) { const auto* n = reinterpret_cast<const clap_event_note_t*>(h); ++s->notesSeen; noteOn(s, n->key, n->velocity); }
  else if (h->type == CLAP_EVENT_NOTE_OFF) { const auto* n = reinterpret_cast<const clap_event_note_t*>(h); noteOff(s, n->key); }
  else if (h->type == CLAP_EVENT_MIDI) {
    const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
    ++s->midiSeen;
    const int st = m->data[0] & 0xF0;
    if (st == 0x90 && m->data[2] > 0) noteOn(s, m->data[1], m->data[2] / 127.0);
    else if (st == 0x80 || st == 0x90) noteOff(s, m->data[1]);
    else if (st == 0xB0 && m->data[1] == 123) for (auto& x : s->v) x.on = false;
  }
}

// ---- extensions ----
std::uint32_t apCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }
bool apGet(const clap_plugin_t*, std::uint32_t i, bool isInput, clap_audio_port_info_t* info) {
  if (i != 0 || isInput) return false;
  info->id = 0; std::snprintf(info->name, sizeof info->name, "out"); info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2; info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kAudioPorts = {apCount, apGet};
std::uint32_t npCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool npGet(const clap_plugin_t* p, std::uint32_t i, bool isInput, clap_note_port_info_t* info) {
  if (i != 0 || !isInput) return false;
  info->id = 0;
  info->supported_dialects = self(p)->midiOnly ? CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_CLAP;
  info->preferred_dialect = info->supported_dialects;
  std::snprintf(info->name, sizeof info->name, "notes");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {npCount, npGet};

// ---- plugin ----
bool pInit(const clap_plugin_t*) { return true; }
void pDestroy(const clap_plugin_t* p) { delete self(p); }
bool pActivate(const clap_plugin_t* p, double sr, std::uint32_t, std::uint32_t) { self(p)->rate = sr; return true; }
void pDeactivate(const clap_plugin_t*) {}
bool pStart(const clap_plugin_t*) { return true; }
void pStop(const clap_plugin_t*) {}
void pReset(const clap_plugin_t* p) { for (auto& x : self(p)->v) x.on = false; }
clap_process_status pProcess(const clap_plugin_t* p, const clap_process_t* proc) {
  Synth* s = self(p);
  if (proc->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;
  clap_audio_buffer_t& out = proc->audio_outputs[0];
  const std::uint32_t n = proc->frames_count, total = proc->in_events ? proc->in_events->size(proc->in_events) : 0;
  std::uint32_t ei = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    while (ei < total) {
      const clap_event_header_t* h = proc->in_events->get(proc->in_events, ei);
      if (h->time > i) break;
      handle(s, h);
      ++ei;
    }
    float acc = 0.f;
    for (auto& x : s->v) if (x.on) { acc += static_cast<float>(x.amp * std::sin(2.0 * 3.14159265358979 * x.phase)); x.phase += x.inc; if (x.phase >= 1.0) x.phase -= 1.0; }
    for (std::uint32_t c = 0; c < out.channel_count; ++c) out.data32[c][i] = acc;
  }
  while (ei < total) handle(s, proc->in_events->get(proc->in_events, ei++));
  return CLAP_PROCESS_CONTINUE;
}
const void* pExt(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  return nullptr;
}
void pMainThread(const clap_plugin_t*) {}

std::uint32_t fCount(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, std::uint32_t i) { return i == 0 ? &kDescNote : i == 1 ? &kDescMidi : nullptr; }
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!clap_version_is_compatible(host->clap_version)) return nullptr;
  const bool midi = !std::strcmp(id, kDescMidi.id);
  if (!midi && std::strcmp(id, kDescNote.id) != 0) return nullptr;
  auto* s = new Synth();
  s->host = host; s->midiOnly = midi;
  s->plugin = {midi ? &kDescMidi : &kDescNote, s, pInit, pDestroy, pActivate, pDeactivate, pStart, pStop, pReset, pProcess, pExt, pMainThread};
  return &s->plugin;
}
const clap_plugin_factory_t kFactory = {fCount, fDesc, fCreate};
bool eInit(const char*) { return true; }
void eDeinit() {}
const void* eFactory(const char* id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, eInit, eDeinit, eFactory};
