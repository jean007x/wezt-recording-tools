/* Wezt Agent Interface, version 1.
 *
 * A way for an audio plugin to tell Wezt (and, through Wezt, any AI agent that speaks the Model
 * Context Protocol) what its own AI features can do, and to let that agent call them.
 *
 * Plain C, no dependencies, MIT-style: copy this file into your plugin. It is a CLAP extension:
 * return a pointer to a `wezt_plugin_agent_t` from `clap_plugin_t::get_extension` when asked for
 * WEZT_AGENT_EXT_ID. Plugins that do not implement it still work: every parameter they publish
 * through the normal plugin format (VST3, CLAP) is controllable by agents with no extra work.
 *
 * Both calls happen on the host's main (control) thread, never on the audio thread, so they may
 * allocate and take locks. They must not block for long; start a job and return a status if the work
 * is slow. Text is UTF-8 JSON.
 *
 * list_tools returns an array of tools:
 *   [ {"name":"suggest_eq","description":"Listen to the input and propose EQ settings",
 *      "input_schema":{"type":"object","properties":{"target":{"type":"string"}},"required":[]}} ]
 * call_tool receives the tool name and a JSON object of arguments and writes a JSON or plain-text
 * result. Both copy at most cap-1 bytes into `out`, NUL-terminate when cap > 0, and return the number
 * of bytes the full text needs (so the host can call again with a bigger buffer). call_tool returns
 * WEZT_AGENT_FAILED when the call failed; the text it wrote then says why.
 *
 * Safety: the host treats everything an agent sends as untrusted. Validate arguments, never run
 * shell commands or open arbitrary paths from them, and keep tools reversible where you can.
 */
#ifndef WEZT_AGENT_H
#define WEZT_AGENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEZT_AGENT_EXT_ID "wezt.agent/1"
#define WEZT_AGENT_FAILED 0xFFFFFFFFu

struct clap_plugin; /* from <clap/clap.h>; only a pointer is used here */

typedef struct wezt_plugin_agent {
  uint32_t (*list_tools)(const struct clap_plugin* plugin, char* out, uint32_t cap);
  uint32_t (*call_tool)(const struct clap_plugin* plugin, const char* name, const char* args_json, char* out, uint32_t cap);
} wezt_plugin_agent_t;

#ifdef __cplusplus
}
#endif
#endif /* WEZT_AGENT_H */
