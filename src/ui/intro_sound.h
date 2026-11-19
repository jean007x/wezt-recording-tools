#pragma once
// The start-up intro's timeline and its sound.
//
//   0.0 - 3.8 s   the logo grows from a speck to full size while it spins (riser, drone, rising shimmer)
//   3.8 s         it lands: impact boom
//   3.8 - 4.5 s   it holds, a glint runs across it
//   4.5 - 6.0 s   it spins back down to a speck (descending whoosh)
//   6.0 s         the main window opens on a flash and the metal-on-metal clash
//
// The built-in sound is synthesized here from oscillators, filtered noise and a small reverb (no recordings, nothing
// licensed); a user can replace it with their own WAV or MP3.
#include <atomic>
#include <memory>
#include <string>

#include "audio/audio_device.h"
#include "io/audio_file.h"

namespace wezt::ui {

constexpr double kIntroGrowEnd = 3.8, kIntroHoldEnd = 4.5, kIntroOpen = 6.0, kIntroEnd = 6.8;

// The built-in sound at `rate` Hz, stereo, about 9 s long: deterministic (same samples every time).
io::StereoClip renderIntroSound(unsigned rate);

// Plays a clip on the system's default output without involving the DAW engine. Silent (start() returns false) on a
// machine with no audio backend. Control-thread API; the audio callback only reads the clip.
class IntroPlayer : public audio::AudioCallback {
 public:
  IntroPlayer() = default;
  ~IntroPlayer() override;
  IntroPlayer(const IntroPlayer&) = delete;
  IntroPlayer& operator=(const IntroPlayer&) = delete;
  // customPath empty = the built-in sound. A custom file that cannot be read falls back to the built-in sound and says so
  // in *message. Returns false when nothing could be played (no audio device).
  bool start(const std::string& customPath, std::string* message);
  void stop(double fadeSeconds = 0.25);   // fade out, then stop
  bool playing();                          // false once the clip ended or stop() finished; also releases the device
  void process(const float* const* in, float* const* out, unsigned frames) override;

 private:
  void release();
  std::unique_ptr<audio::AudioDevice> dev_;
  io::StereoClip clip_;
  unsigned outCh_ = 2;
  std::atomic<std::size_t> pos_{0};
  std::atomic<std::size_t> fadeLeft_{0};
  std::atomic<bool> fading_{false};
  std::atomic<bool> done_{true};
  std::size_t fadeTotal_ = 1;
};

}  // namespace wezt::ui
