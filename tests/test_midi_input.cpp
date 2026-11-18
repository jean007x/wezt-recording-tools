#include "midi/midi_input.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace wezt::midi;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::vector<MidiMessage> parseAll(const std::vector<int>& bytes) {
  MidiParser p; std::vector<MidiMessage> v; MidiMessage m;
  for (int b : bytes) if (p.feed(static_cast<std::uint8_t>(b), m)) v.push_back(m);
  return v;
}

int main() {
  {  // plain note on / off
    auto v = parseAll({0x90, 60, 100, 0x80, 60, 0});
    CHECK(v.size() == 2);
    CHECK(v[0].type == MidiMessage::NoteOn && v[0].a == 60 && v[0].b == 100 && v[0].channel == 0);
    CHECK(v[1].type == MidiMessage::NoteOff && v[1].a == 60);
  }
  {  // running status, velocity-0 note on is a note off, channel kept
    auto v = parseAll({0x93, 64, 90, 64, 0, 67, 80});
    CHECK(v.size() == 3);
    CHECK(v[0].type == MidiMessage::NoteOn && v[0].channel == 3);
    CHECK(v[1].type == MidiMessage::NoteOff && v[1].a == 64);
    CHECK(v[2].type == MidiMessage::NoteOn && v[2].a == 67 && v[2].b == 80);
  }
  {  // realtime clock bytes inside a message do not break it
    auto v = parseAll({0x90, 0xF8, 60, 0xFE, 100});
    CHECK(v.size() == 1 && v[0].a == 60 && v[0].b == 100);
  }
  {  // sysex is skipped, a stray data byte is ignored, and a new status cancels a half message
    auto v = parseAll({0x55, 0xF0, 1, 2, 3, 0xF7, 0x90, 60, 0x80, 61, 0, 0xB0, 7, 99});
    CHECK(v.size() == 2);
    CHECK(v[0].type == MidiMessage::NoteOff && v[0].a == 61);
    CHECK(v[1].type == MidiMessage::Control && v[1].a == 7 && v[1].b == 99);
  }
  {  // one-data-byte messages and pitch bend
    auto v = parseAll({0xC1, 5, 0xD0, 33, 0xE0, 0x00, 0x40, 0xE0, 0x7F, 0x7F, 0xE0, 0, 0});
    CHECK(v.size() == 5);
    CHECK(v[0].type == MidiMessage::Program && v[0].a == 5 && v[0].channel == 1);
    CHECK(v[1].type == MidiMessage::ChannelPressure && v[1].a == 33);
    CHECK(v[2].type == MidiMessage::PitchBend && v[2].pitchBendValue() == 0);
    CHECK(v[3].pitchBendValue() == 8191);
    CHECK(v[4].pitchBendValue() == -8192);
  }
  {  // queue: order, wrap-around and overflow counting
    MidiQueue q; MidiMessage m;
    CHECK(!q.pop(m));
    for (int i = 0; i < 3000; ++i) { MidiMessage x; x.a = static_cast<std::uint8_t>(i & 127); x.b = 1; CHECK(q.push(x)); CHECK(q.pop(m) && m.a == (i & 127)); }
    int pushed = 0; MidiMessage x; while (q.push(x)) ++pushed;
    CHECK(pushed == 1024 && q.dropped() == 1);
  }
#if defined(__unix__) || defined(__APPLE__)
  {  // a real byte stream through the device thread: a FIFO stands in for the keyboard
    const std::string path = "/tmp/wezt_midi_test_fifo_" + std::to_string(::getpid());
    ::unlink(path.c_str());
    CHECK(::mkfifo(path.c_str(), 0600) == 0);
    auto dev = openRawMidi(path);
    CHECK(dev != nullptr);
    std::string err;
    CHECK(dev->start(&err));
    int w = ::open(path.c_str(), O_WRONLY);
    CHECK(w >= 0);
    const unsigned char bytes[] = {0x90, 60, 100, 0x80, 60, 0, 0x90, 72, 1};
    CHECK(::write(w, bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes));
    std::vector<MidiMessage> got;
    for (int i = 0; i < 100 && got.size() < 3; ++i) { MidiMessage m; while (dev->queue().pop(m)) got.push_back(m); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(got.size() == 3);
    if (got.size() == 3) { CHECK(got[0].type == MidiMessage::NoteOn && got[0].a == 60); CHECK(got[1].type == MidiMessage::NoteOff); CHECK(got[2].a == 72 && got[2].b == 1); }
    ::close(w);
    dev->stop(); dev->stop();   // stopping twice is fine
    ::unlink(path.c_str());
    auto bad = openRawMidi("/nonexistent/midi");
    CHECK(bad && !bad->start(&err) && !err.empty());
  }
#endif
  std::printf(failures ? "midi_input: %d FAILED\n" : "midi_input: all passed\n", failures);
  return failures ? 1 : 0;
}
