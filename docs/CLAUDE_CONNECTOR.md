# Letting Claude drive Wezt (wezt-mcp)

`wezt-mcp` is a small local program that speaks the Model Context Protocol over stdin/stdout.
An assistant that supports local MCP servers (Claude Desktop, Claude Code) starts it and can then
compose a song, add parts, find mix clashes, sidechain, master and export.

**Be clear about the limit:** claude.ai in a web browser cannot reach programs on your computer.
Use Claude Desktop or Claude Code. Nothing here opens a network port; the program only reads and
writes the work folder (default `~/.wezt/mcp`).

## Claude Desktop
Add to `claude_desktop_config.json` (use the real path to the built program), then restart Claude:

    {
      "mcpServers": {
        "wezt": { "command": "/path/to/wezt-mcp", "args": ["/path/to/work-folder"] }
      }
    }

## Claude Code
    claude mcp add wezt -- /path/to/wezt-mcp /path/to/work-folder

## Tools
new_session, get_state, suggest_chords, compose_song, add_part, add_audio_file (.wav only),
set_track, separate_stems, audio_to_midi, analyze_mix, apply_mix_fixes, sidechain, analyze_audio,
fit_vocal, pitch_audio, make_choir, train_voice_model, convert_voice, list_voice_models, build_voice_bank, stretch_audio, beat_detective, edit_midi, sing_lyrics, list_plugins, describe_plugin, set_plugin_param, set_plugin_params, automate_plugin_param, reset_plugin, tune_plugin, control_plugins, add_plugin, plugin_ai_tools, call_plugin_tool, agent_history, undo_agent_changes (see AGENT_INTERFACE.md), render_mix, master, save_session, ask_tutor.

Try: "Make a 16-bar lo-fi chill song in A minor, duck the bass under the drums, then master it for streaming."
The result is `.wezt` + stems + `mix.wav` + `song.mid` in the work folder; open the `.wezt` in the app.

## Not verified
The server passes scripted JSON-RPC tests (and sanitizers) but has not been run against a real
Claude Desktop here.

## Building a voice bank from one recording

`split_voice_takes` splits a .wav in the work folder (many sounds with pauses between them) into takes and measures each.
`build_voice_bank_from_takes` names the takes from what the person says they recorded, in order (for example
`"0=s,1=t,2=m,4=eh,5=ah"`), and saves a bank. Options: `derive` (fill consonants nobody recorded from the person's own
sounds), `short_vowels` (vowels inside spoken letter names). It needs `consent=true`, which Claude must only set after the
person confirmed the voice is theirs or the speaker agreed. Files are read from the work folder only.
