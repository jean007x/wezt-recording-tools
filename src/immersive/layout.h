#pragma once
// Speaker layouts for immersive monitoring. Geometry follows the ITU-R BS.2051 conventions
// (azimuth in degrees, positive to the LEFT; elevation positive up); the exact angles of each
// layout are our nominal choices. Channel order of the named layouts is the common film order.
#include <string>
#include <vector>

namespace wezt::immersive {

struct Speaker {
  std::string label;       // "L", "Ltf", "LFE" ...
  float azimuth = 0.f;     // degrees, +left
  float elevation = 0.f;   // degrees, +up
  bool lfe = false;
};

struct SpeakerLayout {
  std::string name;
  std::vector<Speaker> speakers;
  std::size_t size() const { return speakers.size(); }
  int lfeIndex() const { for (std::size_t i = 0; i < speakers.size(); ++i) if (speakers[i].lfe) return static_cast<int>(i); return -1; }
  bool hasHeight() const { for (const auto& s : speakers) if (s.elevation > 1.f) return true; return false; }
  std::size_t mainCount() const { std::size_t n = 0; for (const auto& s : speakers) if (!s.lfe) ++n; return n; }
};

SpeakerLayout makeLayout(const std::string& name);   // "2.0","5.1","7.1","5.1.2","7.1.2","7.1.4","9.1.6"; empty layout if unknown
std::vector<std::string> layoutNames();
// BS.2051-style label such as "M+030", "U-135", "LFE1" for the ADM export.
std::string admSpeakerLabel(const Speaker& s);

}  // namespace wezt::immersive
