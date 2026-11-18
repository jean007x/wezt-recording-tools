#pragma once
// WAV / BWF / RF64 reading and writing.
//
//  * Reader: PCM 8/16/24/32-bit, float 32/64, WAVE_FORMAT_EXTENSIBLE, BWF (`bext` time reference),
//    RF64/BW64 (files over 4 GB). Tolerates files whose sizes were never finalized (a crashed
//    recording): the data chunk then runs to the end of the file.
//  * Writer: streaming (write blocks as they arrive, finalize at the end). The header is patched
//    about once a second, so a crash loses at most that much audio, not the whole take. Files that
//    pass 4 GB become RF64 automatically. The BWF `bext` chunk follows EBU Tech 3285.
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "engine/audio_buffer.h"

namespace wezt::io {

struct WavInfo {
  unsigned channels = 0;
  unsigned sampleRate = 0;
  std::uint64_t frames = 0;
  unsigned bitsPerSample = 0;
  bool isFloat = false;
  bool rf64 = false;
  bool hasBext = false;
  std::uint64_t timeReference = 0;  // BWF: sample position of the first frame since midnight / session start
  std::string description;
};

class WavReader {
 public:
  ~WavReader() { close(); }
  bool open(const std::string& path, std::string* error);
  void close();
  bool isOpen() const { return file_.is_open(); }
  const WavInfo& info() const { return info_; }
  // Reads up to `count` frames starting at `start` into planar float buffers (dst[c] holds `count`
  // floats). Returns the number of frames actually read (less than `count` at the end of the file).
  std::size_t read(std::uint64_t start, std::size_t count, float* const* dst);

 private:
  std::ifstream file_;
  WavInfo info_;
  std::uint64_t dataOffset_ = 0;
  unsigned blockAlign_ = 0;
  std::vector<std::uint8_t> raw_;
};

enum class WavSampleType { Int16, Int24, Float32 };

class WavWriter {
 public:
  ~WavWriter();
  bool open(const std::string& path, unsigned channels, unsigned sampleRate, WavSampleType type, std::string* error,
            const std::string& description = {}, std::uint64_t timeReference = 0);
  bool isOpen() const { return file_.is_open(); }
  // Appends `frames` frames of planar float. Returns false on a write error (e.g. disk full).
  bool write(const float* const* planar, std::size_t frames);
  bool finalize(std::string* error = nullptr);  // patches the header and closes; safe to call twice
  // Chunks written after the audio when the file is finalized (e.g. "chna" and "axml" for ADM).
  // `tag` is 4 characters. Call before finalize().
  void addTrailingChunk(const char* tag, std::vector<std::uint8_t> bytes);
  std::uint64_t framesWritten() const { return frames_; }
  bool failed() const { return failed_; }
  // Test seam: switch to RF64 once the file would exceed this many bytes (default: the 4 GB RIFF limit).
  void setRf64ThresholdBytes(std::uint64_t n) { rf64Threshold_ = n; }

 private:
  void patchHeader(bool final);
  std::ofstream file_;
  WavSampleType type_ = WavSampleType::Float32;
  unsigned channels_ = 0, sampleRate_ = 0, bytesPerSample_ = 4;
  std::uint64_t frames_ = 0, framesAtLastPatch_ = 0;
  std::uint64_t riffSizePos_ = 0, junkPos_ = 0, dataSizePos_ = 0, dataStart_ = 0;
  bool failed_ = false, rf64_ = false;
  std::uint64_t rf64Threshold_ = 0xFFFFFFFFull;
  std::vector<std::uint8_t> bytes_;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> trailing_;
};

// Convenience: write a whole buffer as a 32-bit float BWF. Returns false on any error.
bool writeWavFloat(const std::string& path, const AudioBuffer& buf, unsigned sampleRate,
                   const std::string& description = {}, unsigned long long timeReference = 0);

}  // namespace wezt::io
