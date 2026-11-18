// WinMM MIDI input. Written without a Windows machine to test on.
#if defined(_WIN32)
#include "midi/midi_input.h"
#include <windows.h>
#include <mmsystem.h>
namespace wezt::midi {
namespace {
class WinMidi final : public IMidiInput {
 public:
  explicit WinMidi(UINT id) : id_(id) {}
  ~WinMidi() override { stop(); }
  std::string name() const override {
    MIDIINCAPSA c{}; if (midiInGetDevCapsA(id_, &c, sizeof c) == MMSYSERR_NOERROR) return c.szPname; return "MIDI input";
  }
  bool start(std::string* error) override {
    if (h_) return true;
    if (midiInOpen(&h_, id_, reinterpret_cast<DWORD_PTR>(&WinMidi::cb), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
      h_ = nullptr; if (error) *error = "Cannot open the MIDI input"; return false; }
    midiInStart(h_); return true;
  }
  void stop() override { if (h_) { midiInStop(h_); midiInReset(h_); midiInClose(h_); h_ = nullptr; } }
 private:
  static void CALLBACK cb(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
    if (msg != MIM_DATA) return;   // sysex is ignored
    auto* self = reinterpret_cast<WinMidi*>(inst);
    const std::uint8_t st = p1 & 0xFF;
    MidiParser parser; MidiMessage m;
    const std::uint8_t bytes[3] = {st, static_cast<std::uint8_t>((p1 >> 8) & 0xFF), static_cast<std::uint8_t>((p1 >> 16) & 0xFF)};
    for (std::uint8_t b : bytes) if (parser.feed(b, m)) { self->queue_.push(m); break; }
  }
  UINT id_; HMIDIIN h_ = nullptr;
};
}  // namespace
std::vector<std::string> listMidiInputs() {
  std::vector<std::string> v; const UINT n = midiInGetNumDevs();
  for (UINT i = 0; i < n; ++i) { MIDIINCAPSA c{}; midiInGetDevCapsA(i, &c, sizeof c); v.push_back(c.szPname); }
  return v;
}
std::unique_ptr<IMidiInput> openMidiInput(std::size_t i) { return i < midiInGetNumDevs() ? std::make_unique<WinMidi>(static_cast<UINT>(i)) : nullptr; }
}  // namespace wezt::midi
#endif
