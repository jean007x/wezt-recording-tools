// Hardware MIDI keyboard input.
//
//   MidiParser   raw MIDI bytes -> messages (running status, realtime bytes inside messages, sysex skipped)
//   MidiQueue    single-producer / single-consumer ring: the device thread pushes, the UI/audio side pops
//   IMidiInput   a device; openRawMidi() reads any raw MIDI byte stream (ALSA /dev/snd/midiC*D*, a FIFO in tests)
//   WinMM and CoreMIDI backends are compiled on their platforms only (written without access to those systems: untested).
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wezt::midi {

struct MidiMessage {
  enum Type : std::uint8_t { NoteOff, NoteOn, PolyPressure, Control, Program, ChannelPressure, PitchBend, Transport };   // Transport: MIDI Machine Control (sysex) and the realtime Start/Continue/Stop bytes
  Type type = NoteOn;
  std::uint8_t channel = 0;  // 0..15
  std::uint8_t a = 0;        // note / controller / program / pressure
  std::uint8_t b = 0;        // velocity / value (for PitchBend: low 7 bits in a, high in b)
  // For Transport: `a` is one of these, and for kLocate `arg` is the target in milliseconds.
  enum : std::uint8_t { kStop = 1, kPlay = 2, kRecord = 3, kRecordExit = 4, kPause = 5, kLocate = 6 };
  std::uint32_t arg = 0;
  int pitchBendValue() const { return (static_cast<int>(b) << 7 | a) - 8192; }  // -8192..8191
};

class MidiParser {
 public:
  // Feeds one byte; returns true and fills `out` when a complete channel message is ready.
  // A NoteOn with velocity 0 is reported as NoteOff. System messages are ignored, except MIDI Machine Control (F0 7F dev 06 ...) and realtime Start (FA), Continue (FB) and Stop (FC), which come out as Transport messages.
  bool feed(std::uint8_t byte, MidiMessage& out);
  void reset();
 private:
  std::uint8_t status_ = 0;   // running status (channel messages only)
  std::uint8_t data_[2] = {0, 0};
  int have_ = 0, need_ = 0;
  bool inSysex_ = false;
  std::uint8_t sysex_[16] = {0};
  int sysexLen_ = 0;
};

class MidiQueue {
 public:
  bool push(const MidiMessage& m);   // producer thread; false when full (the message is dropped and counted)
  bool pop(MidiMessage& m);          // consumer thread
  std::size_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
 private:
  static constexpr std::size_t kSize = 1024;  // power of two
  std::array<MidiMessage, kSize> buf_{};
  std::atomic<std::size_t> head_{0}, tail_{0};
  std::atomic<std::size_t> dropped_{0};
};

class IMidiInput {
 public:
  virtual ~IMidiInput() = default;
  virtual bool start(std::string* error) = 0;
  virtual void stop() = 0;
  virtual std::string name() const = 0;
  MidiQueue& queue() { return queue_; }
 protected:
  MidiQueue queue_;
};

// Reads raw MIDI bytes from a file/device/FIFO on a background thread (POSIX only; null elsewhere).
std::unique_ptr<IMidiInput> openRawMidi(const std::string& path);
// Lists the MIDI input devices this platform can see (paths for raw devices, names for the others).
std::vector<std::string> listMidiInputs();
// Opens the nth entry of listMidiInputs() with the platform backend.
std::unique_ptr<IMidiInput> openMidiInput(std::size_t index);

}  // namespace wezt::midi
