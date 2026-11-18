#pragma once
// ADM BWF export (ITU-R BS.2076 Audio Definition Model carried in a BWF/RF64 file with "chna" and
// "axml" chunks, as described in ITU-R BS.2088). This is the open interchange format that
// object-based mastering tools import. Beds are written as DirectSpeakers packs, objects as
// Objects packs with cartesian position blocks (X right, Y front, Z up). IDs are in the custom
// range allowed by the standard, so no common-definition table is needed.
#include <string>
#include <vector>

#include "immersive/layout.h"
#include "io/wav.h"

namespace wezt::immersive {

struct AdmBlock {
  double start = 0.0, duration = 0.0;  // seconds; duration 0 on the last block = until the end
  float x = 0.f, y = 1.f, z = 0.f;
  float gain = 1.f, size = 0.f;
};

struct AdmObjectTrack {
  std::string name;
  std::vector<AdmBlock> blocks;        // at least one
};

struct AdmProgramme {
  std::string name = "Wezt Mix";
  double sampleRate = 48000.0;
  SpeakerLayout bed;                   // optional: its channels come first in the file
  std::string bedName = "Bed";
  std::vector<AdmObjectTrack> objects; // one mono channel each, after the bed channels
  std::size_t channelCount() const { return bed.size() + objects.size(); }
};

// Metadata text (axml) and channel map (chna) for a programme, exposed for tests and for tools.
std::string buildAdmXml(const AdmProgramme& p, double durationSeconds);
std::vector<std::uint8_t> buildChna(const AdmProgramme& p);
std::string xmlEscape(const std::string& s);
std::string admTime(double seconds);   // hh:mm:ss.fffff

class AdmWriter {
 public:
  bool open(const std::string& path, const AdmProgramme& p, io::WavSampleType type, std::string* error);
  bool write(const float* const* planar, std::size_t frames);   // one pointer per channel (bed channels first)
  bool finish(std::string* error);                               // writes the metadata and closes
 private:
  io::WavWriter w_;
  AdmProgramme p_;
  bool open_ = false;
};

}  // namespace wezt::immersive
