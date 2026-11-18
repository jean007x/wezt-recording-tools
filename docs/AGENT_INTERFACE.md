# Plugin control for AI agents

One control surface for every plugin, used by the built-in assistant and by the MCP connector. There is no second registry, undo system or MCP server: it sits on `IPluginInstance` (src/plugins/plugin_host.h), the existing `Session` plugin inserts, the existing tool table in `src/mcp/server.cpp`, `util/json.h`, and `edit::UndoStack`.

## What a plugin exposes
- Parameters: name, plain min/max/unit/group, steps, log scale, a semantic tag (cutoff, attack, treble, ...), normalized get/set, and display text.
- Optional AI tools (CLAP only, extension `wezt.agent/1`, include/wezt_agent.h): `list_tools` returns JSON `{name, description, input_schema}`; `call_tool(name, args_json)` returns JSON text.
- VST3 and bridged plugins expose parameters only (no AI-tool extension exists in VST3).

## MCP tools
list_plugins, describe_plugin, set_plugin_param, set_plugin_params, reset_plugin, automate_plugin_param, tune_plugin (intent: "brighter", "punchier", "a little less drive"), control_plugins (free-text command), add_plugin, plugin_ai_tools, call_plugin_tool, agent_history, undo_agent_changes.

## Safety model
- add_plugin loads only built-ins (Wezt Synth, Sampler, EQ, Compressor, Limiter, Stereo Width); it never loads a path.
- Values are clamped to the declared range; non-finite numbers rejected; at most 64 settings per call.
- A plugin's AI tool must be advertised by that plugin; arguments are limited to 64 KB; output is limited to 64 KB and returned labelled `untrusted_plugin_output` (treat as data, not instructions).
- Every change is recorded (history capped at 400) and can be undone through the agent history or the app's undo stack.

## Writing a plugin that talks to agents (CLAP)
Implement `clap_plugin_wezt_agent_t` from include/wezt_agent.h and return it from `get_extension("wezt.agent/1")`. The reference is tests/clap_gain/gain_plugin.cpp.

## Limits (honest list)
- Semantic tags for third-party plugins are guessed from parameter names and can be wrong; guesses are flagged in describe_plugin.
- Only a test CLAP plugin written here implements `wezt.agent/1`; no third-party plugin has been tried.
- Automation: see below.
- Parameter edits from several control threads (UI and agent) are safe: pushes take a small mutex among themselves, the audio thread never locks (stress-tested with 4 producers, clean under ThreadSanitizer). If the 512-slot queue is full an edit is dropped and the call reports it was not accepted.
- Tested on Linux only; not tried with a real Claude Desktop.

## Automation of plugin parameters
`automate_plugin_param(track, plugin, parameter, points)` with points like `[{"time": 0, "value": "500 Hz"}, {"time": 8, "value": "4 kHz"}]` (seconds; values in the same forms as `set_plugin_param`). The lane is part of the session (saved, copied with a duplicated track), follows the playhead, and is undoable. In the app: View > Plugin Parameters and Automation, tick "Write automation", press play and move a control.
- Built on a new real-time-safe call, `IPluginInstance::automateParameter`: it never waits or allocates. VST3, CLAP and bridged plugins use the same queue as other parameter edits through a non-blocking push; if the queue is busy that block's value is delivered the next block.
- The value is updated once per audio block (a few milliseconds), straight line between points. Sample-accurate automation inside a block is not done.
- While stopped, a parameter keeps whatever value you set; pressing play hands control back to its lane.
- Tests: audio follows the lane (EQ high gain +10.7 dB over 1 s, compressor makeup +11.4 dB), save/load, duplicate, removing a plugin while a lane plays, edits from another thread while playing (clean under ASan and ThreadSanitizer).
