// A tiny CLAP gain plugin used as a test subject for Wezt's CLAP host. Written from the public
// CLAP headers; implements parameters, state, latency and audio-port extensions.
#include <clap/clap.h>

#include "wezt_agent.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace {

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};

const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT,
                                        "com.wezt.test.gain",
                                        "Wezt Test Gain",
                                        "Wezt",
                                        "https://example.invalid",
                                        "",
                                        "",
                                        "1.0.0",
                                        "Test plugin: one gain parameter",
                                        kFeatures};

struct Gain {
  clap_plugin_t plugin;
  const clap_host_t* host;
  double gain = 1.0;
  std::uint32_t latency = 0;
};
Gain* self(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

void handleEvents(Gain* g, const clap_input_events_t* ev) {
  if (!ev) return;
  for (std::uint32_t i = 0; i < ev->size(ev); ++i) {
    const clap_event_header_t* h = ev->get(ev, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
      const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(h);
      if (pv->param_id == 0) g->gain = pv->value;
    }
  }
}

// ---- extensions ----
std::uint32_t portsCount(const clap_plugin_t*, bool) { return 1; }
bool portsGet(const clap_plugin_t*, std::uint32_t index, bool, clap_audio_port_info_t* info) {
  if (index != 0) return false;
  info->id = 0;
  std::snprintf(info->name, sizeof info->name, "main");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

std::uint32_t paramsCount(const clap_plugin_t*) { return 1; }
bool paramsInfo(const clap_plugin_t*, std::uint32_t i, clap_param_info_t* info) {
  if (i != 0) return false;
  std::memset(info, 0, sizeof *info);
  info->id = 0;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  std::snprintf(info->name, sizeof info->name, "Gain");
  info->min_value = 0.0;
  info->max_value = 1.0;
  info->default_value = 1.0;
  return true;
}
bool paramsValue(const clap_plugin_t* p, clap_id id, double* out) {
  if (id != 0) return false;
  *out = self(p)->gain;
  return true;
}
bool paramsToText(const clap_plugin_t*, clap_id, double v, char* buf, std::uint32_t n) {
  std::snprintf(buf, n, "%.3f", v);
  return true;
}
bool paramsFromText(const clap_plugin_t*, clap_id, const char* text, double* out) {
  *out = std::atof(text);
  return true;
}
void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) { handleEvents(self(p), in); }
const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush};

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) {
  const double g = self(p)->gain;
  return s->write(s, &g, sizeof g) == static_cast<std::int64_t>(sizeof g);
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) {
  double g = 0;
  if (s->read(s, &g, sizeof g) != static_cast<std::int64_t>(sizeof g)) return false;
  self(p)->gain = g;
  return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

// ---- wezt.agent/1: what a plugin with its own AI would publish ----
// Writes `text` into out (truncating to cap-1, NUL-terminated) and returns the full length needed.
std::uint32_t agentWrite(const std::string& text, char* out, std::uint32_t cap) {
  if (cap > 0) {
    const std::size_t n = std::min<std::size_t>(text.size(), cap - 1);
    std::memcpy(out, text.data(), n);
    out[n] = 0;
  }
  return static_cast<std::uint32_t>(text.size());
}
std::uint32_t agentList(const clap_plugin_t*, char* out, std::uint32_t cap) {
  return agentWrite(
      "[{\"name\":\"set_gain_db\",\"description\":\"Set the output gain in decibels\","
      "\"input_schema\":{\"type\":\"object\",\"properties\":{\"gain_db\":{\"type\":\"number\"}},\"required\":[\"gain_db\"]}},"
      "{\"name\":\"set_latency\",\"description\":\"Test hook: change the reported latency\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"samples\":{\"type\":\"number\"}}}},"
      "{\"name\":\"big_report\",\"description\":\"Returns a long text (tests large answers)\",\"input_schema\":{\"type\":\"object\"}},"
      "{\"name\":\"always_fails\",\"description\":\"Fails on purpose\",\"input_schema\":{\"type\":\"object\"}}]",
      out, cap);
}
std::uint32_t agentCall(const clap_plugin_t* p, const char* name, const char* args, char* out, std::uint32_t cap) {
  Gain* g = self(p);
  if (!std::strcmp(name, "set_gain_db")) {
    const char* k = std::strstr(args, "\"gain_db\"");
    const char* colon = k ? std::strchr(k, ':') : nullptr;
    if (!colon) { agentWrite("gain_db is required", out, cap); return WEZT_AGENT_FAILED; }
    const double db = std::atof(colon + 1);
    g->gain = std::pow(10.0, db / 20.0);
    return agentWrite("gain set to " + std::to_string(db) + " dB", out, cap);
  }
  if (!std::strcmp(name, "set_latency")) {   // test hook: change the latency and announce it, as a look-ahead plug-in would
    const char* k = std::strstr(args, "\"samples\"");
    const char* colon = k ? std::strchr(k, ':') : nullptr;
    g->latency = colon ? static_cast<std::uint32_t>(std::atoi(colon + 1)) : 0;
    if (g->host && g->host->get_extension) {
      const auto* hl = static_cast<const clap_host_latency_t*>(g->host->get_extension(g->host, CLAP_EXT_LATENCY));
      if (hl && hl->changed) hl->changed(g->host);
    }
    return agentWrite("latency set", out, cap);
  }
  if (!std::strcmp(name, "big_report")) return agentWrite(std::string(40000, 'x'), out, cap);
  agentWrite("this tool always fails", out, cap);
  return WEZT_AGENT_FAILED;
}
const wezt_plugin_agent_t kAgent = {agentList, agentCall};

std::uint32_t latencyGet(const clap_plugin_t* p) { return self(p)->latency; }
const clap_plugin_latency_t kLatency = {latencyGet};

// ---- plugin ----
bool pInit(const clap_plugin_t*) { return true; }
void pDestroy(const clap_plugin_t* p) { delete self(p); }
bool pActivate(const clap_plugin_t*, double, std::uint32_t, std::uint32_t) { return true; }
void pDeactivate(const clap_plugin_t*) {}
bool pStart(const clap_plugin_t*) { return true; }
void pStop(const clap_plugin_t*) {}
void pReset(const clap_plugin_t*) {}
clap_process_status pProcess(const clap_plugin_t* p, const clap_process_t* proc) {
  Gain* g = self(p);
  handleEvents(g, proc->in_events);
  if (proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;
  const clap_audio_buffer_t& in = proc->audio_inputs[0];
  clap_audio_buffer_t& out = proc->audio_outputs[0];
  const float k = static_cast<float>(g->gain);
  for (std::uint32_t c = 0; c < out.channel_count && c < in.channel_count; ++c)
    for (std::uint32_t i = 0; i < proc->frames_count; ++i) out.data32[c][i] = in.data32[c][i] * k;
  return CLAP_PROCESS_CONTINUE;
}
const void* pExt(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
  if (!std::strcmp(id, WEZT_AGENT_EXT_ID)) return &kAgent;
  return nullptr;
}
void pMainThread(const clap_plugin_t*) {}

// ---- factory / entry ----
std::uint32_t fCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, std::uint32_t i) { return i == 0 ? &kDesc : nullptr; }
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDesc.id) != 0) return nullptr;
  auto* g = new Gain();
  g->host = host;
  g->plugin = {&kDesc, g, pInit, pDestroy, pActivate, pDeactivate, pStart, pStop, pReset, pProcess, pExt, pMainThread};
  return &g->plugin;
}
const clap_plugin_factory_t kFactory = {fCount, fDesc, fCreate};

bool eInit(const char*) { return true; }
void eDeinit() {}
const void* eFactory(const char* id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, eInit, eDeinit, eFactory};
