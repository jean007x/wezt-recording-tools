// Implementation of the Windows / COM / WASAPI model declared in sim_windows_api.h.
//
// IMPORTANT: this is a model written from our reading of the API documentation, not Microsoft's
// code. The simulated "hardware" runs one thread per started stream, signalling the stream's event
// once per period, which is how an event-driven WASAPI client is driven. Passing against it shows
// the backend is self-consistent; it does not prove behaviour on real Windows drivers.
#include "sim_windows_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "audio/sample_convert.h"

using namespace wezt::sim::win;

namespace {

// All events share one lock/condvar so WaitForMultipleObjects is simple and race-free.
std::mutex g_mu;
std::condition_variable g_cv;
std::atomic<long> g_handles{0}, g_com{0}, g_mmcss{0};
std::atomic<int> g_mmPrio{-1};

struct Event {
  bool signaled = false;
  bool manual = false;
};

std::map<std::wstring, std::unique_ptr<Endpoint>> g_endpoints;
std::vector<Endpoint*> g_order;
std::map<std::wstring, std::vector<std::vector<float>>> g_rendered;

wezt::audio::SampleFormat toSampleFormat(const FormatSpec& f) {
  using wezt::audio::SampleKind;
  static const SampleKind kinds[] = {SampleKind::Float32, SampleKind::Int16, SampleKind::Int24, SampleKind::Int32};
  return {kinds[f.kind], f.kind == 0 ? 32u : f.validBits};
}

FormatSpec fromWave(const WAVEFORMATEX* w) {
  const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
  const bool isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  if (isFloat) return {0, 32};
  switch (w->wBitsPerSample) {
    case 16: return {1, 16};
    case 24: return {2, x->Samples.wValidBitsPerSample};
    default: return {3, x->Samples.wValidBitsPerSample};
  }
}

template <class I>
struct ComObj : I {
  std::atomic<ULONG> refs{1};
  ComObj() { ++g_com; }
  ~ComObj() override { --g_com; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    const ULONG r = --refs;
    if (r == 0) delete this;
    return r;
  }
};

void fire(Event* e) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    e->signaled = true;
  }
  g_cv.notify_all();
}

wchar_t* wdup(const std::wstring& s) {
  auto* p = static_cast<wchar_t*>(std::malloc((s.size() + 1) * sizeof(wchar_t)));
  std::copy(s.begin(), s.end(), p);
  p[s.size()] = L'\0';
  return p;
}

struct Client;

struct RenderSvc : ComObj<IAudioRenderClient> {
  Client* c;
  explicit RenderSvc(Client* cl);
  ~RenderSvc() override;
  HRESULT GetBuffer(UINT32 frames, BYTE** data) override;
  HRESULT ReleaseBuffer(UINT32 frames, DWORD flags) override;
};
struct CaptureSvc : ComObj<IAudioCaptureClient> {
  Client* c;
  explicit CaptureSvc(Client* cl);
  ~CaptureSvc() override;
  HRESULT GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, uint64_t*, uint64_t*) override;
  HRESULT ReleaseBuffer(UINT32 frames) override;
  HRESULT GetNextPacketSize(UINT32* frames) override;
};

struct Client : ComObj<IAudioClient> {
  Endpoint* ep;
  bool initialized = false, exclusive = false;
  FormatSpec fmt{0, 32};
  unsigned channels = 0, rate = 48000;
  UINT32 bufFrames = 0, alignedHint = 0, padding = 0;
  Event* ev = nullptr;
  std::atomic<bool> running{false};
  std::thread th;
  std::vector<uint8_t> buf;                 // render: what the app writes into
  std::deque<std::vector<uint8_t>> packets;  // capture: what the "hardware" delivered
  UINT32 packetFrames = 0;

  explicit Client(Endpoint* e) : ep(e) {}
  ~Client() override { Stop(); }

  unsigned bps() const { return toSampleFormat(fmt).bytesPerSample(); }

  HRESULT Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME bufDur, REFERENCE_TIME period,
                     const WAVEFORMATEX* wf, const GUID*) override {
    if (initialized) return E_FAIL;
    const FormatSpec spec = fromWave(wf);
    const unsigned ch = wf->nChannels;
    const unsigned r = wf->nSamplesPerSec;
    const UINT32 frames = static_cast<UINT32>(static_cast<double>(bufDur) * r / 1e7 + 0.5);

    if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
      ++ep->exclusiveInitializeCalls;
      if (!ep->allowExclusive) return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
      if (ep->inUseByOther) return AUDCLNT_E_DEVICE_IN_USE;
      if (!supported(spec, ch)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
      // Event-driven exclusive streams need buffer duration == period and a period the hardware allows.
      if ((flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && bufDur != period) return AUDCLNT_E_INVALID_DEVICE_PERIOD;
      if (frames < ep->minPeriodFrames) return AUDCLNT_E_INVALID_DEVICE_PERIOD;
      if (ep->alignFrames && frames % ep->alignFrames != 0) {
        ++ep->notAlignedErrors;
        alignedHint = (frames + ep->alignFrames - 1) / ep->alignFrames * ep->alignFrames;
        return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;  // GetBufferSize now reports the aligned size
      }
      exclusive = true;
    } else {
      ++ep->sharedInitializeCalls;
      exclusive = false;
    }
    fmt = spec;
    channels = ch;
    rate = r;
    bufFrames = std::max<UINT32>(frames, 1);
    buf.assign(static_cast<std::size_t>(bufFrames) * channels * bps(), 0);
    initialized = true;
    ep->lastWasExclusive = exclusive;
    ep->lastFormat = spec;
    ep->lastBufferFrames = bufFrames;
    return S_OK;
  }

  bool supported(const FormatSpec& s, unsigned ch) const {
    if (ch == 0 || ch > ep->maxChannels) return false;
    for (const auto& n : ep->native)
      if (n.kind == s.kind && (s.kind == 0 || n.validBits == s.validBits)) return true;
    return false;
  }

  HRESULT GetBufferSize(UINT32* frames) override {
    *frames = initialized ? bufFrames : alignedHint;
    return S_OK;
  }
  HRESULT GetCurrentPadding(UINT32* frames) override {
    std::lock_guard<std::mutex> l(g_mu);
    if (ep->removed) return AUDCLNT_E_DEVICE_INVALIDATED;
    *frames = padding;
    return S_OK;
  }
  HRESULT IsFormatSupported(AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* wf, WAVEFORMATEX** closest) override {
    if (closest) *closest = nullptr;
    if (mode == AUDCLNT_SHAREMODE_SHARED) return S_OK;  // Windows converts
    return supported(fromWave(wf), wf->nChannels) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
  }
  HRESULT GetMixFormat(WAVEFORMATEX** out) override {
    auto* f = static_cast<WAVEFORMATEX*>(std::calloc(1, sizeof(WAVEFORMATEX)));
    f->wFormatTag = 3;
    f->nChannels = static_cast<WORD>(ep->mixChannels);
    f->nSamplesPerSec = 48000;
    f->wBitsPerSample = 32;
    f->nBlockAlign = static_cast<WORD>(f->nChannels * 4);
    f->nAvgBytesPerSec = f->nSamplesPerSec * f->nBlockAlign;
    *out = f;
    return S_OK;
  }
  HRESULT GetDevicePeriod(REFERENCE_TIME* def, REFERENCE_TIME* min) override {
    if (def) *def = 100000;  // 10 ms
    if (min) *min = static_cast<REFERENCE_TIME>(ep->minPeriodFrames) * 10000000LL / 48000;
    return S_OK;
  }
  HRESULT SetEventHandle(HANDLE h) override {
    ev = static_cast<Event*>(h);
    return ev ? S_OK : E_FAIL;
  }
  HRESULT GetService(REFIID iid, void** out) override {
    if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (iid == uuidOf<IAudioRenderClient>() && !ep->capture) { *out = new RenderSvc(this); return S_OK; }
    if (iid == uuidOf<IAudioCaptureClient>() && ep->capture) { *out = new CaptureSvc(this); return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT Start() override {
    if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
    if (running.exchange(true)) return S_OK;
    ep->running = true;
    th = std::thread([this] { hardware(); });
    return S_OK;
  }
  HRESULT Stop() override {
    if (running.exchange(false) || th.joinable()) {
      if (th.joinable()) th.join();
    }
    ep->running = false;
    return S_OK;
  }

  // The "device": once per period, consume (render) or deliver (capture) audio and signal the event.
  void hardware() {
    const UINT32 period = exclusive ? bufFrames : std::max<UINT32>(1, bufFrames / 2);
    const auto interval = std::chrono::nanoseconds(1000000000LL * period / rate);
    if (!exclusive && ev) fire(ev);  // shared streams ask for data right away
    while (running) {
      std::this_thread::sleep_for(interval);
      if (!running) break;
      if (ep->removed) continue;   // an unplugged device is silent: no data moves and no event fires
      if (ep->capture) {
        std::vector<std::vector<float>> planar(channels, std::vector<float>(period));
        std::vector<const float*> ptrs;
        for (unsigned c = 0; c < channels; ++c) {
          std::fill(planar[c].begin(), planar[c].end(), 0.1f * static_cast<float>(c + 1));
          ptrs.push_back(planar[c].data());
        }
        std::vector<uint8_t> bytes(static_cast<std::size_t>(period) * channels * bps());
        wezt::audio::planarToInterleaved(ptrs.data(), channels, period, bytes.data(), toSampleFormat(fmt));
        std::lock_guard<std::mutex> l(g_mu);
        packets.push_back(std::move(bytes));
        packetFrames = period;
      } else {
        std::lock_guard<std::mutex> l(g_mu);
        padding = padding > period ? padding - period : 0;
      }
      if (ev) fire(ev);
    }
  }
};

RenderSvc::RenderSvc(Client* cl) : c(cl) { c->AddRef(); }
RenderSvc::~RenderSvc() { c->Release(); }
HRESULT RenderSvc::GetBuffer(UINT32 frames, BYTE** data) {
  std::lock_guard<std::mutex> l(g_mu);
  if (frames > c->bufFrames - c->padding) return AUDCLNT_E_BUFFER_TOO_LARGE;
  *data = c->buf.data();
  return S_OK;
}
HRESULT RenderSvc::ReleaseBuffer(UINT32 frames, DWORD flags) {
  std::vector<std::vector<float>> planar(c->channels, std::vector<float>(frames, 0.f));
  std::vector<float*> ptrs;
  for (auto& v : planar) ptrs.push_back(v.data());
  if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
    wezt::audio::interleavedToPlanar(c->buf.data(), c->channels, frames, ptrs.data(), toSampleFormat(c->fmt));
  std::lock_guard<std::mutex> l(g_mu);
  auto& out = g_rendered[c->ep->id];
  out.resize(c->channels);
  for (unsigned ch = 0; ch < c->channels; ++ch) out[ch].insert(out[ch].end(), planar[ch].begin(), planar[ch].end());
  if (!c->exclusive) c->padding += frames;
  return S_OK;
}

CaptureSvc::CaptureSvc(Client* cl) : c(cl) { c->AddRef(); }
CaptureSvc::~CaptureSvc() { c->Release(); }
HRESULT CaptureSvc::GetNextPacketSize(UINT32* frames) {
  std::lock_guard<std::mutex> l(g_mu);
  *frames = c->packets.empty() ? 0 : c->packetFrames;
  return S_OK;
}
HRESULT CaptureSvc::GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, uint64_t*, uint64_t*) {
  std::lock_guard<std::mutex> l(g_mu);
  if (c->packets.empty()) { *frames = 0; *data = nullptr; return S_FALSE; }
  *data = c->packets.front().data();
  *frames = c->packetFrames;
  *flags = 0;
  return S_OK;
}
HRESULT CaptureSvc::ReleaseBuffer(UINT32) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!c->packets.empty()) c->packets.pop_front();
  return S_OK;
}

struct PropStore : ComObj<IPropertyStore> {
  Endpoint* ep;
  std::wstring name;
  explicit PropStore(Endpoint* e) : ep(e), name(e->name) {}
  HRESULT GetValue(REFPROPERTYKEY key, PROPVARIANT* v) override {
    PropVariantInit(v);
    static const GUID friendly = {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}};
    static const GUID exclusiveGuid = {0xB3F8FA53, 0x0004, 0x438E, {0x90, 0x03, 0x51, 0xA4, 0x6E, 0x13, 0x9B, 0xFC}};
    if (key.fmtid == friendly && key.pid == 14) {
      v->vt = VT_LPWSTR;
      v->pwszVal = name.data();  // owned by this store, valid until it is released
      return S_OK;
    }
    if (key.fmtid == exclusiveGuid && (key.pid == 3 || key.pid == 4)) {
      if (!ep->propsKnown) return E_NOTFOUND;
      v->vt = VT_UI4;
      v->ulVal = key.pid == 3 ? (ep->allowExclusive ? 1 : 0) : (ep->exclusivePriority ? 1 : 0);
      return S_OK;
    }
    return E_NOTFOUND;
  }
};

struct Device : ComObj<IMMDevice> {
  Endpoint* ep;
  explicit Device(Endpoint* e) : ep(e) {}
  HRESULT Activate(REFIID iid, DWORD, void*, void** out) override {
    if (iid == uuidOf<IAudioClient>()) { *out = new Client(ep); return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT OpenPropertyStore(DWORD, IPropertyStore** ps) override { *ps = new PropStore(ep); return S_OK; }
  HRESULT GetId(LPWSTR* id) override { *id = wdup(ep->id); return S_OK; }
};

struct Collection : ComObj<IMMDeviceCollection> {
  std::vector<Endpoint*> items;
  HRESULT GetCount(UINT* n) override { *n = static_cast<UINT>(items.size()); return S_OK; }
  HRESULT Item(UINT i, IMMDevice** d) override {
    if (i >= items.size()) return E_FAIL;
    *d = new Device(items[i]);
    return S_OK;
  }
};

struct Enumerator : ComObj<IMMDeviceEnumerator> {
  HRESULT EnumAudioEndpoints(EDataFlow flow, DWORD, IMMDeviceCollection** out) override {
    auto* c = new Collection();
    for (auto* e : g_order)
      if (flow == eAll || e->capture == (flow == eCapture)) c->items.push_back(e);
    *out = c;
    return S_OK;
  }
  HRESULT GetDefaultAudioEndpoint(EDataFlow flow, ERole, IMMDevice** out) override {
    for (auto* e : g_order)
      if (e->isDefault && e->capture == (flow == eCapture)) { *out = new Device(e); return S_OK; }
    *out = nullptr;
    return E_NOTFOUND;
  }
  HRESULT GetDevice(LPCWSTR id, IMMDevice** out) override {
    auto it = g_endpoints.find(id ? id : L"");
    if (it == g_endpoints.end()) { *out = nullptr; return E_NOTFOUND; }
    *out = new Device(it->second.get());
    return S_OK;
  }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Win32 / COM free functions
// ---------------------------------------------------------------------------------------------
HANDLE CreateEventW(void*, BOOL manualReset, BOOL initial, LPCWSTR) {
  auto* e = new Event();
  e->manual = manualReset != 0;
  e->signaled = initial != 0;
  ++g_handles;
  return e;
}
BOOL SetEvent(HANDLE h) {
  fire(static_cast<Event*>(h));
  return TRUE;
}
BOOL CloseHandle(HANDLE h) {
  delete static_cast<Event*>(h);
  --g_handles;
  return TRUE;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjects(1, &h, FALSE, ms); }
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL, DWORD ms) {
  std::unique_lock<std::mutex> l(g_mu);
  auto ready = [&]() -> int {
    for (DWORD i = 0; i < count; ++i)
      if (static_cast<Event*>(handles[i])->signaled) return static_cast<int>(i);
    return -1;
  };
  int idx = ready();
  if (idx < 0) {
    g_cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return ready() >= 0; });
    idx = ready();
  }
  if (idx < 0) return WAIT_TIMEOUT;
  auto* e = static_cast<Event*>(handles[idx]);
  if (!e->manual) e->signaled = false;
  return WAIT_OBJECT_0 + static_cast<DWORD>(idx);
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* w, int wlen, char* out, int outLen, const char*, BOOL*) {
  std::string s;
  const int n = wlen < 0 ? static_cast<int>(std::char_traits<wchar_t>::length(w)) : wlen;
  for (int i = 0; i < n; ++i) {
    const unsigned c = static_cast<unsigned>(w[i]);
    if (c < 0x80) s.push_back(static_cast<char>(c));
    else if (c < 0x800) { s.push_back(static_cast<char>(0xC0 | (c >> 6))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
    else { s.push_back(static_cast<char>(0xE0 | (c >> 12))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
  }
  if (wlen < 0) s.push_back('\0');
  if (!out || outLen == 0) return static_cast<int>(s.size());
  const int m = std::min<int>(outLen, static_cast<int>(s.size()));
  std::memcpy(out, s.data(), static_cast<std::size_t>(m));
  return m;
}
int MultiByteToWideChar(UINT, DWORD, const char* s, int len, wchar_t* out, int outLen) {
  std::wstring w;
  const int n = len < 0 ? static_cast<int>(std::strlen(s)) : len;
  for (int i = 0; i < n;) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { w.push_back(c); i += 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { w.push_back(static_cast<wchar_t>(((c & 0x1F) << 6) | (s[i + 1] & 0x3F))); i += 2; }
    else if (i + 2 < n) { w.push_back(static_cast<wchar_t>(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F))); i += 3; }
    else break;
  }
  if (len < 0) w.push_back(L'\0');
  if (!out || outLen == 0) return static_cast<int>(w.size());
  const int m = std::min<int>(outLen, static_cast<int>(w.size()));
  std::copy(w.begin(), w.begin() + m, out);
  return m;
}

HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR, DWORD* index) {
  static int token = 0;
  if (index) *index = 1;
  ++g_mmcss;
  return &token;
}
BOOL AvSetMmThreadPriority(HANDLE, int p) {
  g_mmPrio = p;
  return TRUE;
}
BOOL AvRevertMmThreadCharacteristics(HANDLE) { return TRUE; }

HRESULT CoInitializeEx(void*, DWORD) {
  thread_local bool inited = false;
  if (inited) return S_FALSE;  // already initialised on this thread; still needs a CoUninitialize
  inited = true;
  return S_OK;
}
void CoUninitialize() {}
void CoTaskMemFree(void* p) { std::free(p); }
HRESULT CoCreateInstance(const GUID& clsid, void*, DWORD, const GUID& iid, void** out) {
  if (clsid == uuidOf<MMDeviceEnumerator>() && iid == uuidOf<IMMDeviceEnumerator>()) {
    *out = new Enumerator();
    return S_OK;
  }
  *out = nullptr;
  return E_NOINTERFACE;
}

// ---------------------------------------------------------------------------------------------
// Test controls
// ---------------------------------------------------------------------------------------------
namespace wezt::sim::win {

void reset() {
  g_endpoints.clear();
  g_order.clear();
  g_rendered.clear();
  g_mmPrio = -1;
  g_mmcss = 0;
}

Endpoint& addEndpoint(const std::wstring& id, const std::wstring& name, bool capture, unsigned channels, bool isDefault) {
  auto e = std::make_unique<Endpoint>();
  e->id = id;
  e->name = name;
  e->capture = capture;
  e->isDefault = isDefault;
  e->mixChannels = channels;
  e->maxChannels = channels;
  e->native = {{3, 24}, {2, 24}, {1, 16}};  // typical interface: 24-in-32, packed 24, 16-bit
  Endpoint* raw = e.get();
  g_endpoints[id] = std::move(e);
  g_order.push_back(raw);
  return *raw;
}

Endpoint* endpoint(const std::wstring& id) {
  auto it = g_endpoints.find(id);
  return it == g_endpoints.end() ? nullptr : it->second.get();
}

std::vector<std::vector<float>> renderedAudio(const std::wstring& id) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_rendered.find(id);
  return it == g_rendered.end() ? std::vector<std::vector<float>>{} : it->second;
}

long openHandles() { return g_handles.load(); }
long liveComObjects() { return g_com.load(); }
long mmcssRegistrations() { return g_mmcss.load(); }
int lastMmcssPriority() { return g_mmPrio.load(); }
void waitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace wezt::sim::win
