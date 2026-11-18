#include "sim_coreaudio.h"

#include <algorithm>
#include <cstddef>

#include <cstring>
#include <map>

// ---------------- minimal CoreFoundation ----------------
struct SimCF {
  enum Kind { Str, Num, Dict, Arr } kind = Str;
  std::string s;
  int n = 0;
  std::map<std::string, const SimCF*> d;
  std::vector<const SimCF*> a;
  mutable int rc = 1;
};

static long g_live = 0;
static int dummyAllocator;
const CFAllocatorRef kCFAllocatorDefault = &dummyAllocator;
const SimCallbacks kCFTypeDictionaryKeyCallBacks = {}, kCFTypeDictionaryValueCallBacks = {}, kCFTypeArrayCallBacks = {};

static SimCF* make(SimCF::Kind k) {
  auto* p = new SimCF;
  p->kind = k;
  ++g_live;
  return p;
}
static const SimCF* retain(const SimCF* p) {
  ++p->rc;
  return p;
}

CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char* c, CFStringEncoding) {
  SimCF* p = make(SimCF::Str);
  p->s = c;
  return p;
}
bool CFStringGetCString(CFStringRef s, char* buf, long size, CFStringEncoding) {
  if (!s || static_cast<long>(s->s.size()) + 1 > size) return false;
  std::memcpy(buf, s->s.c_str(), s->s.size() + 1);
  return true;
}
void CFRelease(const void* v) {
  auto* p = static_cast<const SimCF*>(v);
  if (!p || --p->rc > 0) return;
  for (auto& kv : p->d) CFRelease(kv.second);
  for (auto* e : p->a) CFRelease(e);
  delete p;
  --g_live;
}
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, long, const SimCallbacks*, const SimCallbacks*) {
  return make(SimCF::Dict);
}
void CFDictionarySetValue(CFMutableDictionaryRef dict, const void* key, const void* value) {
  const std::string k = static_cast<const SimCF*>(key)->s;
  auto it = dict->d.find(k);
  if (it != dict->d.end()) CFRelease(it->second);
  dict->d[k] = retain(static_cast<const SimCF*>(value));
}
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef, long, const SimCallbacks*) { return make(SimCF::Arr); }
void CFArrayAppendValue(CFMutableArrayRef arr, const void* v) { arr->a.push_back(retain(static_cast<const SimCF*>(v))); }
CFNumberRef CFNumberCreate(CFAllocatorRef, int, const void* v) {
  SimCF* p = make(SimCF::Num);
  p->n = *static_cast<const int*>(v);
  return p;
}

// ---------------- fake hardware ----------------
using wezt::sim::mac::FakeDevice;

static std::map<AudioDeviceID, FakeDevice> g_devices;
static AudioDeviceID g_defOut = 0, g_defIn = 0, g_next = 10;

struct SimAudioComponent { int dummy; };
struct SimAudioUnit {
  bool enIn = false, enOut = true, init = false, started = false;
  AudioDeviceID dev = 0;
  UInt32 maxSlice = 0;
  unsigned cliOut = 0, cliIn = 0;
  AURenderCallbackStruct cb{nullptr, nullptr};
};
static std::vector<SimAudioUnit*> g_units;
static SimAudioComponent g_component;
static std::vector<std::vector<float>> g_lastOut;
static unsigned g_lastCliOut = 0, g_lastCliIn = 0;

namespace wezt::sim::mac {

void reset() {
  g_devices.clear();
  g_defOut = g_defIn = 0;
  g_next = 10;
  g_lastOut.clear();
  g_lastCliOut = g_lastCliIn = 0;
}
AudioDeviceID addDevice(const std::string& uid, const std::string& name, unsigned in, unsigned out) {
  FakeDevice d;
  d.id = g_next++;
  d.uid = uid;
  d.name = name;
  d.in = in;
  d.out = out;
  g_devices[d.id] = d;
  return d.id;
}
FakeDevice* deviceById(AudioDeviceID id) {
  auto it = g_devices.find(id);
  return it == g_devices.end() ? nullptr : &it->second;
}
FakeDevice* device(const std::string& uid) {
  for (auto& kv : g_devices)
    if (kv.second.uid == uid) return &kv.second;
  return nullptr;
}
void setDefault(bool output, const std::string& uid) {
  FakeDevice* d = device(uid);
  (output ? g_defOut : g_defIn) = d ? d->id : 0;
}
int aggregateCount() {
  int n = 0;
  for (auto& kv : g_devices) n += kv.second.isAggregate;
  return n;
}
long liveCFObjects() { return g_live; }
const std::vector<std::vector<float>>& lastOutput() { return g_lastOut; }
unsigned lastClientOutChannels() { return g_lastCliOut; }
unsigned lastClientInChannels() { return g_lastCliIn; }

void pump(unsigned blocks) {
  for (SimAudioUnit* u : g_units) {
    if (!u->started) continue;
    FakeDevice* d = deviceById(u->dev);
    if (!d) return;
    const UInt32 frames = d->bufFrames;
    std::vector<std::vector<float>> bufs(u->cliOut, std::vector<float>(frames, 0.f));
    std::vector<char> raw(offsetof(AudioBufferList, mBuffers) + std::max(1u, u->cliOut) * sizeof(AudioBuffer));
    auto* list = reinterpret_cast<AudioBufferList*>(raw.data());
    list->mNumberBuffers = u->cliOut;
    AudioTimeStamp ts{0};
    for (unsigned b = 0; b < blocks; ++b) {
      for (unsigned c = 0; c < u->cliOut; ++c) {
        std::fill(bufs[c].begin(), bufs[c].end(), 0.f);
        list->mBuffers[c] = {1, static_cast<UInt32>(frames * sizeof(float)), bufs[c].data()};
      }
      AudioUnitRenderActionFlags flags = 0;
      u->cb.inputProc(u->cb.inputProcRefCon, &flags, &ts, 0, frames, list);
      ts.mSampleTime += frames;
    }
    g_lastOut = bufs;
    g_lastCliOut = u->cliOut;
    g_lastCliIn = u->cliIn;
    return;
  }
}

}  // namespace wezt::sim::mac

using wezt::sim::mac::deviceById;

static const FakeDevice* findDev(AudioObjectID id) {
  return deviceById(id);
}

// Builds the bytes for a property. Returns false if the object/property is unknown.
static bool fetch(AudioObjectID obj, const AudioObjectPropertyAddress* a, const void* qualifier, std::vector<char>& out) {
  auto put = [&](const void* p, size_t n) {
    out.assign(static_cast<const char*>(p), static_cast<const char*>(p) + n);
  };
  if (obj == kAudioObjectSystemObject) {
    switch (a->mSelector) {
      case kAudioHardwarePropertyDefaultOutputDevice: put(&g_defOut, sizeof g_defOut); return true;
      case kAudioHardwarePropertyDefaultInputDevice: put(&g_defIn, sizeof g_defIn); return true;
      case kAudioHardwarePropertyDevices: {
        std::vector<AudioDeviceID> ids;
        for (auto& kv : g_devices)
          if (!kv.second.isPrivate) ids.push_back(kv.first);
        out.assign(reinterpret_cast<char*>(ids.data()), reinterpret_cast<char*>(ids.data()) + ids.size() * sizeof(AudioDeviceID));
        return true;
      }
      case kAudioHardwarePropertyTranslateUIDToDevice: {
        AudioDeviceID id = 0;
        if (qualifier) {
          const std::string uid = (*static_cast<const CFStringRef*>(qualifier))->s;
          if (FakeDevice* d = wezt::sim::mac::device(uid)) id = d->id;
        }
        put(&id, sizeof id);
        return true;
      }
    }
    return false;
  }
  if (const FakeDevice* d = findDev(obj)) {
    const unsigned chans = a->mScope == kAudioObjectPropertyScopeInput ? d->in : d->out;
    switch (a->mSelector) {
      case kAudioDevicePropertyStreamConfiguration: {
        std::vector<char> raw(offsetof(AudioBufferList, mBuffers) + (chans ? sizeof(AudioBuffer) : 0), 0);
        auto* l = reinterpret_cast<AudioBufferList*>(raw.data());
        l->mNumberBuffers = chans ? 1 : 0;
        if (chans) l->mBuffers[0] = {chans, 0, nullptr};
        out = raw;
        return true;
      }
      case kAudioDevicePropertyDeviceUID: { CFStringRef s = CFStringCreateWithCString(nullptr, d->uid.c_str(), 1); put(&s, sizeof s); return true; }
      case kAudioObjectPropertyName: { CFStringRef s = CFStringCreateWithCString(nullptr, d->name.c_str(), 1); put(&s, sizeof s); return true; }
      case kAudioDevicePropertyLatency: put(&d->latency, sizeof(UInt32)); return true;
      case kAudioDevicePropertySafetyOffset: put(&d->safety, sizeof(UInt32)); return true;
      case kAudioDevicePropertyStreams: {
        out.clear();
        if (chans) { AudioStreamID s = d->id * 100 + a->mScope; put(&s, sizeof s); }
        return true;
      }
      case kAudioDevicePropertyHogMode: put(&d->hogOwner, sizeof(pid_t)); return true;
      case kAudioDevicePropertyNominalSampleRate: put(&d->rate, sizeof(double)); return true;
      case kAudioDevicePropertyBufferFrameSize: { UInt32 f = d->bufFrames; put(&f, sizeof f); return true; }
    }
    return false;
  }
  if (obj >= 1000 && a->mSelector == kAudioStreamPropertyLatency) {  // stream object: devId*100 + scope
    if (const FakeDevice* d = findDev(obj / 100)) { put(&d->streamLatency, sizeof(UInt32)); return true; }
  }
  return false;
}

OSStatus AudioObjectGetPropertyDataSize(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32, const void* q, UInt32* size) {
  std::vector<char> b;
  if (!fetch(obj, a, q, b)) return -1;
  // CFString results are owned by the caller; a size query must not leak one.
  if (a->mSelector == kAudioDevicePropertyDeviceUID || a->mSelector == kAudioObjectPropertyName) CFRelease(*reinterpret_cast<CFStringRef*>(b.data()));
  *size = static_cast<UInt32>(b.size());
  return noErr;
}

OSStatus AudioObjectGetPropertyData(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32, const void* q, UInt32* size, void* data) {
  std::vector<char> b;
  if (!fetch(obj, a, q, b)) return -1;
  const UInt32 n = std::min<UInt32>(*size, static_cast<UInt32>(b.size()));
  std::memcpy(data, b.data(), n);
  *size = n;
  return noErr;
}

OSStatus AudioObjectSetPropertyData(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32, const void*, UInt32 size, const void* data) {
  FakeDevice* d = deviceById(obj);
  if (!d) return -1;
  switch (a->mSelector) {
    case kAudioDevicePropertyHogMode: {
      pid_t want;
      std::memcpy(&want, data, sizeof want);
      if (d->refuseHog) return -1;
      if (want != -1 && d->hogOwner != -1 && d->hogOwner != want) return -1;
      d->hogOwner = want;
      return noErr;
    }
    case kAudioDevicePropertyNominalSampleRate: {
      double r;
      std::memcpy(&r, data, sizeof r);
      if (r != 44100 && r != 48000 && r != 88200 && r != 96000) return -1;  // a device only supports certain rates
      d->rate = r;
      return noErr;
    }
    case kAudioDevicePropertyBufferFrameSize: {
      UInt32 f;
      std::memcpy(&f, data, std::min<size_t>(size, sizeof f));
      d->bufFrames = std::min<UInt32>(std::max<UInt32>(f, 64), 1024);  // hardware clamps the request
      return noErr;
    }
  }
  return -1;
}

AudioComponent AudioComponentFindNext(AudioComponent, const AudioComponentDescription* d) {
  return d->componentType == kAudioUnitType_Output && d->componentSubType == kAudioUnitSubType_HALOutput ? &g_component : nullptr;
}
OSStatus AudioComponentInstanceNew(AudioComponent, AudioUnit* u) {
  *u = new SimAudioUnit;
  g_units.push_back(*u);
  return noErr;
}
OSStatus AudioComponentInstanceDispose(AudioUnit u) {
  for (size_t i = 0; i < g_units.size(); ++i)
    if (g_units[i] == u) g_units.erase(g_units.begin() + static_cast<long>(i));
  delete u;
  return noErr;
}

OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void* data, UInt32) {
  switch (id) {
    case kAudioOutputUnitProperty_EnableIO: {
      UInt32 v;
      std::memcpy(&v, data, sizeof v);
      if (scope == kAudioUnitScope_Input && el == 1) u->enIn = v != 0;
      else if (scope == kAudioUnitScope_Output && el == 0) u->enOut = v != 0;
      else return -1;
      return noErr;
    }
    case kAudioOutputUnitProperty_CurrentDevice: {
      AudioDeviceID d;
      std::memcpy(&d, data, sizeof d);
      if (!deviceById(d)) return -1;
      u->dev = d;
      return noErr;
    }
    case kAudioUnitProperty_MaximumFramesPerSlice: std::memcpy(&u->maxSlice, data, sizeof u->maxSlice); return noErr;
    case kAudioUnitProperty_StreamFormat: {
      AudioStreamBasicDescription f;
      std::memcpy(&f, data, sizeof f);
      const FakeDevice* d = deviceById(u->dev);
      if (!d) return -1;
      const bool planarFloat = f.mFormatID == kAudioFormatLinearPCM && (f.mFormatFlags & kAudioFormatFlagIsFloat) &&
                               (f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) && f.mBitsPerChannel == 32;
      if (!planarFloat) return -1;
      if (scope == kAudioUnitScope_Input && el == 0) {  // client side of the output
        if (f.mChannelsPerFrame > d->out) return -10868;
        u->cliOut = f.mChannelsPerFrame;
        return noErr;
      }
      if (scope == kAudioUnitScope_Output && el == 1) {  // client side of the input
        if (!u->enIn || f.mChannelsPerFrame > d->in) return -10868;
        u->cliIn = f.mChannelsPerFrame;
        return noErr;
      }
      return -1;
    }
    case kAudioUnitProperty_SetRenderCallback: std::memcpy(&u->cb, data, sizeof u->cb); return noErr;
  }
  return -1;
}
OSStatus AudioUnitInitialize(AudioUnit u) {
  if (!u->dev || !u->cb.inputProc || !u->enOut || u->cliOut == 0) return -10875;
  u->init = true;
  return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit u) { u->init = false; return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit u) {
  if (!u->init) return -10867;
  u->started = true;
  return noErr;
}
OSStatus AudioOutputUnitStop(AudioUnit u) { u->started = false; return noErr; }

OSStatus AudioUnitRender(AudioUnit u, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32 bus, UInt32 frames, AudioBufferList* list) {
  if (!u->started || bus != 1 || !u->enIn || u->cliIn == 0 || list->mNumberBuffers != u->cliIn) return -1;
  for (UInt32 c = 0; c < list->mNumberBuffers; ++c) {
    if (list->mBuffers[c].mDataByteSize < frames * sizeof(float)) return -1;
    float* p = static_cast<float*>(list->mBuffers[c].mData);
    for (UInt32 f = 0; f < frames; ++f) p[f] = 0.1f * static_cast<float>(c + 1);
  }
  return noErr;
}

OSStatus AudioHardwareCreateAggregateDevice(CFDictionaryRef dict, AudioObjectID* out) {
  auto get = [&](const char* k) -> const SimCF* {
    auto it = dict->d.find(k);
    return it == dict->d.end() ? nullptr : it->second;
  };
  const SimCF *uid = get("uid"), *name = get("name"), *master = get("master"), *subs = get("subdevices"), *priv = get("private");
  if (!uid || !name || !master || !subs || subs->kind != SimCF::Arr) return -1;
  FakeDevice agg;
  agg.isAggregate = true;
  agg.uid = uid->s;
  agg.name = name->s;
  agg.masterUid = master->s;
  agg.isPrivate = priv && priv->n != 0;
  bool masterFound = false;
  for (const SimCF* sd : subs->a) {
    auto u = sd->d.find("uid");
    auto dr = sd->d.find("drift");
    if (u == sd->d.end()) return -1;
    FakeDevice* sub = wezt::sim::mac::device(u->second->s);
    if (!sub || sub->isAggregate) return -1;
    agg.in += sub->in;
    agg.out += sub->out;
    agg.subDevices.push_back({sub->uid, dr == sd->d.end() ? 0 : dr->second->n});
    if (sub->uid == agg.masterUid) {
      masterFound = true;
      agg.rate = sub->rate;
      agg.bufFrames = sub->bufFrames;
      agg.latency = sub->latency;
      agg.safety = sub->safety;
      agg.streamLatency = sub->streamLatency;
    }
  }
  if (!masterFound) return -1;
  agg.id = g_next++;
  g_devices[agg.id] = agg;
  *out = agg.id;
  return noErr;
}
OSStatus AudioHardwareDestroyAggregateDevice(AudioObjectID id) {
  FakeDevice* d = deviceById(id);
  if (!d || !d->isAggregate) return -1;
  g_devices.erase(id);
  return noErr;
}
