// CoreMIDI input. Written without a Mac to test on.
#if defined(__APPLE__)
#include "midi/midi_input.h"
#include <CoreMIDI/CoreMIDI.h>
namespace wezt::midi {
namespace {
class MacMidi final : public IMidiInput {
 public:
  explicit MacMidi(ItemCount i) : src_(MIDIGetSource(i)) {}
  ~MacMidi() override { stop(); }
  std::string name() const override {
    CFStringRef s = nullptr; char buf[128] = "MIDI input";
    if (MIDIObjectGetStringProperty(src_, kMIDIPropertyDisplayName, &s) == noErr && s) { CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(s); }
    return buf;
  }
  bool start(std::string* error) override {
    if (port_) return true;
    if (MIDIClientCreate(CFSTR("Wezt"), nullptr, nullptr, &client_) != noErr ||
        MIDIInputPortCreate(client_, CFSTR("in"), &MacMidi::read, this, &port_) != noErr ||
        MIDIPortConnectSource(port_, src_, nullptr) != noErr) { port_ = 0; if (error) *error = "Cannot open the MIDI input"; return false; }
    return true;
  }
  void stop() override { if (port_) { MIDIPortDisconnectSource(port_, src_); MIDIPortDispose(port_); port_ = 0; } if (client_) { MIDIClientDispose(client_); client_ = 0; } }
 private:
  static void read(const MIDIPacketList* l, void* self_, void*) {
    auto* self = static_cast<MacMidi*>(self_);
    const MIDIPacket* p = &l->packet[0];
    for (UInt32 i = 0; i < l->numPackets; ++i) {
      MidiMessage m;
      for (UInt16 k = 0; k < p->length; ++k) if (self->parser_.feed(p->data[k], m)) self->queue_.push(m);
      p = MIDIPacketNext(p);
    }
  }
  MIDIEndpointRef src_; MIDIClientRef client_ = 0; MIDIPortRef port_ = 0; MidiParser parser_;
};
class MacMidiOut final : public IMidiOutput {
 public:
  explicit MacMidiOut(ItemCount i) : dst_(MIDIGetDestination(i)) {}
  ~MacMidiOut() override { stop(); }
  std::string name() const override {
    CFStringRef s = nullptr; char buf[128] = "MIDI output";
    if (MIDIObjectGetStringProperty(dst_, kMIDIPropertyDisplayName, &s) == noErr && s) { CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(s); }
    return buf;
  }
  bool start(std::string* error) override {
    if (port_) return true;
    if (MIDIClientCreate(CFSTR("Wezt out"), nullptr, nullptr, &client_) != noErr || MIDIOutputPortCreate(client_, CFSTR("out"), &port_) != noErr) {
      port_ = 0; if (error) *error = "Cannot open the MIDI output"; return false; }
    return true;
  }
  void stop() override { if (port_) { MIDIPortDispose(port_); port_ = 0; } if (client_) { MIDIClientDispose(client_); client_ = 0; } }
  bool send(const MidiMessage& m) override {
    std::uint8_t b[3] = {0, 0, 0};
    const int n = encodeMessage(m, b);
    if (!port_ || n == 0) return false;
    Byte buf[64];
    MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(buf);
    MIDIPacket* p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, sizeof buf, p, 0, static_cast<ByteCount>(n), b);
    return p && MIDISend(port_, dst_, list) == noErr;
  }
 private:
  MIDIEndpointRef dst_; MIDIClientRef client_ = 0; MIDIPortRef port_ = 0;
};
}  // namespace
std::vector<std::string> listMidiOutputs() {
  std::vector<std::string> v;
  for (ItemCount i = 0, n = MIDIGetNumberOfDestinations(); i < n; ++i) v.push_back(MacMidiOut(i).name());
  return v;
}
std::unique_ptr<IMidiOutput> openMidiOutput(std::size_t i) { return i < MIDIGetNumberOfDestinations() ? std::make_unique<MacMidiOut>(static_cast<ItemCount>(i)) : nullptr; }
std::vector<std::string> listMidiInputs() {
  std::vector<std::string> v;
  for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; ++i) v.push_back(MacMidi(i).name());
  return v;
}
std::unique_ptr<IMidiInput> openMidiInput(std::size_t i) { return i < MIDIGetNumberOfSources() ? std::make_unique<MacMidi>(static_cast<ItemCount>(i)) : nullptr; }
}  // namespace wezt::midi
#endif
