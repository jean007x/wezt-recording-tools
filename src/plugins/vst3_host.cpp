#include "plugins/vst3_host.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <cstring>

#include "base/source/fobject.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"

using namespace Steinberg;

namespace wezt::plugins {
namespace {

std::string narrow(const Vst::TChar* s) {
  std::string out;
  for (; *s; ++s) out.push_back(*s < 128 ? static_cast<char>(*s) : '?');
  return out;
}

Vst::HostApplication& hostApp() {
  static Vst::HostApplication app;
  static bool once = (Vst::PluginContextFactory::instance().setPluginContext(&app), true);
  (void)once;
  return app;
}

// Single-producer / single-consumer ring for parameter edits from the UI thread.
struct ParamRing {
  struct Item { Vst::ParamID id; double value; };
  static constexpr std::size_t N = 512;
  std::array<Item, N> buf{};
  std::atomic<std::size_t> head{0}, tail{0};
  std::mutex producers;   // several control threads (UI, AI agent) may push; the audio thread only pops
  bool tryPush(Item it) {   // audio thread (automation): never waits for a control thread
    std::unique_lock<std::mutex> lock(producers, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const auto h = head.load(std::memory_order_relaxed);
    const auto n = (h + 1) % N;
    if (n == tail.load(std::memory_order_acquire)) return false;
    buf[h] = it;
    head.store(n, std::memory_order_release);
    return true;
  }
  bool push(Item it) {
    std::lock_guard<std::mutex> lock(producers);
    const auto h = head.load(std::memory_order_relaxed);
    const auto n = (h + 1) % N;
    if (n == tail.load(std::memory_order_acquire)) return false;
    buf[h] = it;
    head.store(n, std::memory_order_release);
    return true;
  }
  bool pop(Item& it) {
    const auto t = tail.load(std::memory_order_relaxed);
    if (t == head.load(std::memory_order_acquire)) return false;
    it = buf[t];
    tail.store((t + 1) % N, std::memory_order_release);
    return true;
  }
};

Vst::SpeakerArrangement arrangementFor(int channels) {
  switch (channels) {
    case 1: return Vst::SpeakerArr::kMono;
    case 2: return Vst::SpeakerArr::kStereo;
    default: return Vst::SpeakerArr::kStereo;
  }
}

}  // namespace

std::vector<Vst3ClassInfo> describeVst3(const std::string& modulePath, std::string* error) {
  std::string err;
  auto module = VST3::Hosting::Module::create(modulePath, err);
  if (!module) {
    if (error) *error = err;
    return {};
  }
  std::vector<Vst3ClassInfo> out;
  for (const auto& ci : module->getFactory().classInfos()) {
    if (ci.category() != kVstAudioEffectClass) continue;
    out.push_back({ci.name(), ci.vendor(), ci.category(), ci.subCategoriesString()});
  }
  return out;
}

// The host side of the controller's conversation: the plug-in tells us when its latency or parameter list changed.
// Edits a plug-in makes on its own (performEdit) are accepted and ignored: this host has no plug-in windows to originate them.
class ComponentHandler : public FObject, public Vst::IComponentHandler {
 public:
  tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
  tresult PLUGIN_API performEdit(Vst::ParamID, Vst::ParamValue) override { return kResultOk; }
  tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
  tresult PLUGIN_API restartComponent(int32 flags) override {
    if (flags & Vst::kLatencyChanged) latencyChanged.store(true, std::memory_order_release);
    if (flags & Vst::kReloadComponent) reloadRequested.store(true, std::memory_order_release);
    return kResultOk;
  }
  std::atomic<bool> latencyChanged{false}, reloadRequested{false};
  OBJ_METHODS(ComponentHandler, FObject)
  DEFINE_INTERFACES
    DEF_INTERFACE(Vst::IComponentHandler)
  END_DEFINE_INTERFACES(FObject)
  REFCOUNT_METHODS(FObject)
};

struct Vst3Instance::Impl {
  VST3::Hosting::Module::Ptr module;  // declared first: destroyed last
  IPtr<Vst::PlugProvider> provider;
  IPtr<Vst::IComponent> component;
  IPtr<Vst::IEditController> controller;
  FUnknownPtr<Vst::IAudioProcessor> processor;
  Vst::HostProcessData data;
  static constexpr std::size_t kMaxEditsPerBlock = 64;
  Vst::ParameterChanges paramChanges{static_cast<int32>(kMaxEditsPerBlock)};
  ParamRing ring;
  Vst::ParamID editIds[kMaxEditsPerBlock]{};
  double editValues[kMaxEditsPerBlock]{};
  ParamRing::Item carry{};  // an edit that did not fit in the previous block
  bool hasCarry = false;
  std::string name;
  int inChannels = 0, outChannels = 0;
  bool active = false;
  double sampleRate = 48000;
  int maxFrames = 0;

  // MIDI input (set up in load(); touched on the audio thread only through handleMidi/process)
  bool midiIn = false;
  Vst::EventList events{static_cast<int32>(kMaxMidiPerBlock) + 128};  // allocated here, never on the audio thread
  std::array<MidiEvent, kMaxMidiPerBlock> midi{};
  std::size_t midiN = 0;
  std::array<std::uint16_t, 128> held{};  // per key: bit per channel that has a note on, so AllNotesOff can release exactly those
  FUnknownPtr<Vst::IMidiMapping> midiMap;
  IPtr<ComponentHandler> handler;

  void deactivate() {
    if (!active) return;
    processor->setProcessing(false);
    component->setActive(false);
    data.unprepare();
    active = false;
  }
};

Vst3Instance::Vst3Instance() : impl_(std::make_unique<Impl>()) {}

Vst3Instance::~Vst3Instance() {
  if (impl_) {
    impl_->deactivate();
    if (impl_->controller) impl_->controller->setComponentHandler(nullptr);
    impl_->processor = nullptr;
    impl_->controller = nullptr;
    impl_->component = nullptr;
    impl_->provider = nullptr;
  }
}

std::unique_ptr<Vst3Instance> Vst3Instance::load(const std::string& modulePath, std::string* error, const std::string& className) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<Vst3Instance> {
    if (error) *error = m;
    return nullptr;
  };
  hostApp();
  std::unique_ptr<Vst3Instance> inst(new Vst3Instance());
  auto& d = *inst->impl_;

  std::string err;
  d.module = VST3::Hosting::Module::create(modulePath, err);
  if (!d.module) return fail("cannot load module: " + err);

  const auto& factory = d.module->getFactory();
  for (const auto& ci : factory.classInfos()) {
    if (ci.category() != kVstAudioEffectClass) continue;
    if (!className.empty() && ci.name() != className) continue;
    d.provider = owned(new Vst::PlugProvider(factory, ci, true));
    d.name = ci.name();
    break;
  }
  if (!d.provider) return fail(className.empty() ? "no audio module class in " + modulePath : "no audio module class called \"" + className + "\" in " + modulePath);
  if (!d.provider->initialize()) return fail("plugin failed to initialize: " + d.name);

  d.component = d.provider->getComponentPtr();
  d.controller = d.provider->getControllerPtr();
  d.processor = FUnknownPtr<Vst::IAudioProcessor>(d.component);
  if (!d.processor) return fail("component has no IAudioProcessor: " + d.name);

  d.inChannels = d.outChannels = 0;
  Vst::BusInfo bi;
  if (d.component->getBusCount(Vst::kAudio, Vst::kInput) > 0 &&
      d.component->getBusInfo(Vst::kAudio, Vst::kInput, 0, bi) == kResultOk)
    d.inChannels = bi.channelCount;
  if (d.component->getBusCount(Vst::kAudio, Vst::kOutput) > 0 &&
      d.component->getBusInfo(Vst::kAudio, Vst::kOutput, 0, bi) == kResultOk)
    d.outChannels = bi.channelCount;
  d.midiIn = d.component->getBusCount(Vst::kEvent, Vst::kInput) > 0;
  if (d.controller) {
    d.midiMap = FUnknownPtr<Vst::IMidiMapping>(d.controller);
    d.handler = owned(new ComponentHandler());
    d.controller->setComponentHandler(d.handler);
  }
  return inst;
}

void Vst3Instance::prepare(double sampleRate, std::size_t maxFrames) {
  auto& d = *impl_;
  d.deactivate();
  d.sampleRate = sampleRate;
  d.maxFrames = static_cast<int>(maxFrames);

  // Ask for the arrangement matching the bus' current width; plugins may refuse, which is fine.
  Vst::SpeakerArrangement in = arrangementFor(d.inChannels), out = arrangementFor(d.outChannels);
  d.processor->setBusArrangements(d.inChannels ? &in : nullptr, d.inChannels ? 1 : 0, d.outChannels ? &out : nullptr,
                                  d.outChannels ? 1 : 0);

  Vst::ProcessSetup setup;
  setup.processMode = Vst::kRealtime;
  setup.symbolicSampleSize = Vst::kSample32;
  setup.maxSamplesPerBlock = d.maxFrames;
  setup.sampleRate = sampleRate;
  d.processor->setupProcessing(setup);

  for (int i = 0; i < d.component->getBusCount(Vst::kAudio, Vst::kInput); ++i)
    d.component->activateBus(Vst::kAudio, Vst::kInput, i, i == 0);
  for (int i = 0; i < d.component->getBusCount(Vst::kAudio, Vst::kOutput); ++i)
    d.component->activateBus(Vst::kAudio, Vst::kOutput, i, i == 0);

  for (int i = 0; i < d.component->getBusCount(Vst::kEvent, Vst::kInput); ++i)
    d.component->activateBus(Vst::kEvent, Vst::kInput, i, i == 0);

  d.component->setActive(true);
  d.data.prepare(*d.component, d.maxFrames, Vst::kSample32);
  d.data.processMode = Vst::kRealtime;
  d.processor->setProcessing(true);  // kNotImplemented is acceptable
  d.active = true;
}

void Vst3Instance::process(const AudioBuffer& in, AudioBuffer& out) {
  auto& d = *impl_;
  if (!d.active || out.frames == 0 || static_cast<int>(out.frames) > d.maxFrames) {
    out.clear();
    return;
  }
  const int frames = static_cast<int>(out.frames);

  // Apply queued parameter edits. Repeated edits of one parameter collapse to the latest value, and
  // at most kMaxEditsPerBlock distinct parameters go in one block (the rest wait for the next one),
  // so the changes list never has to grow on the audio thread.
  d.paramChanges.clearQueue();
  std::size_t distinct = 0;
  auto apply = [&](const ParamRing::Item& e) {
    for (std::size_t k = 0; k < distinct; ++k)
      if (d.editIds[k] == e.id) { d.editValues[k] = e.value; return true; }
    if (distinct == Vst3Instance::Impl::kMaxEditsPerBlock) return false;
    d.editIds[distinct] = e.id;
    d.editValues[distinct++] = e.value;
    return true;
  };
  ParamRing::Item it;
  if (d.hasCarry) { d.hasCarry = false; apply(d.carry); }
  while (d.ring.pop(it)) {
    if (!apply(it)) { d.carry = it; d.hasCarry = true; break; }
  }
  for (std::size_t k = 0; k < distinct; ++k) {
    int32 idx = 0;
    if (auto* q = d.paramChanges.addParameterData(d.editIds[k], idx)) {
      int32 pt = 0;
      q->addPoint(0, d.editValues[k], pt);
    }
  }
  // MIDI from the sequencer or key preview, in time order. Notes become VST3 events; controllers and
  // pitch bend become parameter points through the plugin's own MIDI mapping.
  d.events.clear();
  if (d.midiIn && d.midiN) {
    std::stable_sort(d.midi.begin(), d.midi.begin() + static_cast<std::ptrdiff_t>(d.midiN), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
    auto noteEvent = [&](bool on, std::uint32_t off, int ch, int key, int vel) {
      Vst::Event e{};
      e.busIndex = 0; e.sampleOffset = static_cast<int32>(std::min<std::uint32_t>(off, static_cast<std::uint32_t>(frames - 1))); e.ppqPosition = 0; e.flags = 0;
      if (on) { e.type = Vst::Event::kNoteOnEvent; e.noteOn.channel = static_cast<int16>(ch); e.noteOn.pitch = static_cast<int16>(key); e.noteOn.velocity = std::clamp(vel, 1, 127) / 127.f; e.noteOn.length = 0; e.noteOn.tuning = 0.f; e.noteOn.noteId = -1; }
      else { e.type = Vst::Event::kNoteOffEvent; e.noteOff.channel = static_cast<int16>(ch); e.noteOff.pitch = static_cast<int16>(key); e.noteOff.velocity = 0.f; e.noteOff.tuning = 0.f; e.noteOff.noteId = -1; }
      d.events.addEvent(e);
    };
    auto point = [&](int ch, int ccOrBend, std::uint32_t off, double value) {
      if (!d.midiMap) return;
      Vst::ParamID pid = 0;
      if (d.midiMap->getMidiControllerAssignment(0, static_cast<int16>(ch), static_cast<Vst::CtrlNumber>(ccOrBend), pid) != kResultOk) return;
      int32 idx = 0;
      if (auto* q = d.paramChanges.addParameterData(pid, idx)) { int32 pt = 0; q->addPoint(static_cast<int32>(std::min<std::uint32_t>(off, static_cast<std::uint32_t>(frames - 1))), std::clamp(value, 0.0, 1.0), pt); }
    };
    for (std::size_t i = 0; i < d.midiN; ++i) {
      const MidiEvent& m = d.midi[i];
      const int ch = m.channel & 15, key = m.a & 127;
      switch (m.type) {
        case MidiEvent::NoteOn: noteEvent(true, m.offset, ch, key, m.b); d.held[static_cast<std::size_t>(key)] |= static_cast<std::uint16_t>(1u << ch); break;
        case MidiEvent::NoteOff: noteEvent(false, m.offset, ch, key, 0); d.held[static_cast<std::size_t>(key)] &= static_cast<std::uint16_t>(~(1u << ch)); break;
        case MidiEvent::AllNotesOff:
          for (int k = 0; k < 128; ++k)
            for (int c = 0; c < 16; ++c)
              if (d.held[static_cast<std::size_t>(k)] & (1u << c)) noteEvent(false, m.offset, c, k, 0);
          d.held.fill(0);
          break;
        case MidiEvent::Controller: point(ch, m.a, m.offset, m.b / 127.0); break;
        case MidiEvent::PitchBend: point(ch, Vst::kPitchBend, m.offset, m.a / 16383.0); break;
      }
    }
  }
  d.midiN = 0;
  d.data.inputEvents = d.midiIn ? &d.events : nullptr;
  d.data.inputParameterChanges = &d.paramChanges;
  d.data.numSamples = frames;

  if (d.data.numInputs > 0 && d.data.inputs) {
    auto& bus = d.data.inputs[0];
    for (int c = 0; c < bus.numChannels; ++c) {
      if (static_cast<std::size_t>(c) < in.channels && in.frames >= out.frames)
        std::copy(in.channel(c), in.channel(c) + frames, bus.channelBuffers32[c]);
      else
        std::fill(bus.channelBuffers32[c], bus.channelBuffers32[c] + frames, 0.f);
    }
  }

  if (d.processor->process(d.data) != kResultOk) {
    out.clear();
    return;
  }

  if (d.data.numOutputs > 0 && d.data.outputs) {
    auto& bus = d.data.outputs[0];
    for (std::size_t c = 0; c < out.channels; ++c) {
      const int src = bus.numChannels == 0 ? -1 : static_cast<int>(std::min<std::size_t>(c, bus.numChannels - 1));
      if (src < 0) std::fill(out.channel(c), out.channel(c) + frames, 0.f);
      else std::copy(bus.channelBuffers32[src], bus.channelBuffers32[src] + frames, out.channel(c));
    }
  } else {
    out.clear();
  }
}

bool Vst3Instance::acceptsMidi() const { return impl_->midiIn; }

void Vst3Instance::handleMidi(const MidiEvent* events, std::size_t count) {
  auto& d = *impl_;
  if (!d.midiIn) return;
  // Keep what an earlier call left (a block that did not run), then append.
  for (std::size_t i = 0; i < count && d.midiN < d.midi.size(); ++i) d.midi[d.midiN++] = events[i];
}

bool Vst3Instance::takeLatencyChanged() { return impl_->handler && impl_->handler->latencyChanged.exchange(false, std::memory_order_acq_rel); }

std::size_t Vst3Instance::latencySamples() const { return impl_->processor ? impl_->processor->getLatencySamples() : 0; }

std::size_t Vst3Instance::parameterCount() const {
  return impl_->controller ? static_cast<std::size_t>(impl_->controller->getParameterCount()) : 0;
}

std::string Vst3Instance::name() const { return impl_->name; }

std::vector<Vst3ParamInfo> Vst3Instance::parameters() const {
  std::vector<Vst3ParamInfo> out;
  auto* c = impl_->controller.get();
  if (!c) return out;
  for (int32 i = 0; i < c->getParameterCount(); ++i) {
    Vst::ParameterInfo pi;
    if (c->getParameterInfo(i, pi) != kResultOk) continue;
    Vst3ParamInfo p{pi.id, narrow(pi.title), pi.defaultNormalizedValue, (pi.flags & Vst::ParameterInfo::kIsHidden) != 0};
    p.min = c->normalizedParamToPlain(pi.id, 0.0);
    p.max = c->normalizedParamToPlain(pi.id, 1.0);
    p.unit = narrow(pi.units);
    p.steps = pi.stepCount;
    out.push_back(std::move(p));
  }
  return out;
}

std::string Vst3Instance::parameterText(unsigned id, double normalized) const {
  if (!impl_->controller) return {};
  Vst::String128 text = {0};
  if (impl_->controller->getParamStringByValue(id, std::clamp(normalized, 0.0, 1.0), text) != kResultOk) return {};
  return narrow(text);
}

double Vst3Instance::parameterNormalized(unsigned id) const {
  return impl_->controller ? impl_->controller->getParamNormalized(id) : 0.0;
}

bool Vst3Instance::setParameterNormalized(unsigned id, double value) {
  value = std::clamp(value, 0.0, 1.0);
  if (impl_->controller) impl_->controller->setParamNormalized(id, value);  // keep controller/UI in sync
  return impl_->ring.push({id, value});
}

bool Vst3Instance::automateParameter(unsigned id, double value) {   // audio thread; the controller is not touched (not thread-safe)
  return impl_->ring.tryPush({id, std::clamp(value, 0.0, 1.0)});
}

std::vector<std::uint8_t> Vst3Instance::saveState() const {
  MemoryStream stream;  // growable when default-constructed
  if (!impl_->component || impl_->component->getState(&stream) != kResultOk) return {};
  const auto* p = reinterpret_cast<const std::uint8_t*>(stream.getData());
  return std::vector<std::uint8_t>(p, p + stream.getSize());
}

bool Vst3Instance::loadState(const std::vector<std::uint8_t>& state) {
  if (!impl_->component || state.empty()) return false;
  MemoryStream stream(const_cast<std::uint8_t*>(state.data()), static_cast<TSize>(state.size()));
  if (impl_->component->setState(&stream) != kResultOk) return false;
  stream.seek(0, IBStream::kIBSeekSet, nullptr);
  if (impl_->controller) impl_->controller->setComponentState(&stream);
  return true;
}

}  // namespace wezt::plugins
