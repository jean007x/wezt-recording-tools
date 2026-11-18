#pragma once
#include <memory>
#include <string>
#include <vector>

namespace wezt::audio {

struct AudioConfig {
  unsigned sampleRate = 48000;
  unsigned bufferFrames = 256;  // requested; the backend reports what it actually got
  unsigned inChannels = 2;      // 0 = output only
  unsigned outChannels = 2;

  // Device selection. Empty = the system default. Ids come from listDevices().
  std::string outputDeviceId;
  std::string inputDeviceId;

  // Exclusive mode: WASAPI exclusive on Windows, CoreAudio "hog mode" on macOS. The device is
  // then owned by this application (lowest latency, bit-exact, other apps are muted).
  bool exclusive = false;
  // If exclusive mode cannot be obtained (device busy, driver refuses, disabled in Windows
  // sound settings), fall back to shared mode instead of failing. A notice is recorded.
  bool fallbackToShared = true;

  // Filled in by the backend in actualConfig() only (ignored on input): the delay between the
  // engine writing a sample and it leaving the interface, and between the interface and the
  // engine receiving it, in frames. Use these to show a round-trip latency readout.
  unsigned outputLatencyFrames = 0;
  unsigned inputLatencyFrames = 0;
};

struct AudioDeviceInfo {
  std::string id;    // stable identifier to pass back in AudioConfig
  std::string name;  // human readable
  unsigned inputChannels = 0;
  unsigned outputChannels = 0;
  bool isDefaultInput = false;
  bool isDefaultOutput = false;
  // Windows only: the two per-device switches in Sound > Properties > Advanced. They are user
  // settings; Wezt reads them but never changes them. "known" is false if Windows did not say.
  bool exclusiveSettingsKnown = false;
  bool allowsExclusive = true;       // "Allow applications to take exclusive control of this device"
  bool exclusivePriority = false;    // "Give exclusive mode applications priority"
};

// Implemented by the engine. Called on the backend's real-time thread: no allocation,
// no locks, no I/O. Buffers are planar (one pointer per channel). `frames` may be any
// value up to the backend's maximum callback size, and may vary between calls.
class AudioCallback {
 public:
  virtual ~AudioCallback() = default;
  virtual void deviceAboutToStart(const AudioConfig& actual) { (void)actual; }
  virtual void process(const float* const* in, float* const* out, unsigned frames) = 0;
};

class AudioDevice {
 public:
  virtual ~AudioDevice() = default;
  virtual std::string backendName() const = 0;
  // Validates the request. Does not touch hardware until start().
  virtual bool open(const AudioConfig& requested, AudioCallback* cb, std::string* error) = 0;
  virtual bool start(std::string* error) = 0;
  virtual void stop() = 0;
  // What the hardware actually gave us (rate, buffer size, channels, exclusive).
  virtual AudioConfig actualConfig() const = 0;
  // Non-fatal messages from start(), e.g. "exclusive mode refused (device busy); using shared mode".
  virtual std::string notice() const { return {}; }
  // True once the running stream died on its own: the device was unplugged, disabled, or taken over by the system. The
  // callback is no longer being called. The owner should stop() and try open()+start() again (cleared by start()).
  virtual bool lost() const { return false; }
};

// WASAPI on Windows, CoreAudio on macOS, nullptr elsewhere.
std::unique_ptr<AudioDevice> createPlatformDevice();
// Active input and output devices on this machine (empty on platforms without a backend).
std::vector<AudioDeviceInfo> listDevices();

// Synchronous device for tests and headless rendering: nothing runs until pump() is called.
class NullDevice : public AudioDevice {
 public:
  std::string backendName() const override { return "null"; }
  bool open(const AudioConfig& requested, AudioCallback* cb, std::string* error) override;
  bool start(std::string* error) override;
  void stop() override { started_ = false; }
  AudioConfig actualConfig() const override { return cfg_; }
  // Runs `blocks` callbacks of `callbackFrames` frames each. Every input channel carries
  // `inputValue`.
  void pump(unsigned blocks, unsigned callbackFrames, float inputValue = 0.f);

 private:
  AudioConfig cfg_;
  AudioCallback* cb_ = nullptr;
  bool started_ = false;
};

}  // namespace wezt::audio
