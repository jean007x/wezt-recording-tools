#include <vector>

#include "audio/audio_device.h"

namespace wezt::audio {

bool NullDevice::open(const AudioConfig& requested, AudioCallback* cb, std::string* error) {
  if (!cb || requested.outChannels == 0 || requested.bufferFrames == 0) {
    if (error) *error = "invalid configuration";
    return false;
  }
  cfg_ = requested;
  cb_ = cb;
  return true;
}

bool NullDevice::start(std::string* error) {
  if (!cb_) {
    if (error) *error = "device not opened";
    return false;
  }
  cb_->deviceAboutToStart(cfg_);
  started_ = true;
  return true;
}

void NullDevice::pump(unsigned blocks, unsigned callbackFrames, float inputValue) {
  if (!started_) return;
  std::vector<std::vector<float>> in(cfg_.inChannels, std::vector<float>(callbackFrames, inputValue));
  std::vector<std::vector<float>> out(cfg_.outChannels, std::vector<float>(callbackFrames, 0.f));
  std::vector<const float*> inPtrs;
  std::vector<float*> outPtrs;
  for (auto& v : in) inPtrs.push_back(v.data());
  for (auto& v : out) outPtrs.push_back(v.data());
  for (unsigned b = 0; b < blocks; ++b) cb_->process(inPtrs.data(), outPtrs.data(), callbackFrames);
}

}  // namespace wezt::audio
