#include "immersive/adm.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace wezt::immersive {
namespace {
std::string hex8(unsigned v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
std::string hex4(unsigned v) { char b[16]; std::snprintf(b, sizeof b, "%04X", v); return b; }
std::string num(double v) { char b[32]; std::snprintf(b, sizeof b, "%.6f", v); return b; }

// IDs (hex digits as the standard writes them)
std::string bedPackId() { return "AP_00011001"; }
std::string bedChannelId(std::size_t i) { return "AC_0001" + hex4(0x1001u + static_cast<unsigned>(i)); }
std::string objPackId(std::size_t i) { return "AP_0003" + hex4(0x1001u + static_cast<unsigned>(i)); }
std::string objChannelId(std::size_t i) { return "AC_0003" + hex4(0x1001u + static_cast<unsigned>(i)); }
std::string trackFormatId(const std::string& channelId) { return "AT_" + channelId.substr(3) + "_01"; }
std::string streamFormatId(const std::string& channelId) { return "AS_" + channelId.substr(3); }
std::string uid(std::size_t trackIndex) { return "ATU_" + hex8(static_cast<unsigned>(trackIndex + 1)); }
}  // namespace

std::string xmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&apos;"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') o += '?';  // not legal in XML 1.0
        else o += c;
    }
  }
  return o;
}

std::string admTime(double seconds) {
  if (seconds < 0.0) seconds = 0.0;
  const long long total = std::llround(seconds * 100000.0);
  const long long frac = total % 100000, secs = total / 100000;
  char b[48];
  std::snprintf(b, sizeof b, "%02lld:%02lld:%02lld.%05lld", secs / 3600, (secs / 60) % 60, secs % 60, frac);
  return b;
}

std::string buildAdmXml(const AdmProgramme& p, double durationSeconds) {
  std::string x;
  x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  x += "<ebuCoreMain xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns=\"urn:ebu:metadata-schema:ebuCore_2016\" xml:lang=\"en\" version=\"1\">\n";
  x += " <coreMetadata>\n  <format>\n   <audioFormatExtended version=\"ITU-R_BS.2076-2\">\n";
  const bool hasBed = p.bed.size() > 0;
  // programme, content, objects
  x += "    <audioProgramme audioProgrammeID=\"APR_1001\" audioProgrammeName=\"" + xmlEscape(p.name) + "\" start=\"00:00:00.00000\" end=\"" + admTime(durationSeconds) + "\">\n";
  x += "     <audioContentIDRef>ACO_1001</audioContentIDRef>\n    </audioProgramme>\n";
  x += "    <audioContent audioContentID=\"ACO_1001\" audioContentName=\"" + xmlEscape(p.name) + "\">\n";
  if (hasBed) x += "     <audioObjectIDRef>AO_1001</audioObjectIDRef>\n";
  for (std::size_t i = 0; i < p.objects.size(); ++i) x += "     <audioObjectIDRef>AO_" + hex4(0x1002u + static_cast<unsigned>(i)) + "</audioObjectIDRef>\n";
  x += "    </audioContent>\n";
  if (hasBed) {
    x += "    <audioObject audioObjectID=\"AO_1001\" audioObjectName=\"" + xmlEscape(p.bedName) + "\">\n";
    x += "     <audioPackFormatIDRef>" + bedPackId() + "</audioPackFormatIDRef>\n";
    for (std::size_t i = 0; i < p.bed.size(); ++i) x += "     <audioTrackUIDRef>" + uid(i) + "</audioTrackUIDRef>\n";
    x += "    </audioObject>\n";
  }
  for (std::size_t i = 0; i < p.objects.size(); ++i) {
    x += "    <audioObject audioObjectID=\"AO_" + hex4(0x1002u + static_cast<unsigned>(i)) + "\" audioObjectName=\"" + xmlEscape(p.objects[i].name) + "\">\n";
    x += "     <audioPackFormatIDRef>" + objPackId(i) + "</audioPackFormatIDRef>\n";
    x += "     <audioTrackUIDRef>" + uid(p.bed.size() + i) + "</audioTrackUIDRef>\n    </audioObject>\n";
  }
  // pack formats
  if (hasBed) {
    x += "    <audioPackFormat audioPackFormatID=\"" + bedPackId() + "\" audioPackFormatName=\"" + xmlEscape(p.bedName) + "\" typeLabel=\"0001\" typeDefinition=\"DirectSpeakers\">\n";
    for (std::size_t i = 0; i < p.bed.size(); ++i) x += "     <audioChannelFormatIDRef>" + bedChannelId(i) + "</audioChannelFormatIDRef>\n";
    x += "    </audioPackFormat>\n";
  }
  for (std::size_t i = 0; i < p.objects.size(); ++i) {
    x += "    <audioPackFormat audioPackFormatID=\"" + objPackId(i) + "\" audioPackFormatName=\"" + xmlEscape(p.objects[i].name) + "\" typeLabel=\"0003\" typeDefinition=\"Objects\">\n";
    x += "     <audioChannelFormatIDRef>" + objChannelId(i) + "</audioChannelFormatIDRef>\n    </audioPackFormat>\n";
  }
  // channel formats with their blocks
  for (std::size_t i = 0; i < p.bed.size(); ++i) {
    const Speaker& s = p.bed.speakers[i];
    x += "    <audioChannelFormat audioChannelFormatID=\"" + bedChannelId(i) + "\" audioChannelFormatName=\"" + xmlEscape(s.label) + "\" typeLabel=\"0001\" typeDefinition=\"DirectSpeakers\">\n";
    x += "     <audioBlockFormat audioBlockFormatID=\"AB_" + bedChannelId(i).substr(3) + "_00000001\">\n";
    x += "      <speakerLabel>" + admSpeakerLabel(s) + "</speakerLabel>\n";
    x += "      <position coordinate=\"azimuth\">" + num(s.azimuth) + "</position>\n";
    x += "      <position coordinate=\"elevation\">" + num(s.elevation) + "</position>\n";
    x += "      <position coordinate=\"distance\">1.000000</position>\n";
    x += "     </audioBlockFormat>\n    </audioChannelFormat>\n";
  }
  for (std::size_t i = 0; i < p.objects.size(); ++i) {
    x += "    <audioChannelFormat audioChannelFormatID=\"" + objChannelId(i) + "\" audioChannelFormatName=\"" + xmlEscape(p.objects[i].name) + "\" typeLabel=\"0003\" typeDefinition=\"Objects\">\n";
    std::vector<AdmBlock> blocks = p.objects[i].blocks;
    if (blocks.empty()) blocks.push_back(AdmBlock{});
    for (std::size_t b = 0; b < blocks.size(); ++b) {
      const AdmBlock& k = blocks[b];
      double dur = k.duration;
      if (dur <= 0.0) dur = b + 1 < blocks.size() ? blocks[b + 1].start - k.start : durationSeconds - k.start;
      if (dur < 0.0) dur = 0.0;
      char id[40];
      std::snprintf(id, sizeof id, "_%08zX", b + 1);
      x += "     <audioBlockFormat audioBlockFormatID=\"AB_" + objChannelId(i).substr(3) + id + "\" rtime=\"" + admTime(k.start) + "\" duration=\"" + admTime(dur) + "\">\n";
      x += "      <cartesian>1</cartesian>\n";
      x += "      <position coordinate=\"X\">" + num(k.x) + "</position>\n";
      x += "      <position coordinate=\"Y\">" + num(k.y) + "</position>\n";
      x += "      <position coordinate=\"Z\">" + num(k.z) + "</position>\n";
      x += "      <gain>" + num(k.gain) + "</gain>\n";
      if (k.size > 0.f) { x += "      <width>" + num(k.size * 360.0) + "</width>\n"; }
      x += "     </audioBlockFormat>\n";
    }
    x += "    </audioChannelFormat>\n";
  }
  // stream and track formats, track UIDs
  const std::size_t total = p.bed.size() + p.objects.size();
  for (std::size_t t = 0; t < total; ++t) {
    const std::string ch = t < p.bed.size() ? bedChannelId(t) : objChannelId(t - p.bed.size());
    const std::string name = t < p.bed.size() ? p.bed.speakers[t].label : p.objects[t - p.bed.size()].name;
    x += "    <audioStreamFormat audioStreamFormatID=\"" + streamFormatId(ch) + "\" audioStreamFormatName=\"PCM_" + xmlEscape(name) + "\" formatLabel=\"0001\" formatDefinition=\"PCM\">\n";
    x += "     <audioChannelFormatIDRef>" + ch + "</audioChannelFormatIDRef>\n";
    x += "     <audioTrackFormatIDRef>" + trackFormatId(ch) + "</audioTrackFormatIDRef>\n    </audioStreamFormat>\n";
  }
  for (std::size_t t = 0; t < total; ++t) {
    const std::string ch = t < p.bed.size() ? bedChannelId(t) : objChannelId(t - p.bed.size());
    const std::string name = t < p.bed.size() ? p.bed.speakers[t].label : p.objects[t - p.bed.size()].name;
    x += "    <audioTrackFormat audioTrackFormatID=\"" + trackFormatId(ch) + "\" audioTrackFormatName=\"PCM_" + xmlEscape(name) + "\" formatLabel=\"0001\" formatDefinition=\"PCM\">\n";
    x += "     <audioStreamFormatIDRef>" + streamFormatId(ch) + "</audioStreamFormatIDRef>\n    </audioTrackFormat>\n";
  }
  for (std::size_t t = 0; t < total; ++t) {
    const std::string ch = t < p.bed.size() ? bedChannelId(t) : objChannelId(t - p.bed.size());
    const std::string pack = t < p.bed.size() ? bedPackId() : objPackId(t - p.bed.size());
    x += "    <audioTrackUID UID=\"" + uid(t) + "\">\n";
    x += "     <audioTrackFormatIDRef>" + trackFormatId(ch) + "</audioTrackFormatIDRef>\n";
    x += "     <audioPackFormatIDRef>" + pack + "</audioPackFormatIDRef>\n    </audioTrackUID>\n";
  }
  x += "   </audioFormatExtended>\n  </format>\n </coreMetadata>\n</ebuCoreMain>\n";
  return x;
}

std::vector<std::uint8_t> buildChna(const AdmProgramme& p) {
  const std::size_t total = p.bed.size() + p.objects.size();
  std::vector<std::uint8_t> b(4 + 40 * total, 0);
  auto put16 = [&](std::size_t at, unsigned v) { b[at] = static_cast<std::uint8_t>(v); b[at + 1] = static_cast<std::uint8_t>(v >> 8); };
  put16(0, static_cast<unsigned>(total));   // numTracks
  put16(2, static_cast<unsigned>(total));   // numUIDs
  for (std::size_t t = 0; t < total; ++t) {
    const std::size_t at = 4 + 40 * t;
    put16(at, static_cast<unsigned>(t + 1));
    const std::string ch = t < p.bed.size() ? bedChannelId(t) : objChannelId(t - p.bed.size());
    const std::string pack = t < p.bed.size() ? bedPackId() : objPackId(t - p.bed.size());
    const std::string u = uid(t), tr = trackFormatId(ch);
    std::memcpy(&b[at + 2], u.data(), std::min<std::size_t>(12, u.size()));
    std::memcpy(&b[at + 14], tr.data(), std::min<std::size_t>(14, tr.size()));
    std::memcpy(&b[at + 28], pack.data(), std::min<std::size_t>(11, pack.size()));
  }
  return b;
}

bool AdmWriter::open(const std::string& path, const AdmProgramme& p, io::WavSampleType type, std::string* error) {
  if (p.channelCount() == 0 || p.channelCount() > 128) { if (error) *error = "ADM export needs 1 to 128 channels"; return false; }
  if (p.bed.size() > 0 && p.bed.size() > p.channelCount()) { if (error) *error = "bad bed"; return false; }
  p_ = p;
  open_ = w_.open(path, static_cast<unsigned>(p.channelCount()), static_cast<unsigned>(std::lround(p.sampleRate)), type, error, "ADM BWF by Wezt Recording Tools");
  return open_;
}

bool AdmWriter::write(const float* const* planar, std::size_t frames) { return open_ && w_.write(planar, frames); }

bool AdmWriter::finish(std::string* error) {
  if (!open_) return false;
  const double dur = static_cast<double>(w_.framesWritten()) / p_.sampleRate;
  const std::string xml = buildAdmXml(p_, dur);
  w_.addTrailingChunk("chna", buildChna(p_));
  w_.addTrailingChunk("axml", std::vector<std::uint8_t>(xml.begin(), xml.end()));
  open_ = false;
  return w_.finalize(error);
}

}  // namespace wezt::immersive
