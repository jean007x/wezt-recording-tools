#pragma once
// A stand-in for the slice of the Windows / COM / WASAPI API that Wezt's WASAPI backend uses, so the
// real backend source can be compiled and exercised on Linux.
//
// IMPORTANT: this is a model written from our reading of the API docs, not Microsoft's code.
// Passing against it shows the backend is self-consistent; it does not prove behaviour on Windows.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

typedef int32_t HRESULT, LONG;
typedef uint32_t DWORD, UINT, UINT32, ULONG;
typedef uint16_t WORD, VARTYPE;
typedef uint8_t BYTE;
typedef int BOOL;
typedef int64_t REFERENCE_TIME;
typedef void* HANDLE;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef const char* LPCSTR;
typedef char* LPSTR;
typedef void* LPVOID;
typedef void* LPUNKNOWN;

#define STDMETHODCALLTYPE
#define WINAPI
constexpr BOOL FALSE = 0, TRUE = 1;
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
constexpr HRESULT S_OK = 0, S_FALSE = 1;
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u), E_NOTFOUND = static_cast<HRESULT>(0x80070490u),
                  E_NOINTERFACE = static_cast<HRESULT>(0x80004002u);
constexpr HRESULT AUDCLNT_E_BUFFER_TOO_LARGE = static_cast<HRESULT>(0x88890006u),
                  AUDCLNT_E_UNSUPPORTED_FORMAT = static_cast<HRESULT>(0x88890008u),
                  AUDCLNT_E_DEVICE_IN_USE = static_cast<HRESULT>(0x8889000Au),
                  AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED = static_cast<HRESULT>(0x8889000Eu),
                  AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED = static_cast<HRESULT>(0x88890019u),
                  AUDCLNT_E_INVALID_DEVICE_PERIOD = static_cast<HRESULT>(0x88890020u),
                  AUDCLNT_E_NOT_INITIALIZED = static_cast<HRESULT>(0x88890001u),
                  AUDCLNT_E_DEVICE_INVALIDATED = static_cast<HRESULT>(0x88890004u),
                  AUDCLNT_E_SERVICE_NOT_RUNNING = static_cast<HRESULT>(0x88890010u),
                  AUDCLNT_E_RESOURCES_INVALIDATED = static_cast<HRESULT>(0x88890026u);

struct GUID {
  uint32_t Data1;
  uint16_t Data2, Data3;
  uint8_t Data4[8];
};
inline bool operator==(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }
typedef const GUID& REFIID;
typedef const GUID& REFCLSID;
struct PROPERTYKEY {
  GUID fmtid;
  DWORD pid;
};
typedef const PROPERTYKEY& REFPROPERTYKEY;

// ---- sample formats ----
constexpr WORD WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
struct WAVEFORMATEX {
  WORD wFormatTag, nChannels;
  DWORD nSamplesPerSec, nAvgBytesPerSec;
  WORD nBlockAlign, wBitsPerSample, cbSize;
};
struct WAVEFORMATEXTENSIBLE {
  WAVEFORMATEX Format;
  union {
    WORD wValidBitsPerSample, wSamplesPerBlock, wReserved;
  } Samples;
  DWORD dwChannelMask;
  GUID SubFormat;
};
inline const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
inline const GUID KSDATAFORMAT_SUBTYPE_PCM = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};

enum AUDCLNT_SHAREMODE { AUDCLNT_SHAREMODE_SHARED, AUDCLNT_SHAREMODE_EXCLUSIVE };
constexpr DWORD AUDCLNT_STREAMFLAGS_EVENTCALLBACK = 0x00040000, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM = 0x80000000,
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY = 0x08000000, AUDCLNT_BUFFERFLAGS_SILENT = 0x2;

// ---- COM ----
enum EDataFlow { eRender, eCapture, eAll };
enum ERole { eConsole, eMultimedia, eCommunications };
constexpr DWORD DEVICE_STATE_ACTIVE = 1, STGM_READ = 0, CLSCTX_ALL = 0x17, COINIT_MULTITHREADED = 0;
constexpr VARTYPE VT_EMPTY = 0, VT_UI4 = 19, VT_LPWSTR = 31;

struct IUnknown {
  virtual ULONG AddRef() = 0;
  virtual ULONG Release() = 0;
  virtual ~IUnknown() = default;
};

struct PROPVARIANT {
  VARTYPE vt;
  union {
    ULONG ulVal;
    LPWSTR pwszVal;
  };
};
inline void PropVariantInit(PROPVARIANT* v) { std::memset(v, 0, sizeof *v); }
inline HRESULT PropVariantClear(PROPVARIANT* v) { PropVariantInit(v); return S_OK; }

struct IPropertyStore : IUnknown {
  virtual HRESULT GetValue(REFPROPERTYKEY key, PROPVARIANT* v) = 0;
};
struct IAudioRenderClient : IUnknown {
  virtual HRESULT GetBuffer(UINT32 frames, BYTE** data) = 0;
  virtual HRESULT ReleaseBuffer(UINT32 frames, DWORD flags) = 0;
};
struct IAudioCaptureClient : IUnknown {
  virtual HRESULT GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, uint64_t* devPos, uint64_t* qpc) = 0;
  virtual HRESULT ReleaseBuffer(UINT32 frames) = 0;
  virtual HRESULT GetNextPacketSize(UINT32* frames) = 0;
};
struct IAudioClient : IUnknown {
  virtual HRESULT Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME bufDur, REFERENCE_TIME period,
                             const WAVEFORMATEX* fmt, const GUID* session) = 0;
  virtual HRESULT GetBufferSize(UINT32* frames) = 0;
  virtual HRESULT GetCurrentPadding(UINT32* frames) = 0;
  virtual HRESULT IsFormatSupported(AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* fmt, WAVEFORMATEX** closest) = 0;
  virtual HRESULT GetMixFormat(WAVEFORMATEX** fmt) = 0;
  virtual HRESULT GetDevicePeriod(REFERENCE_TIME* def, REFERENCE_TIME* min) = 0;
  virtual HRESULT Start() = 0;
  virtual HRESULT Stop() = 0;
  virtual HRESULT SetEventHandle(HANDLE h) = 0;
  virtual HRESULT GetService(REFIID iid, void** out) = 0;
};
struct IMMDevice : IUnknown {
  virtual HRESULT Activate(REFIID iid, DWORD ctx, void* params, void** out) = 0;
  virtual HRESULT OpenPropertyStore(DWORD access, IPropertyStore** ps) = 0;
  virtual HRESULT GetId(LPWSTR* id) = 0;
};
struct IMMDeviceCollection : IUnknown {
  virtual HRESULT GetCount(UINT* n) = 0;
  virtual HRESULT Item(UINT i, IMMDevice** d) = 0;
};
struct IMMDeviceEnumerator : IUnknown {
  virtual HRESULT EnumAudioEndpoints(EDataFlow flow, DWORD state, IMMDeviceCollection** out) = 0;
  virtual HRESULT GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** out) = 0;
  virtual HRESULT GetDevice(LPCWSTR id, IMMDevice** out) = 0;
};
struct MMDeviceEnumerator {};

template <class T> const GUID& uuidOf();
#define WEZT_SIM_UUID(T, N) \
  template <> inline const GUID& uuidOf<T>() { static const GUID g = {N, 0, 0, {0, 0, 0, 0, 0, 0, 0, 1}}; return g; }
WEZT_SIM_UUID(IAudioClient, 0xA001)
WEZT_SIM_UUID(IAudioRenderClient, 0xA002)
WEZT_SIM_UUID(IAudioCaptureClient, 0xA003)
WEZT_SIM_UUID(IMMDeviceEnumerator, 0xA004)
WEZT_SIM_UUID(MMDeviceEnumerator, 0xA005)
WEZT_SIM_UUID(IMMDevice, 0xA006)
#define __uuidof(T) uuidOf<T>()

