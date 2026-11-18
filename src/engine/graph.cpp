#include "engine/graph.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace wezt {

void Graph::prepareNode(Node& n) {
  n.in = AudioBuffer(channels_, maxFrames_);
  n.out = AudioBuffer(channels_, maxFrames_);
  n.proc->prepare(sampleRate_, maxFrames_);
}

Graph::NodeId Graph::add(std::unique_ptr<Processor> p) {
  nodes_.push_back(Node{});
  nodes_.back().proc = std::move(p);
  rebuildOrder();
  if (prepared_) {
    prepareNode(nodes_.back());
    refreshLatency();
  }
  return nodes_.size() - 1;
}

bool Graph::remove(NodeId n) {
  if (!alive(n)) return false;
  for (auto& other : nodes_) {
    for (std::size_t k = other.inputs.size(); k-- > 0;) {
      if (other.inputs[k] == n) {
        other.inputs.erase(other.inputs.begin() + static_cast<std::ptrdiff_t>(k));
        other.edgeDelay.erase(other.edgeDelay.begin() + static_cast<std::ptrdiff_t>(k));
      }
    }
  }
  nodes_[n] = Node{};
  rebuildOrder();
  if (prepared_) refreshLatency();
  return true;
}

// Is `target` reachable by following signal flow forward from `from`?
bool Graph::reaches(NodeId from, NodeId target) const {
  if (from == target) return true;
  std::vector<std::vector<NodeId>> consumers(nodes_.size());
  for (NodeId i = 0; i < nodes_.size(); ++i)
    for (NodeId in : nodes_[i].inputs) consumers[in].push_back(i);
  std::vector<char> seen(nodes_.size(), 0);
  std::vector<NodeId> stack{from};
  seen[from] = 1;
  while (!stack.empty()) {
    const NodeId n = stack.back();
    stack.pop_back();
    for (NodeId c : consumers[n]) {
      if (c == target) return true;
      if (!seen[c]) { seen[c] = 1; stack.push_back(c); }
    }
  }
  return false;
}

bool Graph::connect(NodeId from, NodeId to) {
  if (!alive(from) || !alive(to)) return false;
  if (reaches(to, from)) return false;  // to -> ... -> from already exists: adding from -> to makes a cycle
  auto& in = nodes_[to].inputs;
  if (std::find(in.begin(), in.end(), from) != in.end()) return true;  // already connected
  in.push_back(from);
  nodes_[to].edgeDelay.emplace_back();
  rebuildOrder();
  if (prepared_) refreshLatency();
  return true;
}

bool Graph::disconnect(NodeId from, NodeId to) {
  if (!alive(to)) return false;
  auto& in = nodes_[to].inputs;
  const auto it = std::find(in.begin(), in.end(), from);
  if (it == in.end()) return false;
  const auto k = it - in.begin();
  in.erase(it);
  nodes_[to].edgeDelay.erase(nodes_[to].edgeDelay.begin() + k);
  rebuildOrder();
  if (prepared_) refreshLatency();
  return true;
}

void Graph::rebuildOrder() {  // Kahn's algorithm, O(nodes + edges)
  const std::size_t n = nodes_.size();
  std::vector<std::size_t> indeg(n, 0);
  std::vector<std::vector<NodeId>> consumers(n);
  for (NodeId i = 0; i < n; ++i) {
    if (!alive(i)) continue;
    indeg[i] = nodes_[i].inputs.size();
    for (NodeId in : nodes_[i].inputs) consumers[in].push_back(i);
  }
  std::vector<NodeId> ready, order;
  for (NodeId i = 0; i < n; ++i)
    if (alive(i) && indeg[i] == 0) ready.push_back(i);
  while (!ready.empty()) {
    const NodeId x = ready.back();
    ready.pop_back();
    order.push_back(x);
    for (NodeId c : consumers[x])
      if (--indeg[c] == 0) ready.push_back(c);
  }
  order_ = std::move(order);
}

void Graph::prepare(double sampleRate, std::size_t channels, std::size_t maxFrames) {
  sampleRate_ = sampleRate;
  channels_ = channels;
  maxFrames_ = maxFrames;
  scratch_ = AudioBuffer(channels, maxFrames);
  prepared_ = true;
  for (auto& n : nodes_)
    if (n.proc) prepareNode(n);
  refreshLatency();  // plugins report their latency only after prepare()
}

void Graph::refreshLatency() {
  for (NodeId id : order_) {
    Node& n = nodes_[id];
    std::size_t maxIn = 0;
    for (NodeId src : n.inputs) maxIn = std::max(maxIn, nodes_[src].pathLatency);
    for (std::size_t k = 0; k < n.inputs.size(); ++k)
      n.edgeDelay[k].setDelay(maxIn - nodes_[n.inputs[k]].pathLatency, channels_);
    n.pathLatency = maxIn + n.proc->latencySamples();
  }
}

std::size_t Graph::latencyAt(NodeId n) const { return alive(n) ? nodes_[n].pathLatency : 0; }

std::size_t Graph::nodeCount() const {
  std::size_t c = 0;
  for (const auto& n : nodes_) c += n.proc ? 1 : 0;
  return c;
}

Processor* Graph::processor(NodeId n) { return alive(n) ? nodes_[n].proc.get() : nullptr; }

void Graph::render(AudioBuffer& out, std::size_t frames) {
  frames = std::min(frames, maxFrames_);
  if (out.channels != channels_ || out.capacity() < maxFrames_) out = AudioBuffer(channels_, maxFrames_);  // not RT-safe
  out.setFrames(frames);
  for (NodeId id : order_) {
    Node& n = nodes_[id];
    n.in.setFrames(frames);
    n.out.setFrames(frames);
    n.in.clearValid();
    for (std::size_t k = 0; k < n.inputs.size(); ++k) {
      const AudioBuffer& src = nodes_[n.inputs[k]].out;
      const AudioBuffer* use = &src;
      if (n.edgeDelay[k].delay() > 0) {
        scratch_.setFrames(frames);
        for (std::size_t c = 0; c < channels_; ++c) std::copy(src.channel(c), src.channel(c) + frames, scratch_.channel(c));
        n.edgeDelay[k].process(scratch_);
        use = &scratch_;
      }
      for (std::size_t c = 0; c < channels_; ++c) {
        float* d = n.in.channel(c);
        const float* s = use->channel(c);
        for (std::size_t i = 0; i < frames; ++i) d[i] += s[i];
      }
    }
    if (n.quarantined) {   // a node that kept producing NaN/inf stays silent until resetFault()
      for (std::size_t c = 0; c < channels_; ++c) std::fill(n.out.channel(c), n.out.channel(c) + frames, 0.f);
      continue;
    }
    n.proc->process(n.in, n.out);
    bool bad = false;
    for (std::size_t c = 0; c < channels_; ++c) {
      float* d = n.out.channel(c);
      for (std::size_t i = 0; i < frames; ++i) {
        std::uint32_t bits; std::memcpy(&bits, d + i, sizeof bits);
        if ((bits & 0x7f800000u) == 0x7f800000u) { d[i] = 0.f; bad = true; }   // exponent all ones: NaN or infinity
      }
    }
    if (bad) { ++n.faultBlocks; if (++n.badRun >= kQuarantineAfter) n.quarantined = true; } else n.badRun = 0;
  }
  if (alive(output_)) {
    const AudioBuffer& o = nodes_[output_].out;
    for (std::size_t c = 0; c < channels_; ++c) std::copy(o.channel(c), o.channel(c) + frames, out.channel(c));
  } else {
    out.clearValid();
  }
}

}  // namespace wezt
