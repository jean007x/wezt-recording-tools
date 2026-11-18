#pragma once
// A stand-in for the small slice of Apple's CoreAudio / AudioToolbox / CoreFoundation API that
// Wezt's macOS backend uses, so that backend can be compiled and exercised on Linux.
//
// IMPORTANT: this is a model written from our reading of the API docs, not Apple's code. Passing
// against it shows the backend is self-consistent; it does not prove behaviour on a real Mac.
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef double Float64;
typedef float Float32;
typedef UInt32 AudioObjectID, AudioDeviceID, AudioStreamID;
typedef UInt32 AudioObjectPropertySelector, AudioObjectPropertyScope, AudioObjectPropertyElement;
typedef UInt32 AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, AudioUnitRenderActionFlags;

constexpr OSStatus noErr = 0;
constexpr AudioObjectID kAudioObjectUnknown = 0, kAudioObjectSystemObject = 1;
constexpr AudioObjectPropertyScope kAudioObjectPropertyScopeGlobal = 1, kAudioObjectPropertyScopeInput = 2,
                                   kAudioObjectPropertyScopeOutput = 3;
constexpr AudioObjectPropertyElement kAudioObjectPropertyElementMain = 0;

enum : AudioObjectPropertySelector {
  kAudioHardwarePropertyDefaultOutputDevice = 100,
  kAudioHardwarePropertyDefaultInputDevice,
  kAudioHardwarePropertyDevices,
  kAudioHardwarePropertyTranslateUIDToDevice,
  kAudioDevicePropertyStreamConfiguration,
  kAudioDevicePropertyDeviceUID,
  kAudioObjectPropertyName,
  kAudioDevicePropertyLatency,
  kAudioDevicePropertySafetyOffset,
  kAudioDevicePropertyStreams,
  kAudioStreamPropertyLatency,
  kAudioDevicePropertyHogMode,
  kAudioDevicePropertyNominalSampleRate,
  kAudioDevicePropertyBufferFrameSize,
  kAudioDevicePropertyDeviceIsAlive
};

struct AudioObjectPropertyAddress {
  AudioObjectPropertySelector mSelector;
  AudioObjectPropertyScope mScope;
  AudioObjectPropertyElement mElement;
};

struct AudioBuffer {
  UInt32 mNumberChannels;
  UInt32 mDataByteSize;
  void* mData;
};
struct AudioBufferList {
  UInt32 mNumberBuffers;
  AudioBuffer mBuffers[1];
};

struct AudioStreamBasicDescription {
  Float64 mSampleRate;
  UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved;
};
constexpr UInt32 kAudioFormatLinearPCM = 1;
constexpr UInt32 kAudioFormatFlagIsFloat = 1, kAudioFormatFlagIsPacked = 8, kAudioFormatFlagIsNonInterleaved = 32;

struct AudioTimeStamp {
  Float64 mSampleTime;
};
struct AudioComponentDescription {
  UInt32 componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask;
};
constexpr UInt32 kAudioUnitType_Output = 1, kAudioUnitSubType_HALOutput = 2, kAudioUnitManufacturer_Apple = 3;

typedef struct SimAudioComponent* AudioComponent;
typedef struct SimAudioUnit* AudioUnit;
typedef OSStatus (*AURenderCallback)(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
struct AURenderCallbackStruct {
  AURenderCallback inputProc;
  void* inputProcRefCon;
};

enum : AudioUnitPropertyID {
  kAudioOutputUnitProperty_EnableIO = 200,
  kAudioOutputUnitProperty_CurrentDevice,
  kAudioUnitProperty_MaximumFramesPerSlice,
  kAudioUnitProperty_StreamFormat,
  kAudioUnitProperty_SetRenderCallback
};
constexpr AudioUnitScope kAudioUnitScope_Global = 0, kAudioUnitScope_Input = 1, kAudioUnitScope_Output = 2;

typedef OSStatus (*AudioObjectPropertyListenerProc)(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*);
OSStatus AudioObjectAddPropertyListener(AudioObjectID, const AudioObjectPropertyAddress*, AudioObjectPropertyListenerProc, void*);
OSStatus AudioObjectRemovePropertyListener(AudioObjectID, const AudioObjectPropertyAddress*, AudioObjectPropertyListenerProc, void*);
OSStatus AudioObjectGetPropertyDataSize(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
OSStatus AudioObjectGetPropertyData(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*, void*);
OSStatus AudioObjectSetPropertyData(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);

AudioComponent AudioComponentFindNext(AudioComponent, const AudioComponentDescription*);
OSStatus AudioComponentInstanceNew(AudioComponent, AudioUnit*);
OSStatus AudioComponentInstanceDispose(AudioUnit);
OSStatus AudioUnitSetProperty(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void*, UInt32);
OSStatus AudioUnitInitialize(AudioUnit);
OSStatus AudioUnitUninitialize(AudioUnit);
OSStatus AudioOutputUnitStart(AudioUnit);
OSStatus AudioOutputUnitStop(AudioUnit);
OSStatus AudioUnitRender(AudioUnit, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);

// ---- minimal CoreFoundation ----
struct SimCF;
typedef const SimCF* CFStringRef;
typedef const SimCF* CFNumberRef;
typedef const SimCF* CFDictionaryRef;
typedef SimCF* CFMutableDictionaryRef;
typedef SimCF* CFMutableArrayRef;
typedef const void* CFAllocatorRef;
typedef int CFStringEncoding;
extern const CFAllocatorRef kCFAllocatorDefault;
constexpr CFStringEncoding kCFStringEncodingUTF8 = 1;
constexpr int kCFNumberIntType = 1;
struct SimCallbacks {};
extern const SimCallbacks kCFTypeDictionaryKeyCallBacks, kCFTypeDictionaryValueCallBacks, kCFTypeArrayCallBacks;

CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char*, CFStringEncoding);
bool CFStringGetCString(CFStringRef, char*, long, CFStringEncoding);
void CFRelease(const void*);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, long, const SimCallbacks*, const SimCallbacks*);
void CFDictionarySetValue(CFMutableDictionaryRef, const void* key, const void* value);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef, long, const SimCallbacks*);
void CFArrayAppendValue(CFMutableArrayRef, const void*);
CFNumberRef CFNumberCreate(CFAllocatorRef, int, const void*);

OSStatus AudioHardwareCreateAggregateDevice(CFDictionaryRef, AudioObjectID*);
OSStatus AudioHardwareDestroyAggregateDevice(AudioObjectID);

// ---- test controls ----
namespace wezt::sim::mac {

struct FakeDevice {
  AudioDeviceID id = 0;
  std::string uid, name;
  unsigned in = 0, out = 0;
  unsigned latency = 0, safety = 0, streamLatency = 0;
  double rate = 48000;
  unsigned bufFrames = 512;
  pid_t hogOwner = -1;
  bool refuseHog = false;
  bool isPrivate = false;
  bool isAggregate = false;
  bool alive = true;   // false after killDevice(): the hardware went away
  std::vector<std::pair<AudioObjectPropertyListenerProc, void*>> listeners;
  // aggregate bookkeeping, for assertions
  std::string masterUid;
  std::vector<std::pair<std::string, int>> subDevices;  // uid, drift-compensation flag
};

void reset();
AudioDeviceID addDevice(const std::string& uid, const std::string& name, unsigned in, unsigned out);
FakeDevice* device(const std::string& uid);
FakeDevice* deviceById(AudioDeviceID id);
void setDefault(bool output, const std::string& uid);
void killDevice(AudioDeviceID id);   // unplug: marks it dead and calls the DeviceIsAlive listeners
int listenerCount(AudioDeviceID id);
int aggregateCount();
long liveCFObjects();
// Runs the started unit's render callback `blocks` times at the device buffer size. Each input
// channel c reads as 0.1 * (c + 1). The last output block is kept in lastOutput().
void pump(unsigned blocks);
const std::vector<std::vector<float>>& lastOutput();
unsigned lastClientOutChannels();
unsigned lastClientInChannels();

}  // namespace wezt::sim::mac
