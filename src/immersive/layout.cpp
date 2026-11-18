#include "immersive/layout.h"

#include <cmath>
#include <cstdio>

namespace wezt::immersive {
namespace {
Speaker sp(const char* l, float az, float el = 0.f) { return {l, az, el, false}; }
Speaker lfe() { return {"LFE", 0.f, -15.f, true}; }
}  // namespace

std::vector<std::string> layoutNames() { return {"2.0", "5.1", "7.1", "5.1.2", "7.1.2", "7.1.4", "9.1.6"}; }

SpeakerLayout makeLayout(const std::string& name) {
  SpeakerLayout l;
  l.name = name;
  auto base51 = [&] { l.speakers = {sp("L", 30), sp("R", -30), sp("C", 0), lfe(), sp("Ls", 110), sp("Rs", -110)}; };
  auto base71 = [&] { l.speakers = {sp("L", 30), sp("R", -30), sp("C", 0), lfe(), sp("Lss", 90), sp("Rss", -90), sp("Lrs", 135), sp("Rrs", -135)}; };
  if (name == "2.0") l.speakers = {sp("L", 30), sp("R", -30)};
  else if (name == "5.1") base51();
  else if (name == "7.1") base71();
  else if (name == "5.1.2") { base51(); l.speakers.push_back(sp("Ltm", 90, 45)); l.speakers.push_back(sp("Rtm", -90, 45)); }
  else if (name == "7.1.2") { base71(); l.speakers.push_back(sp("Ltm", 90, 45)); l.speakers.push_back(sp("Rtm", -90, 45)); }
  else if (name == "7.1.4") {
    base71();
    l.speakers.push_back(sp("Ltf", 45, 45)); l.speakers.push_back(sp("Rtf", -45, 45));
    l.speakers.push_back(sp("Ltr", 135, 45)); l.speakers.push_back(sp("Rtr", -135, 45));
  } else if (name == "9.1.6") {
    base71();
    l.speakers.push_back(sp("Lw", 60)); l.speakers.push_back(sp("Rw", -60));
    l.speakers.push_back(sp("Ltf", 45, 45)); l.speakers.push_back(sp("Rtf", -45, 45));
    l.speakers.push_back(sp("Ltm", 90, 45)); l.speakers.push_back(sp("Rtm", -90, 45));
    l.speakers.push_back(sp("Ltr", 135, 45)); l.speakers.push_back(sp("Rtr", -135, 45));
  } else l.name.clear();
  return l;
}

std::string admSpeakerLabel(const Speaker& s) {
  if (s.lfe) return "LFE1";
  const int az = static_cast<int>(std::lround(s.azimuth));
  char b[16];
  std::snprintf(b, sizeof b, "%c%c%03d", s.elevation > 1.f ? 'U' : 'M', az < 0 ? '-' : '+', std::abs(az));
  return b;
}

}  // namespace wezt::immersive
