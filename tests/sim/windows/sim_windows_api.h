#pragma once
// ComPtr (WRL-style), Win32 and COM functions, and the test controls for the Windows simulator.
#include "sim_windows.h"

#include <string>
#include <vector>

template <class T>
struct ComRef {  // what ComPtr::operator& hands out, like WRL's ComPtrRef: converts to T** and void**
  T** pp;
  operator T**() const { return pp; }
  operator void**() const { return reinterpret_cast<void**>(pp); }
};
template <class T> const GUID& wezt_sim_iid(ComRef<T>) { return uuidOf<T>(); }
template <class T> void** wezt_sim_ppv(ComRef<T> r) { return r; }
#define IID_PPV_ARGS(pp) wezt_sim_iid(pp), wezt_sim_ppv(pp)

namespace Microsoft::WRL {
template <class T>
class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
  ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ComPtr& operator=(const ComPtr& o) { if (this != &o) { Reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
  ComPtr& operator=(ComPtr&& o) noexcept { if (this != &o) { Reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
  ~ComPtr() { Reset(); }
  void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
  T* Get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  bool operator!() const { return p_ == nullptr; }
  ComRef<T> operator&() { Reset(); return ComRef<T>{&p_}; }

 private:
  T* p_ = nullptr;
};
}  // namespace Microsoft::WRL

// ---- Win32 ----
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xFFFFFFFFu, CP_UTF8 = 65001;
constexpr int AVRT_PRIORITY_CRITICAL = 2;
HANDLE CreateEventW(void*, BOOL manualReset, BOOL initial, LPCWSTR name);
BOOL SetEvent(HANDLE);
BOOL CloseHandle(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD ms);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* w, int wlen, char* out, int outLen, const char*, BOOL*);
int MultiByteToWideChar(UINT cp, DWORD flags, const char* s, int len, wchar_t* out, int outLen);
HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR task, DWORD* index);
BOOL AvSetMmThreadPriority(HANDLE, int);
BOOL AvRevertMmThreadCharacteristics(HANDLE);
HRESULT CoInitializeEx(void*, DWORD);
void CoUninitialize();
void CoTaskMemFree(void*);
HRESULT CoCreateInstance(const GUID& clsid, void* outer, DWORD ctx, const GUID& iid, void** out);

namespace wezt::sim::win {

struct FormatSpec {
  int kind;        // 0 float32, 1 int16, 2 int24 (packed), 3 int32 container
  unsigned validBits;
};

struct Endpoint {
  std::wstring id, name;
  bool capture = false;
  bool isDefault = false;
  unsigned mixChannels = 2;  // what shared mode reports
  unsigned maxChannels = 2;  // what exclusive mode can carry
  // Exclusive-mode hardware model (the "Advanced" tab switches and driver quirks)
  bool propsKnown = true, allowExclusive = true, exclusivePriority = false;
  bool inUseByOther = false;
  std::vector<FormatSpec> native;  // formats the driver accepts in exclusive mode
  unsigned alignFrames = 0;        // exclusive buffers must be a multiple of this (0 = no constraint)
  unsigned minPeriodFrames = 48;
  // Observations, filled in by the simulated hardware
  int exclusiveInitializeCalls = 0, sharedInitializeCalls = 0, notAlignedErrors = 0;
  bool lastWasExclusive = false;
  FormatSpec lastFormat{0, 32};
  unsigned lastBufferFrames = 0;
  bool running = false;
};

void reset();
Endpoint& addEndpoint(const std::wstring& id, const std::wstring& name, bool capture, unsigned channels, bool isDefault);
Endpoint* endpoint(const std::wstring& id);
// Decoded audio the backend wrote to a render endpoint (planar floats, one vector per channel).
std::vector<std::vector<float>> renderedAudio(const std::wstring& id);
// Input pattern generated for capture endpoints: channel c reads 0.1 * (c + 1).
long openHandles();       // events still open (leak check)
long liveComObjects();    // COM objects still alive (leak check)
long mmcssRegistrations(); // AvSetMmThreadCharacteristicsW calls (audio thread joined the "Pro Audio" class)
int lastMmcssPriority();   // last AvSetMmThreadPriority value (AVRT_PRIORITY_CRITICAL = 2)
void waitMs(int ms);

}  // namespace wezt::sim::win
