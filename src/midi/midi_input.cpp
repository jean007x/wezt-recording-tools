#include "midi/midi_input.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace wezt::midi {

bool MidiParser::feed(std::uint8_t b, MidiMessage& out) {
  if (b >= 0xF8) {                                    // realtime: may appear anywhere, never disturbs the message in progress
    if (b == 0xFA || b == 0xFB) { out = MidiMessage{}; out.type = MidiMessage::Transport; out.a = MidiMessage::kPlay; return true; }
    if (b == 0xFC) { out = MidiMessage{}; out.type = MidiMessage::Transport; out.a = MidiMessage::kStop; return true; }
    return false;
  }
  if (b & 0x80) {
    if (b == 0xF0) { inSysex_ = true; status_ = 0; have_ = 0; sysexLen_ = 0; return false; }
    if (b == 0xF7) {
      const bool was = inSysex_;
      inSysex_ = false;
      // MIDI Machine Control: 7F <device> 06 <command> [...]; the device number is ignored (any device, or all call)
      if (!was || sysexLen_ < 4 || sysex_[0] != 0x7F || sysex_[2] != 0x06) return false;
      out = MidiMessage{};
      out.type = MidiMessage::Transport;
      switch (sysex_[3]) {
        case 0x01: out.a = MidiMessage::kStop; return true;
        case 0x02: case 0x03: out.a = MidiMessage::kPlay; return true;       // play, deferred play
        case 0x06: out.a = MidiMessage::kRecord; return true;                // record strobe
        case 0x07: out.a = MidiMessage::kRecordExit; return true;
        case 0x09: out.a = MidiMessage::kPause; return true;
        case 0x44:                                                           // locate, "target" form: 44 06 01 hr mn sc fr sf
          if (sysexLen_ < 10 || sysex_[4] != 0x06 || sysex_[5] != 0x01) return false;
          {
            const int hr = sysex_[6] & 0x1F, type = (sysex_[6] >> 5) & 3;
            const double fps = type == 0 ? 24.0 : type == 1 ? 25.0 : type == 2 ? 29.97 : 30.0;
            const double sec = hr * 3600.0 + sysex_[7] * 60.0 + sysex_[8] + sysex_[9] / fps + (sysexLen_ > 10 ? sysex_[10] : 0) / (fps * 100.0);
            out.a = MidiMessage::kLocate;
            out.arg = static_cast<std::uint32_t>(sec * 1000.0 + 0.5);
          }
          return true;
        default: return false;
      }
    }
    if (b >= 0xF0) { inSysex_ = false; status_ = 0; have_ = 0; return false; }   // other system common: ignored
    inSysex_ = false;
    status_ = b; have_ = 0;
    const int hi = b >> 4;
    need_ = (hi == 0xC || hi == 0xD) ? 1 : 2;
    return false;
  }
  if (inSysex_) { if (sysexLen_ < 16) sysex_[sysexLen_++] = b; return false; }
  if (status_ == 0) return false;                       // stray data byte
  data_[have_++] = b;
  if (have_ < need_) return false;
  have_ = 0;                                           // running status: the next data bytes reuse status_
  const int hi = status_ >> 4;
  out.channel = status_ & 0x0F;
  out.a = data_[0]; out.b = need_ == 2 ? data_[1] : 0;
  switch (hi) {
    case 0x8: out.type = MidiMessage::NoteOff; break;
    case 0x9: out.type = out.b == 0 ? MidiMessage::NoteOff : MidiMessage::NoteOn; break;
    case 0xA: out.type = MidiMessage::PolyPressure; break;
    case 0xB: out.type = MidiMessage::Control; break;
    case 0xC: out.type = MidiMessage::Program; break;
    case 0xD: out.type = MidiMessage::ChannelPressure; break;
    default:  out.type = MidiMessage::PitchBend; break;
  }
  return true;
}

void MidiParser::reset() { status_ = 0; have_ = 0; need_ = 0; inSysex_ = false; }

bool MidiQueue::push(const MidiMessage& m) {
  const std::size_t h = head_.load(std::memory_order_relaxed);
  if (h - tail_.load(std::memory_order_acquire) >= kSize) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
  buf_[h & (kSize - 1)] = m;
  head_.store(h + 1, std::memory_order_release);
  return true;
}

bool MidiQueue::pop(MidiMessage& m) {
  const std::size_t t = tail_.load(std::memory_order_relaxed);
  if (t == head_.load(std::memory_order_acquire)) return false;
  m = buf_[t & (kSize - 1)];
  tail_.store(t + 1, std::memory_order_release);
  return true;
}

#if defined(__unix__) || defined(__APPLE__)
namespace {
class RawMidi final : public IMidiInput {
 public:
  explicit RawMidi(std::string path) : path_(std::move(path)) {}
  ~RawMidi() override { stop(); }
  std::string name() const override { return path_; }
  bool start(std::string* error) override {
    if (running_.load()) return true;
    fd_ = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { if (error) *error = "Cannot open " + path_ + ": " + std::strerror(errno); return false; }
    running_.store(true);
    th_ = std::thread([this] { loop(); });
    return true;
  }
  void stop() override {
    running_.store(false);
    if (th_.joinable()) th_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  }
 private:
  void loop() {
    MidiParser parser;
    std::uint8_t buf[256];
    while (running_.load()) {
      pollfd p{fd_, POLLIN, 0};
      const int r = ::poll(&p, 1, 50);                 // wakes every 50 ms to notice stop()
      if (r <= 0) continue;
      const ssize_t n = ::read(fd_, buf, sizeof buf);
      if (n > 0) {
        for (ssize_t i = 0; i < n; ++i) { MidiMessage m; if (parser.feed(buf[i], m)) queue_.push(m); }
      } else if (n == 0) {                             // writer closed a FIFO: wait for the next one
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } else if (errno != EAGAIN && errno != EINTR) {  // device unplugged
        running_.store(false);
      }
    }
  }
  std::string path_;
  int fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread th_;
};
}  // namespace

std::unique_ptr<IMidiInput> openRawMidi(const std::string& path) { return std::make_unique<RawMidi>(path); }
#else
std::unique_ptr<IMidiInput> openRawMidi(const std::string&) { return nullptr; }
#endif

#if defined(__linux__)
std::vector<std::string> listMidiInputs() {
  std::vector<std::string> out;
  if (DIR* d = ::opendir("/dev/snd")) {
    while (const dirent* e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (n.rfind("midiC", 0) == 0) out.push_back("/dev/snd/" + n);
    }
    ::closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}
std::unique_ptr<IMidiInput> openMidiInput(std::size_t i) {
  const auto l = listMidiInputs();
  return i < l.size() ? openRawMidi(l[i]) : nullptr;
}
#else
// WinMM / CoreMIDI backends are separate (midi_input_win.cpp / midi_input_mac.cpp).
#endif

// ---- MIDI output ----
int encodeMessage(const MidiMessage& m, std::uint8_t out[3]) {
  const std::uint8_t ch = m.channel & 0x0F;
  switch (m.type) {
    case MidiMessage::NoteOff:         out[0] = 0x80 | ch; out[1] = m.a & 0x7F; out[2] = m.b & 0x7F; return 3;
    case MidiMessage::NoteOn:          out[0] = 0x90 | ch; out[1] = m.a & 0x7F; out[2] = m.b & 0x7F; return 3;
    case MidiMessage::PolyPressure:    out[0] = 0xA0 | ch; out[1] = m.a & 0x7F; out[2] = m.b & 0x7F; return 3;
    case MidiMessage::Control:         out[0] = 0xB0 | ch; out[1] = m.a & 0x7F; out[2] = m.b & 0x7F; return 3;
    case MidiMessage::Program:         out[0] = 0xC0 | ch; out[1] = m.a & 0x7F; return 2;
    case MidiMessage::ChannelPressure: out[0] = 0xD0 | ch; out[1] = m.a & 0x7F; return 2;
    case MidiMessage::PitchBend:       out[0] = 0xE0 | ch; out[1] = m.a & 0x7F; out[2] = m.b & 0x7F; return 3;
    case MidiMessage::Transport:       return 0;
  }
  return 0;
}

#if defined(__unix__) || defined(__APPLE__)
namespace {
class RawMidiOut final : public IMidiOutput {
 public:
  explicit RawMidiOut(std::string path) : path_(std::move(path)) {}
  ~RawMidiOut() override { stop(); }
  std::string name() const override { return path_; }
  bool start(std::string* error) override {
    if (fd_ >= 0) return true;
    fd_ = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { if (error) *error = "Cannot open " + path_ + ": " + std::strerror(errno); return false; }
    return true;
  }
  void stop() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
  bool send(const MidiMessage& m) override {
    std::uint8_t b[3];
    const int n = encodeMessage(m, b);
    if (n == 0 || fd_ < 0) return false;
    return ::write(fd_, b, static_cast<std::size_t>(n)) == n;
  }
 private:
  std::string path_;
  int fd_ = -1;
};
}  // namespace
std::unique_ptr<IMidiOutput> openRawMidiOutput(const std::string& path) { return std::make_unique<RawMidiOut>(path); }
#else
std::unique_ptr<IMidiOutput> openRawMidiOutput(const std::string&) { return nullptr; }
#endif

#if defined(__linux__)
std::vector<std::string> listMidiOutputs() { return listMidiInputs(); }   // the same raw ports carry both directions
std::unique_ptr<IMidiOutput> openMidiOutput(std::size_t i) {
  const auto l = listMidiOutputs();
  return i < l.size() ? openRawMidiOutput(l[i]) : nullptr;
}
#endif

}  // namespace wezt::midi
