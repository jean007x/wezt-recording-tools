#include "io/wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace wezt::io {
namespace {

// ---- little-endian helpers (independent of the host's byte order) ----
void put16(std::vector<std::uint8_t>& b, std::uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
void put64(std::vector<std::uint8_t>& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
void putTag(std::vector<std::uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }
void putFixed(std::vector<std::uint8_t>& b, const std::string& s, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) b.push_back(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t get32(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint64_t get64(const std::uint8_t* p) { return static_cast<std::uint64_t>(get32(p)) | (static_cast<std::uint64_t>(get32(p + 4)) << 32); }

void patch32(std::ofstream& f, std::uint64_t pos, std::uint32_t v) {
  std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
  f.seekp(static_cast<std::streamoff>(pos));
  f.write(reinterpret_cast<const char*>(b), 4);
}
void patch64(std::ofstream& f, std::uint64_t pos, std::uint64_t v) {
  std::uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * i));
  f.seekp(static_cast<std::streamoff>(pos));
  f.write(reinterpret_cast<const char*>(b), 8);
}

// Conventional integer scaling: float = int / 2^(bits-1), clipped on the way in.
inline float decodeInt(std::int32_t v, unsigned bits) { return static_cast<float>(static_cast<double>(v) / static_cast<double>(1LL << (bits - 1))); }
inline std::int32_t encodeInt(float x, unsigned bits) {
  const double full = static_cast<double>(1LL << (bits - 1));
  if (std::isnan(x)) return 0;  // NaN would be undefined behaviour in the cast below
  double v = std::nearbyint(static_cast<double>(x) * full);
  v = std::min(v, full - 1.0);
  v = std::max(v, -full);
  return static_cast<std::int32_t>(v);
}

constexpr std::size_t kBextSize = 602;

}  // namespace

// ============================== reader ==============================

bool WavReader::open(const std::string& path, std::string* error) {
  close();
  auto fail = [&](const std::string& m) { if (error) *error = m; close(); return false; };
  file_.open(path, std::ios::binary);
  if (!file_) return fail("cannot open " + path);

  file_.seekg(0, std::ios::end);
  const std::uint64_t fileSize = static_cast<std::uint64_t>(file_.tellg());
  file_.seekg(0);
  std::uint8_t hdr[12];
  if (!file_.read(reinterpret_cast<char*>(hdr), 12)) return fail("not a WAV file (too short)");
  const bool riff = !std::memcmp(hdr, "RIFF", 4), rf = !std::memcmp(hdr, "RF64", 4) || !std::memcmp(hdr, "BW64", 4);
  if ((!riff && !rf) || std::memcmp(hdr + 8, "WAVE", 4)) return fail("not a WAV file");
  info_ = WavInfo{};
  info_.rf64 = rf;

  bool haveFmt = false, haveData = false;
  std::uint64_t ds64Data = 0;
  std::uint64_t dataSize = 0;
  unsigned tag = 0;
  std::uint64_t pos = 12;
  while (pos + 8 <= fileSize) {
    file_.seekg(static_cast<std::streamoff>(pos));
    std::uint8_t ch[8];
    if (!file_.read(reinterpret_cast<char*>(ch), 8)) break;
    std::uint64_t size = get32(ch + 4);
    const std::uint64_t body = pos + 8;
    if (!std::memcmp(ch, "ds64", 4)) {
      std::uint8_t d[28] = {};
      file_.read(reinterpret_cast<char*>(d), std::min<std::uint64_t>(size, sizeof d));
      ds64Data = get64(d + 8);
    } else if (!std::memcmp(ch, "fmt ", 4)) {
      std::vector<std::uint8_t> f(static_cast<std::size_t>(std::min<std::uint64_t>(size, 64)));
      file_.read(reinterpret_cast<char*>(f.data()), static_cast<std::streamsize>(f.size()));
      if (f.size() < 16) return fail("fmt chunk too short");
      tag = get16(f.data());
      info_.channels = get16(f.data() + 2);
      info_.sampleRate = get32(f.data() + 4);
      blockAlign_ = get16(f.data() + 12);
      info_.bitsPerSample = get16(f.data() + 14);
      if (tag == 0xFFFE && f.size() >= 26) tag = get16(f.data() + 24);  // extensible: sub-format
      info_.isFloat = tag == 3;
      if (tag != 1 && tag != 3) return fail("unsupported WAV encoding (tag " + std::to_string(tag) + ")");
      haveFmt = true;
    } else if (!std::memcmp(ch, "bext", 4) && size >= 346) {
      std::vector<std::uint8_t> b(static_cast<std::size_t>(std::min<std::uint64_t>(size, kBextSize)));
      file_.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
      std::size_t n = 0;
      while (n < 256 && b[n]) ++n;
      info_.description.assign(reinterpret_cast<char*>(b.data()), n);
      info_.timeReference = get64(b.data() + 256 + 32 + 32 + 10 + 8);  // low word then high word, little-endian
      info_.hasBext = true;
    } else if (!std::memcmp(ch, "data", 4)) {
      dataOffset_ = body;
      if (size == 0xFFFFFFFFull && ds64Data) size = ds64Data;
      // An unfinalized or truncated file: trust the file length, not the header.
      // (A genuinely empty data chunk is followed by another well-formed chunk, or nothing.)
      auto looksLikeChunk = [&] {
        std::uint8_t h[8];
        if (body + 8 > fileSize) return false;
        file_.clear(); file_.seekg(static_cast<std::streamoff>(body)); file_.read(reinterpret_cast<char*>(h), 8);
        if (file_.gcount() != 8) return false;
        for (int i = 0; i < 4; ++i) if (h[i] < 0x20 || h[i] > 0x7e) return false;
        return body + 8 + get32(h + 4) <= fileSize;
      };
      if (size == 0 && body < fileSize && !looksLikeChunk()) size = fileSize - body;
      if (size == 0xFFFFFFFFull || body + size > fileSize) size = fileSize - body;
      dataSize = size;
      haveData = true;
      break;  // everything we need precedes the audio in files we write; other writers do the same
    }
    pos = body + size + (size & 1);
  }
  if (!haveFmt) return fail("no fmt chunk");
  if (!haveData) return fail("no data chunk");
  if (info_.channels == 0 || blockAlign_ == 0) return fail("invalid fmt chunk");
  const unsigned b = info_.bitsPerSample;
  if (b == 0 || blockAlign_ < info_.channels * ((b + 7) / 8)) return fail("inconsistent fmt chunk (block align too small)");
  const bool okBits = info_.isFloat ? (b == 32 || b == 64) : (b == 8 || b == 16 || b == 24 || b == 32);
  if (!okBits) return fail("unsupported bit depth " + std::to_string(b));
  info_.frames = dataSize / blockAlign_;
  return true;
}

void WavReader::close() {
  if (file_.is_open()) file_.close();
  file_.clear();
  info_ = WavInfo{};
}

std::size_t WavReader::read(std::uint64_t start, std::size_t count, float* const* dst) {
  if (!file_.is_open() || start >= info_.frames || count == 0) return 0;
  count = static_cast<std::size_t>(std::min<std::uint64_t>(count, info_.frames - start));
  const unsigned ch = info_.channels, bytes = info_.bitsPerSample / 8;
  const std::size_t chunkFrames = 8192;
  std::size_t done = 0;
  while (done < count) {
    const std::size_t n = std::min(chunkFrames, count - done);
    raw_.resize(n * blockAlign_);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_ + (start + done) * blockAlign_));
    file_.read(reinterpret_cast<char*>(raw_.data()), static_cast<std::streamsize>(raw_.size()));
    const std::size_t got = static_cast<std::size_t>(file_.gcount()) / blockAlign_;
    for (std::size_t i = 0; i < got; ++i) {
      for (unsigned c = 0; c < ch; ++c) {
        const std::uint8_t* p = raw_.data() + i * blockAlign_ + c * bytes;
        float v;
        if (info_.isFloat) {
          if (bytes == 4) { std::uint32_t u = get32(p); std::memcpy(&v, &u, 4); }
          else { std::uint64_t u = get64(p); double d; std::memcpy(&d, &u, 8); v = static_cast<float>(d); }
        } else if (bytes == 1) {
          v = static_cast<float>(static_cast<int>(p[0]) - 128) / 128.f;  // 8-bit WAV is unsigned
        } else if (bytes == 2) {
          v = decodeInt(static_cast<std::int16_t>(get16(p)), 16);
        } else if (bytes == 3) {
          std::int32_t s = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
          if (s & 0x800000) s |= ~0xFFFFFF;
          v = decodeInt(s, 24);
        } else {
          v = decodeInt(static_cast<std::int32_t>(get32(p)), 32);
        }
        dst[c][done + i] = v;
      }
    }
    done += got;
    if (got < n) break;
  }
  return done;
}

// ============================== writer ==============================

WavWriter::~WavWriter() { finalize(nullptr); }

bool WavWriter::open(const std::string& path, unsigned channels, unsigned sampleRate, WavSampleType type, std::string* error,
                     const std::string& description, std::uint64_t timeReference) {
  finalize(nullptr);
  if (channels == 0 || sampleRate == 0) { if (error) *error = "invalid channel count or sample rate"; return false; }
  file_.open(path, std::ios::binary | std::ios::trunc);
  if (!file_) { if (error) *error = "cannot create " + path; return false; }
  type_ = type;
  channels_ = channels;
  sampleRate_ = sampleRate;
  bytesPerSample_ = type == WavSampleType::Int16 ? 2 : type == WavSampleType::Int24 ? 3 : 4;
  frames_ = framesAtLastPatch_ = 0;
  failed_ = rf64_ = false;

  const bool ext = channels > 2;
  const unsigned bits = bytesPerSample_ * 8;
  const bool isFloat = type == WavSampleType::Float32;
  std::vector<std::uint8_t> h;
  putTag(h, "RIFF"); riffSizePos_ = h.size(); put32(h, 0); putTag(h, "WAVE");
  // JUNK reserves room for the 28-byte ds64 chunk, so a file that outgrows 4 GB can become RF64 in place.
  putTag(h, "JUNK"); put32(h, 28); junkPos_ = h.size(); for (int i = 0; i < 28; ++i) h.push_back(0);
  putTag(h, "fmt "); put32(h, ext ? 40 : 16);
  put16(h, ext ? 0xFFFE : (isFloat ? 3 : 1));
  put16(h, static_cast<std::uint16_t>(channels));
  put32(h, sampleRate);
  put32(h, sampleRate * channels * bytesPerSample_);
  put16(h, static_cast<std::uint16_t>(channels * bytesPerSample_));
  put16(h, static_cast<std::uint16_t>(bits));
  if (ext) {
    put16(h, 22); put16(h, static_cast<std::uint16_t>(bits)); put32(h, 0);  // cbSize, valid bits, channel mask (unspecified)
    put16(h, isFloat ? 3 : 1);                                           // sub-format GUID: KSDATAFORMAT_SUBTYPE_{PCM,IEEE_FLOAT}
    const std::uint8_t guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    h.insert(h.end(), guidTail, guidTail + 14);
  }
  // bext (EBU Tech 3285 v1): fixed 602-byte layout.
  putTag(h, "bext"); put32(h, kBextSize);
  const std::size_t b0 = h.size();
  putFixed(h, description, 256);
  putFixed(h, "Wezt Recording Tools", 32);
  putFixed(h, "", 32);  // originator reference
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  char date[16], tim[16];
  std::strftime(date, sizeof date, "%Y-%m-%d", &tmv);
  std::strftime(tim, sizeof tim, "%H:%M:%S", &tmv);
  putFixed(h, date, 10);
  putFixed(h, tim, 8);
  put64(h, timeReference);  // low word, then high word
  put16(h, 1);              // BWF version
  while (h.size() - b0 < kBextSize) h.push_back(0);  // UMID, loudness fields, reserved
  putTag(h, "data"); dataSizePos_ = h.size(); put32(h, 0);
  dataStart_ = h.size();
  file_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
  if (!file_) { if (error) *error = "write failed"; file_.close(); return false; }
  patchHeader(false);
  return true;
}

void WavWriter::addTrailingChunk(const char* tag, std::vector<std::uint8_t> bytes) {
  trailing_.emplace_back(std::string(tag, 4), std::move(bytes));
}

void WavWriter::patchHeader(bool final) {
  if (!file_.is_open()) return;
  const std::uint64_t dataBytes = frames_ * channels_ * bytesPerSample_;
  const std::uint64_t pad = dataBytes & 1;
  std::uint64_t extra = 0;
  if (final) for (const auto& t : trailing_) extra += 8 + t.second.size() + (t.second.size() & 1);
  const std::uint64_t total = dataStart_ + dataBytes + pad + extra;  // whole file
  const std::streampos endPos = file_.tellp();
  if (total - 8 > rf64Threshold_) {
    // Past 4 GB: RF64. The JUNK chunk becomes ds64 and the 32-bit size fields say "see ds64".
    rf64_ = true;
    file_.seekp(static_cast<std::streamoff>(junkPos_ - 8));
    file_.write("ds64", 4);
    patch64(file_, junkPos_, total - 8);
    patch64(file_, junkPos_ + 8, dataBytes);
    patch64(file_, junkPos_ + 16, frames_);
    patch32(file_, junkPos_ + 24, 0);
    file_.seekp(0);
    file_.write("RF64", 4);
    patch32(file_, riffSizePos_, 0xFFFFFFFFu);
    patch32(file_, dataSizePos_, 0xFFFFFFFFu);
  } else {
    patch32(file_, riffSizePos_, static_cast<std::uint32_t>(total - 8));
    patch32(file_, dataSizePos_, static_cast<std::uint32_t>(dataBytes));
  }
  file_.seekp(endPos);
  if (final) {
    if (pad) file_.put('\0');
    for (const auto& t : trailing_) {
      std::vector<std::uint8_t> h;
      putTag(h, t.first.c_str()); put32(h, static_cast<std::uint32_t>(t.second.size()));
      file_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
      file_.write(reinterpret_cast<const char*>(t.second.data()), static_cast<std::streamsize>(t.second.size()));
      if (t.second.size() & 1) file_.put('\0');
    }
    file_.flush();
  }
  framesAtLastPatch_ = frames_;
}

bool WavWriter::write(const float* const* planar, std::size_t frames) {
  if (!file_.is_open() || failed_) return false;
  const std::size_t chunk = 4096;
  for (std::size_t off = 0; off < frames; off += chunk) {
    const std::size_t n = std::min(chunk, frames - off);
    bytes_.resize(n * channels_ * bytesPerSample_);
    std::uint8_t* d = bytes_.data();
    for (std::size_t i = 0; i < n; ++i) {
      for (unsigned c = 0; c < channels_; ++c) {
        const float x = planar[c][off + i];
        if (type_ == WavSampleType::Float32) {
          std::uint32_t u;
          std::memcpy(&u, &x, 4);
          d[0] = u & 0xFF; d[1] = (u >> 8) & 0xFF; d[2] = (u >> 16) & 0xFF; d[3] = u >> 24;
          d += 4;
        } else if (type_ == WavSampleType::Int16) {
          const std::int32_t v = encodeInt(x, 16);
          d[0] = v & 0xFF; d[1] = (v >> 8) & 0xFF;
          d += 2;
        } else {
          const std::int32_t v = encodeInt(x, 24);
          d[0] = v & 0xFF; d[1] = (v >> 8) & 0xFF; d[2] = (v >> 16) & 0xFF;
          d += 3;
        }
      }
    }
    file_.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    if (!file_) { failed_ = true; return false; }
    frames_ += n;
  }
  if (frames_ - framesAtLastPatch_ >= sampleRate_) patchHeader(false);  // about once a second
  return true;
}

bool WavWriter::finalize(std::string* error) {
  if (!file_.is_open()) return true;
  patchHeader(true);
  const bool ok = static_cast<bool>(file_) && !failed_;
  file_.close();
  if (!ok && error) *error = "write error (disk full?)";
  return ok;
}

bool writeWavFloat(const std::string& path, const AudioBuffer& buf, unsigned sampleRate, const std::string& description,
                   unsigned long long timeReference) {
  WavWriter w;
  if (buf.channels == 0 || !w.open(path, static_cast<unsigned>(buf.channels), sampleRate, WavSampleType::Float32, nullptr, description, timeReference))
    return false;
  std::vector<const float*> ptrs(buf.channels);
  for (std::size_t c = 0; c < buf.channels; ++c) ptrs[c] = buf.channel(c);
  return w.write(ptrs.data(), buf.frames) && w.finalize(nullptr);
}

}  // namespace wezt::io
