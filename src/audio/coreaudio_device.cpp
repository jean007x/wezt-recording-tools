// CoreAudio backend (macOS): device selection, sample rate and buffer size, exclusive access
// ("hog mode"), separate input/output devices via a private aggregate device, latency reporting.
//
// This follows the usual CoreAudio host structure (a HAL output unit bound to a device). It does
// not claim to match any other application's internals.
//
// Status: runs against Wezt's CoreAudio *simulator* (tests/sim/macos); not yet built against
// Apple's SDK or run on a Mac. The simulator only proves the logic against our own model of the
// API, so expect small fixes on first contact with real hardware.
#if defined(__APPLE__) || defined(WEZT_SIM_MACOS)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_device.h"

namespace wezt::audio {
namespace {

constexpr UInt32 kMaxFrames = 4096;
constexpr unsigned kMaxHwChannels = 64;

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  return {sel, scope, kAudioObjectPropertyElementMain};
}

std::string cfToUtf8(CFStringRef s) {
  if (!s) return {};
  char buf[512];
  if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return {};
  return buf;
}

struct CfStr {  // owning CFString
  CFStringRef ref = nullptr;
  explicit CfStr(const std::string& s) : ref(CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8)) {}
  ~CfStr() { if (ref) CFRelease(ref); }
  CfStr(const CfStr&) = delete;
  CfStr& operator=(const CfStr&) = delete;
};

unsigned channelCount(AudioDeviceID dev, AudioObjectPropertyScope scope) {
  AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreamConfiguration, scope);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
  std::vector<char> raw(size);
  auto* list = reinterpret_cast<AudioBufferList*>(raw.data());
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr) return 0;
  unsigned n = 0;
  for (UInt32 i = 0; i < list->mNumberBuffers; ++i) n += list->mBuffers[i].mNumberChannels;
  return n;
}

std::string stringProperty(AudioObjectID obj, AudioObjectPropertySelector sel) {
  AudioObjectPropertyAddress a = addr(sel);
  CFStringRef s = nullptr;
  UInt32 sz = sizeof(s);
  if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &sz, &s) != noErr || !s) return {};
  std::string out = cfToUtf8(s);
  CFRelease(s);
  return out;
}

std::string deviceUID(AudioDeviceID d) { return stringProperty(d, kAudioDevicePropertyDeviceUID); }
std::string deviceName(AudioDeviceID d) { return stringProperty(d, kAudioObjectPropertyName); }

AudioDeviceID defaultDevice(bool output) {
  AudioDeviceID d = kAudioObjectUnknown;
  UInt32 sz = sizeof(d);
  AudioObjectPropertyAddress a = addr(output ? kAudioHardwarePropertyDefaultOutputDevice : kAudioHardwarePropertyDefaultInputDevice);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &d) != noErr) return kAudioObjectUnknown;
  return d;
}

AudioDeviceID findByUID(const std::string& uid) {
  CfStr s(uid);
  AudioDeviceID d = kAudioObjectUnknown;
  UInt32 sz = sizeof(d);
  AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyTranslateUIDToDevice);
  CFStringRef q = s.ref;
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(q), &q, &sz, &d) != noErr) return kAudioObjectUnknown;
  return d;
}

// Hardware latency in frames: device latency + safety offset + first stream's latency.
UInt32 hardwareLatency(AudioDeviceID dev, AudioObjectPropertyScope scope) {
  UInt32 total = 0, v = 0, sz = sizeof(v);
  AudioObjectPropertyAddress a = addr(kAudioDevicePropertyLatency, scope);
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &v) == noErr) total += v;
  a = addr(kAudioDevicePropertySafetyOffset, scope);
  sz = sizeof(v);
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &v) == noErr) total += v;
  a = addr(kAudioDevicePropertyStreams, scope);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID)) {
    AudioStreamID stream = 0;
    UInt32 one = sizeof(stream);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &one, &stream) == noErr) {
      AudioObjectPropertyAddress la = addr(kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
      sz = sizeof(v);
      if (AudioObjectGetPropertyData(stream, &la, 0, nullptr, &sz, &v) == noErr) total += v;
    }
  }
  return total;
}

// Exclusive access. CoreAudio calls this "hog mode": the device is owned by one process and other
// applications can no longer play through it.
bool setHogMode(AudioDeviceID dev, bool on, std::string* why) {
  AudioObjectPropertyAddress a = addr(kAudioDevicePropertyHogMode);
  pid_t owner = -1;
  UInt32 sz = sizeof(owner);
  AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &owner);
  const pid_t me = getpid();
  if (on && owner != -1 && owner != me) {
    if (why) *why = "device is already in exclusive use by process " + std::to_string(owner);
    return false;
  }
  if (!on && owner != me) return true;  // not ours to release
  pid_t want = on ? me : -1;
  const OSStatus s = AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(want), &want);
  if (s != noErr) {
    if (why) *why = "driver refused exclusive access (OSStatus " + std::to_string(s) + ")";
    return false;
  }
  if (on) {
    owner = -1;
    sz = sizeof(owner);
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &owner);
    if (owner != me) {
      if (why) *why = "exclusive access was not granted";
      return false;
    }
  }
  return true;
}

class CoreAudioDevice : public AudioDevice {
 public:
  ~CoreAudioDevice() override { stop(); }
  std::string backendName() const override { return actual_.exclusive ? "CoreAudio (exclusive)" : "CoreAudio (HAL)"; }
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
    if (unit_) return true;
    notice_.clear();
    auto fail = [&](const std::string& msg, OSStatus s = 0) {
      if (error) *error = "CoreAudio: " + msg + (s ? " (OSStatus " + std::to_string(s) + ")" : "");
      teardown();
      return false;
    };

    // ---- 1. pick devices ----
    const AudioDeviceID outDev = cfg_.outputDeviceId.empty() ? defaultDevice(true) : findByUID(cfg_.outputDeviceId);
    if (outDev == kAudioObjectUnknown) return fail("output device not found");
    if (channelCount(outDev, kAudioObjectPropertyScopeOutput) == 0) return fail("selected device has no output channels");

    AudioDeviceID inDev = kAudioObjectUnknown;
    if (cfg_.inChannels > 0) {
      if (cfg_.inputDeviceId.empty() && channelCount(outDev, kAudioObjectPropertyScopeInput) > 0) {
        // An output device that carries its own inputs (an audio interface) serves both directions.
        inDev = outDev;
      } else {
        inDev = cfg_.inputDeviceId.empty() ? defaultDevice(false) : findByUID(cfg_.inputDeviceId);
        if (inDev != kAudioObjectUnknown && channelCount(inDev, kAudioObjectPropertyScopeInput) == 0) inDev = kAudioObjectUnknown;
        if (inDev == kAudioObjectUnknown && !cfg_.inputDeviceId.empty()) return fail("input device not found or has no inputs");
        if (inDev == kAudioObjectUnknown) notice_ = "no input device available; recording disabled";
      }
    }

    // ---- 2. exclusive access ----
    bool exclusive = false;
    if (cfg_.exclusive) {
      std::string why;
      bool ok = setHogMode(outDev, true, &why);
      if (ok) hogged_.push_back(outDev);
      if (ok && inDev != kAudioObjectUnknown && inDev != outDev) {
        ok = setHogMode(inDev, true, &why);
        if (ok) hogged_.push_back(inDev);
      }
      if (ok) {
        exclusive = true;
      } else {
        releaseHogs();
        if (!cfg_.fallbackToShared) return fail("exclusive access failed: " + why);
        notice_ += std::string(notice_.empty() ? "" : "; ") + "exclusive access failed (" + why + "); using shared mode";
      }
    }

    // ---- 3. one HAL device: the output device, or an aggregate when input and output differ ----
    AudioDeviceID halDev = outDev;
    if (inDev != kAudioObjectUnknown && inDev != outDev) {
      halDev = makeAggregate(outDev, inDev);
      if (halDev == kAudioObjectUnknown) {
        notice_ += std::string(notice_.empty() ? "" : "; ") + "could not combine input and output devices; recording disabled";
        inDev = kAudioObjectUnknown;
        halDev = outDev;
      }
    }
    const unsigned hwIn = inDev == kAudioObjectUnknown ? 0 : channelCount(halDev, kAudioObjectPropertyScopeInput);
    const unsigned hwOut = channelCount(halDev, kAudioObjectPropertyScopeOutput);
    if (hwOut == 0) return fail("device has no output channels");

    // ---- 4. HAL output unit ----
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &d);
    if (!comp) return fail("HAL output unit not found");
    OSStatus s = AudioComponentInstanceNew(comp, &unit_);
    if (s != noErr) return fail("cannot create audio unit", s);

    UInt32 on = 1, off = 0;
    AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, hwIn ? &on : &off, sizeof(UInt32));
    AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &on, sizeof(UInt32));
    s = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &halDev, sizeof(halDev));
    if (s != noErr) return fail("cannot bind device", s);

    // Sample rate and buffer size are device properties; ask, then read back what we really got.
    Float64 rate = cfg_.sampleRate;
    AudioObjectPropertyAddress rateAddr = addr(kAudioDevicePropertyNominalSampleRate);
    AudioObjectSetPropertyData(halDev, &rateAddr, 0, nullptr, sizeof(rate), &rate);  // best effort
    UInt32 rsz = sizeof(rate);
    AudioObjectGetPropertyData(halDev, &rateAddr, 0, nullptr, &rsz, &rate);

    UInt32 frames = cfg_.bufferFrames;
    AudioObjectPropertyAddress bufAddr = addr(kAudioDevicePropertyBufferFrameSize);
    AudioObjectSetPropertyData(halDev, &bufAddr, 0, nullptr, sizeof(frames), &frames);  // best effort
    UInt32 fsz = sizeof(frames);
    AudioObjectGetPropertyData(halDev, &bufAddr, 0, nullptr, &fsz, &frames);

    UInt32 maxSlice = kMaxFrames;
    AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxSlice, sizeof(maxSlice));

    // Client-side format: planar float, as many channels as both sides can carry.
    inChans_ = std::min({cfg_.inChannels, hwIn, kMaxHwChannels});
    outChans_ = std::min({cfg_.outChannels, hwOut, kMaxHwChannels});
    auto fmt = [&](unsigned ch) {
      AudioStreamBasicDescription f = {};
      f.mSampleRate = rate;
      f.mFormatID = kAudioFormatLinearPCM;
      f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
      f.mBitsPerChannel = 32;
      f.mChannelsPerFrame = ch;
      f.mFramesPerPacket = 1;
      f.mBytesPerFrame = f.mBytesPerPacket = 4;
      return f;
    };
    AudioStreamBasicDescription of = fmt(outChans_);
    s = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &of, sizeof(of));
    if (s != noErr) return fail("output format rejected", s);
    if (inChans_ > 0) {
      AudioStreamBasicDescription inf = fmt(inChans_);
      s = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inf, sizeof(inf));
      if (s != noErr) return fail("input format rejected", s);
    }

    // Scratch: input list for AudioUnitRender, zero channels for anything the device lacks.
    inScratch_.assign(std::max(1u, cfg_.inChannels), std::vector<float>(kMaxFrames, 0.f));
    outScratch_.assign(cfg_.outChannels, std::vector<float>(kMaxFrames, 0.f));
    inList_ = static_cast<AudioBufferList*>(std::malloc(offsetof(AudioBufferList, mBuffers) + std::max(1u, inChans_) * sizeof(::AudioBuffer)));
    inList_->mNumberBuffers = inChans_;
    for (unsigned c = 0; c < inChans_; ++c) {
      inList_->mBuffers[c].mNumberChannels = 1;
      inList_->mBuffers[c].mDataByteSize = kMaxFrames * sizeof(float);
      inList_->mBuffers[c].mData = inScratch_[c].data();
    }
    inPtrs_.clear();
    outPtrs_.assign(cfg_.outChannels, nullptr);
    for (auto& v : inScratch_) inPtrs_.push_back(v.data());

    AURenderCallbackStruct cbs = {&CoreAudioDevice::renderTrampoline, this};
    s = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cbs, sizeof(cbs));
    if (s != noErr) return fail("cannot set render callback", s);
    s = AudioUnitInitialize(unit_);
    if (s != noErr) return fail("initialize failed", s);

    actual_ = cfg_;
    actual_.sampleRate = static_cast<unsigned>(rate);
    actual_.bufferFrames = frames;
    actual_.inChannels = inChans_;
    actual_.exclusive = exclusive;
    actual_.outputLatencyFrames = hardwareLatency(outDev, kAudioObjectPropertyScopeOutput) + frames;
    actual_.inputLatencyFrames = inChans_ ? hardwareLatency(inDev, kAudioObjectPropertyScopeInput) + frames : 0;
    cb_->deviceAboutToStart(actual_);

    s = AudioOutputUnitStart(unit_);
    if (s != noErr) return fail("start failed", s);
    return true;
  }

  void stop() override { teardown(); }
  AudioConfig actualConfig() const override { return actual_; }

 private:
  // A private aggregate (invisible to other apps) that merges the chosen output and input devices.
  // The output device is the clock master; the input device is drift-corrected against it.
  AudioDeviceID makeAggregate(AudioDeviceID outDev, AudioDeviceID inDev) {
    const std::string outUid = deviceUID(outDev), inUid = deviceUID(inDev);
    if (outUid.empty() || inUid.empty()) return kAudioObjectUnknown;
    const std::string aggUid = "com.wezt.aggregate." + std::to_string(getpid());

    CfStr kUid("uid"), kName("name"), kSubs("subdevices"), kMaster("master"), kPrivate("private"), kStacked("stacked"), kDrift("drift");
    CfStr vUid(aggUid), vName("Wezt Recording Tools I/O"), vOut(outUid), vIn(inUid);
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableArrayRef subs = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    int one = 1, zero = 0;
    CFNumberRef cfOne = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    CFNumberRef cfZero = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &zero);
    for (int i = 0; i < 2; ++i) {
      CFMutableDictionaryRef sub = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CFDictionarySetValue(sub, kUid.ref, i == 0 ? vOut.ref : vIn.ref);
      CFDictionarySetValue(sub, kDrift.ref, i == 0 ? cfZero : cfOne);
      CFArrayAppendValue(subs, sub);
      CFRelease(sub);
    }
    CFDictionarySetValue(dict, kUid.ref, vUid.ref);
    CFDictionarySetValue(dict, kName.ref, vName.ref);
    CFDictionarySetValue(dict, kMaster.ref, vOut.ref);
    CFDictionarySetValue(dict, kPrivate.ref, cfOne);
    CFDictionarySetValue(dict, kStacked.ref, cfZero);
    CFDictionarySetValue(dict, kSubs.ref, subs);

    AudioObjectID agg = kAudioObjectUnknown;
    const OSStatus s = AudioHardwareCreateAggregateDevice(dict, &agg);
    CFRelease(subs); CFRelease(dict); CFRelease(cfOne); CFRelease(cfZero);
    if (s != noErr || agg == kAudioObjectUnknown) return kAudioObjectUnknown;

    // The new device shows up asynchronously; wait until it reports channels.
    for (int i = 0; i < 50; ++i) {
      if (channelCount(agg, kAudioObjectPropertyScopeOutput) > 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    aggregate_ = agg;
    return agg;
  }

  static OSStatus renderTrampoline(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                                   UInt32 frames, AudioBufferList* io) {
    return static_cast<CoreAudioDevice*>(ref)->render(flags, ts, bus, frames, io);
  }

  OSStatus render(AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 frames, AudioBufferList* io) {
    if (frames > kMaxFrames) {  // never expected; keep the audio thread safe
      for (UInt32 i = 0; i < io->mNumberBuffers; ++i) std::memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
      return noErr;
    }
    if (inChans_ > 0) {
      for (unsigned c = 0; c < inChans_; ++c) inList_->mBuffers[c].mDataByteSize = frames * sizeof(float);
      AudioUnitRenderActionFlags f = 0;
      if (AudioUnitRender(unit_, &f, ts, 1, frames, inList_) != noErr)
        for (unsigned c = 0; c < inChans_; ++c) std::fill(inScratch_[c].begin(), inScratch_[c].begin() + frames, 0.f);
    }
    // The engine always gets cfg_.outChannels pointers; channels the hardware lacks go to scratch.
    for (unsigned c = 0; c < cfg_.outChannels; ++c)
      outPtrs_[c] = c < io->mNumberBuffers && c < outChans_ ? static_cast<float*>(io->mBuffers[c].mData) : outScratch_[c].data();
    cb_->process(inPtrs_.data(), outPtrs_.data(), frames);
    // The engine may have left stale audio in scratch; the hardware doesn't read it, nothing to clear.
    for (UInt32 c = cfg_.outChannels; c < io->mNumberBuffers; ++c) std::memset(io->mBuffers[c].mData, 0, frames * sizeof(float));
    return noErr;
  }

  void releaseHogs() {
    for (AudioDeviceID d : hogged_) setHogMode(d, false, nullptr);
    hogged_.clear();
  }

  void teardown() {
    if (unit_) {
      AudioOutputUnitStop(unit_);
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
    }
    if (inList_) { std::free(inList_); inList_ = nullptr; }
    if (aggregate_ != kAudioObjectUnknown) {
      AudioHardwareDestroyAggregateDevice(aggregate_);
      aggregate_ = kAudioObjectUnknown;
    }
    releaseHogs();
  }

  AudioConfig cfg_, actual_;
  AudioCallback* cb_ = nullptr;
  AudioUnit unit_ = nullptr;
  unsigned inChans_ = 0, outChans_ = 0;
  AudioBufferList* inList_ = nullptr;
  AudioObjectID aggregate_ = kAudioObjectUnknown;
  std::vector<AudioDeviceID> hogged_;
  std::vector<std::vector<float>> inScratch_, outScratch_;
  std::vector<const float*> inPtrs_;
  std::vector<float*> outPtrs_;
  std::string notice_;
};

}  // namespace

std::unique_ptr<AudioDevice> createPlatformDevice() { return std::make_unique<CoreAudioDevice>(); }

std::vector<AudioDeviceInfo> listDevices() {
  std::vector<AudioDeviceInfo> out;
  AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr || size == 0) return out;
  std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return out;
  const AudioDeviceID defOut = defaultDevice(true), defIn = defaultDevice(false);
  for (AudioDeviceID id : ids) {
    AudioDeviceInfo info;
    info.id = deviceUID(id);
    info.name = deviceName(id);
    info.inputChannels = channelCount(id, kAudioObjectPropertyScopeInput);
    info.outputChannels = channelCount(id, kAudioObjectPropertyScopeOutput);
    info.isDefaultOutput = id == defOut;
    info.isDefaultInput = id == defIn;
    info.exclusiveSettingsKnown = false;  // Windows-only concept
    if (!info.id.empty() && (info.inputChannels || info.outputChannels)) out.push_back(std::move(info));
  }
  return out;
}

}  // namespace wezt::audio

#endif  // __APPLE__ || WEZT_SIM_MACOS
