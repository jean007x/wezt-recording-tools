// Fallback for platforms with no native backend (e.g. Linux CI): no hardware device.
#if !defined(_WIN32) && !defined(__APPLE__)
#include "audio/audio_device.h"

namespace wezt::audio {
std::unique_ptr<AudioDevice> createPlatformDevice() { return nullptr; }
std::vector<AudioDeviceInfo> listDevices() { return {}; }
}  // namespace wezt::audio
#endif
