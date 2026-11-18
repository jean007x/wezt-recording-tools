# Plugin Formats

What Wezt Recording Tools can do with each format, as of this build. "Tested" means a test in
`tests/` runs it; it does not mean it has been run on every OS.

| Format | Windows | macOS | Status |
|---|---|---|---|
| VST3 | yes | yes | **Hosted and tested** against real plugins, including note input for instruments (`test_vst3_midi`): on Linux, Windows and macOS in CI, using the Steinberg SDK's sample plug-ins. Third-party commercial plug-ins are untested. |
| CLAP | yes | yes | **Hosted and tested** against real CLAP plugin binaries, including a CLAP synth driven by note events (`test_clap_midi`): on Linux, Windows and macOS in CI. |
| 32-bit plugins | yes | no | **Bridge built and tested** with a same-bitness helper: output is bit-identical to running in-process, and a crashed helper yields silence instead of taking the session down. A real 32-bit helper has **not** been built or run. |
| AU | no | yes | Found by the scanner; **no loader yet**. |
| VST2 | yes | yes | Found by the scanner; **no loader yet**. Steinberg no longer licenses the VST2 SDK, so a loader needs your own headers. |
| AAX | yes | yes | **Not hostable.** Found, listed, and matched to a hostable twin. |
| RTAS, TDM | yes | yes | **Not hostable.** Found, listed, and explained. |

## Building the hosts

```
git clone --recursive https://github.com/steinbergmedia/vst3sdk
git clone https://github.com/free-audio/clap
cmake -S . -B build -DWEZT_ENABLE_VST3=ON -DWEZT_VST3_SDK_DIR=<vst3sdk> \
                    -DWEZT_ENABLE_CLAP=ON -DWEZT_CLAP_DIR=<clap> -DWEZT_ENABLE_BRIDGE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Both SDKs are MIT licensed. The VST3 SDK needs CMake 3.25+. To run the plugin tests against a
real VST3, add `-DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=ON` and
`-DWEZT_VST3_TEST_PLUGIN=<path to again-sample-accurate.vst3>`.

MIDI note, controller and pitch-bend events are supported in both hosts, so VST3 and CLAP instruments work
(`acceptsMidi()` / `handleMidi()`; the built-in Sampler and Wezt Synth use the same path). Not supported yet in the
hosts: side-chain buses (extra input ports are fed silence) and plug-in editor windows (parameters are edited in
Wezt's own Plugin Parameters window).

## "Every plugin installed and found" (`PluginLibrary`)

`PluginLibrary::scan()` looks in the standard system and per-user folders for every format on
the current OS and reports, for each plugin: its format, its bitness (read from the file header,
never by loading it), and one of four verdicts with a plain-language reason:

- **Native**: loads in-process.
- **NeedsBridge**: a 32-bit plugin; runs through the bridge on Windows.
- **NotBuilt**: a format we intend to host, but this build lacks the loader.
- **NotHostable**: Pro Tools-only formats.

`PluginLibrary::install()` copies a plugin file or bundle into the per-user folder for its
format. It does not run vendor installers (`.exe`, `.msi`, `.pkg`) and refuses Pro Tools-only
formats, saying so.

## The 32-bit bridge

A 64-bit process cannot load a 32-bit plugin, so a small helper (`wezt-bridge`), built with the
plugin's bitness, hosts it. Audio, parameters and state travel over shared memory. A block that
takes too long is replaced by silence and counted, so a hung plugin cannot stall the audio thread.

**It does not make plugins sound richer.** The bridge passes 32-bit float audio through
untouched, and the test confirms the output is identical (difference 0) to the plugin run
natively. Running a plugin "in 64-bit" changes its address space, not its sound. Whatever
character a 32-bit plugin has, it keeps. Processing that does change the sound (oversampling,
saturation) would be an explicit effect, and would be audible as such.

To use one on Windows: build `wezt-bridge` as 32-bit (`cmake -A Win32`) and set
`BridgeConfig::helperExecutable` to it. macOS dropped 32-bit support in 10.15, so there is no
32-bit path there.

## AAX, RTAS and TDM

These cannot be hosted by a third-party program, and that is a licensing and technical fact, not
a gap more engineering would close.

- **AAX** plugins are built against Avid's AAX SDK for Pro Tools' own host interfaces. Avid
  licenses that SDK to plugin makers (with PACE signing) and publishes no host-side SDK.
- **RTAS** is a retired Pro Tools format: 32-bit, and tied to Pro Tools' internal plugin
  interfaces.
- **TDM** runs on Avid HD DSP cards, hardware a software host does not have.

Hosting any of them would mean emulating Pro Tools' interfaces by reverse engineering Avid's
software, which this project's clean-room policy and Avid's licence rule out. RTAS and TDM share
the `.dpm` extension, so the scanner reports them as one "RTAS/TDM" kind without opening the file.

What Wezt does instead: it lists them, then looks for the same plugin installed as VST3, CLAP or
AU (preferring VST3) and offers that. Most commercial plugins ship in several formats from one
codebase, so a hostable twin is usually already on the machine. The only legitimate route to
real AAX hosting is a licence from Avid.
