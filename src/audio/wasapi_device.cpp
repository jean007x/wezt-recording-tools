// WASAPI backend (Windows). Event-driven render and capture in shared or exclusive mode.
//
// Exclusive mode talks to the driver directly: lowest latency and bit-exact, but the device
// must support the format natively (usually 24-bit), buffers must be aligned to the hardware,
// and other applications lose the device. Whether exclusive mode is allowed at all is the user's
// choice in Sound > Properties > Advanced ("Allow applications to take exclusive control of this
// device" / "Give exclusive mode applications priority"); we read those switches but never
// change them.
//
// Verified so far: type-checks against the mingw-w64 Windows headers (no real Windows run yet).
#if defined(_WIN32) || defined(WEZT_SIM_WINDOWS)

#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros break std::min/std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

#include "audio/audio_device.h"
#include "audio/sample_convert.h"

namespace wezt::audio {
namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kMaxCallbackFrames = 4096;

std::string toUtf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(std::max(n, 1)) - 1, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring toWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<std::size_t>(std::max(n, 1)) - 1, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::string hr(HRESULT h) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(h));
  return b;
}

// Defined locally so we don't depend on INITGUID / functiondiscoverykeys_devpkey.h linkage.
const PROPERTYKEY kFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
const PROPERTYKEY kAllowExclusive = {{0xB3F8FA53, 0x0004, 0x438E, {0x90, 0x03, 0x51, 0xA4, 0x6E, 0x13, 0x9B, 0xFC}}, 3};
const PROPERTYKEY kExclusivePriority = {{0xB3F8FA53, 0x0004, 0x438E, {0x90, 0x03, 0x51, 0xA4, 0x6E, 0x13, 0x9B, 0xFC}}, 4};

bool readUInt(IPropertyStore* ps, const PROPERTYKEY& key, unsigned long* out) {
  PROPVARIANT v;
  PropVariantInit(&v);
  bool ok = false;
  if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_UI4) {
    *out = v.ulVal;
    ok = true;
  }
  PropVariantClear(&v);
  return ok;
}

ComPtr<IMMDevice> openEndpoint(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& id) {
  ComPtr<IMMDevice> dev;
  if (id.empty()) {
    en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
  } else {
    const std::wstring w = toWide(id);
    en->GetDevice(w.c_str(), &dev);
  }
  return dev;
}

WAVEFORMATEXTENSIBLE makeFormat(SampleFormat f, unsigned channels, unsigned rate) {
  WAVEFORMATEXTENSIBLE x = {};
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = static_cast<WORD>(channels);
  x.Format.nSamplesPerSec = rate;
  x.Format.wBitsPerSample = static_cast<WORD>(f.bytesPerSample() * 8);
  x.Format.nBlockAlign = static_cast<WORD>(channels * f.bytesPerSample());
  x.Format.nAvgBytesPerSec = rate * x.Format.nBlockAlign;
  x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  x.Samples.wValidBitsPerSample = static_cast<WORD>(f.kind == SampleKind::Float32 ? 32 : f.validBits);
  x.dwChannelMask = channels >= 32 ? 0xFFFFFFFFu : ((1u << channels) - 1u);
  x.SubFormat = f.kind == SampleKind::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  return x;
}

struct PlanarFifo {  // single-thread FIFO (capture is drained on the render thread)
  std::vector<std::vector<float>> ch;
  std::size_t cap = 0, head = 0, count = 0;
  void init(unsigned channels, std::size_t capacity) {
    ch.assign(channels, std::vector<float>(capacity, 0.f));
    cap = capacity; head = 0; count = 0;
  }
  void push(const float* const* planar, unsigned frames) {
    for (unsigned f = 0; f < frames; ++f) {
      if (count == cap) { head = (head + 1) % cap; --count; }  // overrun: drop oldest
      const std::size_t idx = (head + count) % cap;
      for (std::size_t c = 0; c < ch.size(); ++c) ch[c][idx] = planar ? planar[c][f] : 0.f;
      ++count;
    }
  }
  void pop(float* const* dst, unsigned frames) {  // zero-fills on underrun
    unsigned got = 0;
    for (; got < frames && count > 0; ++got) {
      for (std::size_t c = 0; c < ch.size(); ++c) dst[c][got] = ch[c][head];
      head = (head + 1) % cap; --count;
    }
    for (unsigned f = got; f < frames; ++f)
      for (std::size_t c = 0; c < ch.size(); ++c) dst[c][f] = 0.f;
  }
};

// One WASAPI stream (render or capture) after negotiation.
struct Stream {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE event = nullptr;
  UINT32 bufFrames = 0;
  unsigned channels = 0;
  SampleFormat fmt;
  bool exclusive = false;
  void reset() {
    render.Reset(); capture.Reset(); client.Reset();
    if (event) { CloseHandle(event); event = nullptr; }
  }
};

class WasapiDevice : public AudioDevice {
 public:
  ~WasapiDevice() override { stop(); }
  std::string backendName() const override { return cfg_.exclusive ? "WASAPI (exclusive)" : "WASAPI (shared)"; }
  std::string notice() const override { return notice_; }

  bool open(const AudioConfig& requested, AudioCallback* cb, std::string* error) override {
    if (!cb || requested.outChannels == 0 || requested.bufferFrames == 0) {
      if (error) *error = "invalid configuration";
      return false;
    }
    cfg_ = requested;
    cb_ = cb;
    return true;
  }

  bool start(std::string* error) override {
    if (running_) return true;
    if (!cb_) { if (error) *error = "device not opened"; return false; }
    quit_ = false;
    notice_.clear();
    // The wake event belongs to this thread (start/stop), not to the audio thread: stop() signals it while the audio
    // thread may be on its way out, so the audio thread must never close it (ThreadSanitizer found that race).
    if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::promise<std::string> ready;
    auto fut = ready.get_future();
    thread_ = std::thread([this, p = std::move(ready)]() mutable { run(std::move(p)); });
    const std::string err = fut.get();
    if (!err.empty()) {
      if (thread_.joinable()) thread_.join();
      closeWake();
      if (error) *error = err;
      return false;
    }
    running_ = true;
    return true;
  }

  void stop() override {
    if (!running_) return;
    quit_ = true;
    if (wake_) SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    closeWake();
    running_ = false;
  }

  AudioConfig actualConfig() const override { return actual_; }
  void closeWake() { if (wake_) { CloseHandle(wake_); wake_ = nullptr; } }

 private:
  void run(std::promise<std::string> ready) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string err = init();
    ready.set_value(err);
    if (err.empty()) {
      DWORD taskIndex = 0;
      // MMCSS "Pro Audio" class: scheduler priority for the real-time audio thread.
      HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
      if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL);
      loop();
      if (task) AvRevertMmThreadCharacteristics(task);
      if (out_.client) out_.client->Stop();
      if (in_.client) in_.client->Stop();
    }
    out_.reset();
    in_.reset();
    if (SUCCEEDED(coHr)) CoUninitialize();
  }

  // Negotiates one stream. In exclusive mode tries the device's native formats; in shared mode
  // lets Windows convert to float at the requested rate.
  std::string openStream(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& id, unsigned channels, bool exclusive,
                         Stream& s) {
    const bool isRender = flow == eRender;
    ComPtr<IMMDevice> dev = openEndpoint(en, flow, id);
    if (!dev) return std::string("WASAPI: ") + (isRender ? "output" : "input") + " device not found";
    s.channels = channels;
    s.exclusive = exclusive;

    const REFERENCE_TIME wanted = static_cast<REFERENCE_TIME>(cfg_.bufferFrames) * 10000000LL / cfg_.sampleRate;

    if (!exclusive) {
      if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &s.client))) return "WASAPI: activate failed";
      s.fmt = {SampleKind::Float32, 32};
      WAVEFORMATEXTENSIBLE f = makeFormat(s.fmt, channels, cfg_.sampleRate);
      DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
      const HRESULT h = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, isRender ? wanted : wanted * 2, 0,
                                             reinterpret_cast<WAVEFORMATEX*>(&f), nullptr);
      if (FAILED(h)) return "WASAPI: shared-mode format rejected (" + hr(h) + ")";
    } else {
      // Candidates, best first. Many interfaces expose 24-bit PCM in a 32-bit container.
      const SampleFormat candidates[] = {
          {SampleKind::Int32, 24}, {SampleKind::Int24, 24}, {SampleKind::Int32, 32},
          {SampleKind::Int16, 16}, {SampleKind::Float32, 32}};
      auto activate = [&]() { s.client.Reset(); return SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &s.client)); };
      if (!activate()) return "WASAPI: activate failed";
      bool chosen = false;
      WAVEFORMATEXTENSIBLE f = {};
      for (const auto& c : candidates) {
        f = makeFormat(c, channels, cfg_.sampleRate);
        if (s.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&f), nullptr) == S_OK) {
          s.fmt = c;
          chosen = true;
          break;
        }
      }
      if (!chosen)
        return "WASAPI exclusive: device does not support " + std::to_string(channels) + " ch at " +
               std::to_string(cfg_.sampleRate) + " Hz in any PCM format";

      REFERENCE_TIME defPeriod = 0, minPeriod = 0;
      s.client->GetDevicePeriod(&defPeriod, &minPeriod);
      REFERENCE_TIME dur = std::max(wanted, minPeriod);
      HRESULT h = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                                       reinterpret_cast<WAVEFORMATEX*>(&f), nullptr);
      if (h == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // The hardware wants whole 128-byte blocks: ask for the aligned size, rebuild, retry.
        UINT32 aligned = 0;
        s.client->GetBufferSize(&aligned);
        if (!activate()) return "WASAPI: re-activate failed";
        dur = static_cast<REFERENCE_TIME>(10000000.0 / cfg_.sampleRate * aligned + 0.5);
        h = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                                 reinterpret_cast<WAVEFORMATEX*>(&f), nullptr);
      }
      if (h == AUDCLNT_E_DEVICE_IN_USE) return "WASAPI exclusive: device is in use by another application";
      if (h == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
        return "WASAPI exclusive: not allowed. Enable \"Allow applications to take exclusive control of this device\" in Sound > Properties > Advanced";
      if (FAILED(h)) return "WASAPI exclusive: initialize failed (" + hr(h) + ")";
    }

    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(s.client->SetEventHandle(s.event))) return "WASAPI: SetEventHandle failed";
    s.client->GetBufferSize(&s.bufFrames);
    if (isRender) {
      if (FAILED(s.client->GetService(IID_PPV_ARGS(&s.render)))) return "WASAPI: no render service";
    } else {
      if (FAILED(s.client->GetService(IID_PPV_ARGS(&s.capture)))) return "WASAPI: no capture service";
    }
    return {};
  }

  std::string init() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
      return "WASAPI: cannot create device enumerator";

    bool exclusive = cfg_.exclusive;
    std::string err = openStream(en.Get(), eRender, cfg_.outputDeviceId, cfg_.outChannels, exclusive, out_);
    if (!err.empty() && exclusive && cfg_.fallbackToShared) {
      notice_ = err + "; using shared mode";
      out_.reset();
      exclusive = false;
      err = openStream(en.Get(), eRender, cfg_.outputDeviceId, cfg_.outChannels, false, out_);
    }
    if (!err.empty()) return err;

    actual_ = cfg_;
    actual_.exclusive = exclusive;
    actual_.bufferFrames = out_.bufFrames;
    actual_.inChannels = 0;
    if (cfg_.inChannels > 0) {
      std::string ierr = openStream(en.Get(), eRecord(), cfg_.inputDeviceId, cfg_.inChannels, exclusive, in_);
      if (!ierr.empty() && exclusive && cfg_.fallbackToShared) {
        in_.reset();
        ierr = openStream(en.Get(), eRecord(), cfg_.inputDeviceId, cfg_.inChannels, false, in_);
        if (ierr.empty()) notice_ += (notice_.empty() ? "" : "; ") + std::string("input is in shared mode");
      }
      if (ierr.empty()) actual_.inChannels = cfg_.inChannels;
      else {
        in_.reset();  // continue output-only, but say so
        notice_ += (notice_.empty() ? "" : "; ") + ierr + " (continuing without input)";
      }
    }

    const unsigned inCh = std::max(1u, actual_.inChannels);
    fifo_.init(inCh, static_cast<std::size_t>(std::max(out_.bufFrames, in_.bufFrames)) * 8);
    inScratch_.assign(inCh, std::vector<float>(kMaxCallbackFrames, 0.f));
    outScratch_.assign(cfg_.outChannels, std::vector<float>(kMaxCallbackFrames, 0.f));
    inPtrs_.clear(); outPtrs_.clear(); capPtrs_.clear();
    for (auto& v : inScratch_) inPtrs_.push_back(v.data());
    for (auto& v : outScratch_) outPtrs_.push_back(v.data());
    capScratch_.assign(inCh, std::vector<float>(std::max<UINT32>(in_.bufFrames, 1) , 0.f));
    for (auto& v : capScratch_) capPtrs_.push_back(v.data());
    outBytes_.assign(static_cast<std::size_t>(out_.bufFrames) * cfg_.outChannels * out_.fmt.bytesPerSample(), 0);

    cb_->deviceAboutToStart(actual_);

    if (out_.exclusive) {  // event-driven exclusive streams must be primed before Start
      BYTE* data = nullptr;
      if (SUCCEEDED(out_.render->GetBuffer(out_.bufFrames, &data))) out_.render->ReleaseBuffer(out_.bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (in_.client) {
      const HRESULT ih = in_.client->Start();
      if (FAILED(ih)) {
        notice_ += (notice_.empty() ? "" : "; ") + std::string("input failed to start (") + hr(ih) + "), continuing without input";
        in_.capture.Reset();
      }
    }
    const HRESULT h = out_.client->Start();
    if (FAILED(h)) return "WASAPI: start failed (" + hr(h) + ")";
    return {};
  }

  static constexpr EDataFlow eRecord() { return eCapture; }

  void drainCapture() {
    if (!in_.capture) return;
    UINT32 packet = 0;
    while (SUCCEEDED(in_.capture->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr; UINT32 n = 0; DWORD flags = 0;
      if (FAILED(in_.capture->GetBuffer(&data, &n, &flags, nullptr, nullptr))) break;
      n = std::min<UINT32>(n, static_cast<UINT32>(capScratch_[0].size()));
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) fifo_.push(nullptr, n);
      else {
        interleavedToPlanar(data, actual_.inChannels, n, capPtrs_.data(), in_.fmt);
        fifo_.push(capPtrs_.data(), n);
      }
      in_.capture->ReleaseBuffer(n);
    }
  }

  void fillRender() {
    UINT32 avail = out_.bufFrames;
    if (!out_.exclusive) {
      UINT32 padding = 0;
      if (FAILED(out_.client->GetCurrentPadding(&padding))) return;
      avail = out_.bufFrames - padding;
    }
    if (avail == 0) return;
    BYTE* data = nullptr;
    if (FAILED(out_.render->GetBuffer(avail, &data))) return;
    UINT32 done = 0;
    const unsigned bps = out_.fmt.bytesPerSample();
    while (done < avail) {
      const unsigned n = std::min<UINT32>(avail - done, kMaxCallbackFrames);
      fifo_.pop(inPtrs_.data(), n);
      cb_->process(inPtrs_.data(), outPtrs_.data(), n);
      planarToInterleaved(outPtrs_.data(), cfg_.outChannels, n, data + static_cast<std::size_t>(done) * cfg_.outChannels * bps, out_.fmt);
      done += n;
    }
    out_.render->ReleaseBuffer(avail, 0);
  }

  void loop() {
    HANDLE handles[3];
    DWORD count = 0;
    handles[count++] = out_.event;
    const bool capEvent = in_.client && in_.event;
    if (capEvent) handles[count++] = in_.event;
    handles[count++] = wake_;
    while (!quit_) {
      const DWORD w = WaitForMultipleObjects(count, handles, FALSE, 2000);
      if (quit_) break;
      if (w == WAIT_TIMEOUT || w == WAIT_FAILED) continue;
      drainCapture();  // always drain first so input is as fresh as possible
      if (w == WAIT_OBJECT_0) fillRender();
    }
  }

  AudioConfig cfg_, actual_;
  AudioCallback* cb_ = nullptr;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  bool running_ = false;
  HANDLE wake_ = nullptr;
  Stream out_, in_;
  std::string notice_;
  PlanarFifo fifo_;
  std::vector<std::vector<float>> inScratch_, outScratch_, capScratch_;
  std::vector<float*> inPtrs_, outPtrs_, capPtrs_;
  std::vector<std::uint8_t> outBytes_;
};

}  // namespace

std::unique_ptr<AudioDevice> createPlatformDevice() { return std::make_unique<WasapiDevice>(); }

std::vector<AudioDeviceInfo> listDevices() {
  std::vector<AudioDeviceInfo> out;
  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    ComPtr<IMMDeviceEnumerator> en;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
      for (EDataFlow flow : {eRender, eCapture}) {
        std::string defaultId;
        {
          ComPtr<IMMDevice> d;
          LPWSTR wid = nullptr;
          if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d)) && SUCCEEDED(d->GetId(&wid))) {
            defaultId = toUtf8(wid);
            CoTaskMemFree(wid);
          }
        }
        ComPtr<IMMDeviceCollection> coll;
        UINT n = 0;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)) || FAILED(coll->GetCount(&n))) continue;
        for (UINT i = 0; i < n; ++i) {
          ComPtr<IMMDevice> d;
          if (FAILED(coll->Item(i, &d))) continue;
          AudioDeviceInfo info;
          LPWSTR wid = nullptr;
          if (SUCCEEDED(d->GetId(&wid))) { info.id = toUtf8(wid); CoTaskMemFree(wid); }
          ComPtr<IPropertyStore> ps;
          if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) info.name = toUtf8(v.pwszVal);
            PropVariantClear(&v);
            unsigned long allow = 1, prio = 0;
            const bool a = readUInt(ps.Get(), kAllowExclusive, &allow);
            const bool p = readUInt(ps.Get(), kExclusivePriority, &prio);
            info.exclusiveSettingsKnown = a || p;
            info.allowsExclusive = allow != 0;
            info.exclusivePriority = prio != 0;
          }
          ComPtr<IAudioClient> client;
          WAVEFORMATEX* mix = nullptr;
          unsigned ch = 0;
          if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) && SUCCEEDED(client->GetMixFormat(&mix))) {
            ch = mix->nChannels;
            CoTaskMemFree(mix);
          }
          if (flow == eRender) { info.outputChannels = ch; info.isDefaultOutput = info.id == defaultId; }
          else { info.inputChannels = ch; info.isDefaultInput = info.id == defaultId; }
          out.push_back(std::move(info));
        }
      }
    }
  }
  if (SUCCEEDED(coHr)) CoUninitialize();
  return out;
}

}  // namespace wezt::audio

#endif  // _WIN32 || WEZT_SIM_WINDOWS
