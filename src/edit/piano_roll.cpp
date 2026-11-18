#include "edit/piano_roll.h"

#include <algorithm>
#include <cmath>

namespace wezt::edit {
using music::Note;

namespace {
double snap(double v, double grid) { return grid > 0 ? std::round(v / grid) * grid : v; }
int clampPitch(int p) { return std::clamp(p, 0, 127); }
int clampVel(double v) { return std::clamp(static_cast<int>(std::lround(v)), 1, 127); }
}  // namespace

void PianoRoll::load(const music::MidiClip& c) {
  items_.clear();
  name_ = c.name; channel_ = c.channel;
  for (const Note& n : c.notes) items_.push_back({n, false});
  sortKeepSelection();
}

music::MidiClip PianoRoll::clip() const {
  music::MidiClip c; c.name = name_; c.channel = channel_;
  for (const Item& i : items_) c.notes.push_back(i.n);
  return c;
}

void PianoRoll::sortKeepSelection() {
  std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.n.start < b.n.start || (a.n.start == b.n.start && a.n.pitch < b.n.pitch); });
}

std::size_t PianoRoll::selectedCount() const { return static_cast<std::size_t>(std::count_if(items_.begin(), items_.end(), [](const Item& i) { return i.sel; })); }

int PianoRoll::addNote(int pitch, double start, double length, int velocity, double grid) {
  Note n; n.pitch = clampPitch(pitch); n.start = std::max(0.0, snap(start, grid)); n.length = std::max(grid > 0 ? grid : 0.0625, length); n.velocity = clampVel(velocity);
  for (const Item& i : items_) if (i.n.pitch == n.pitch && std::fabs(i.n.start - n.start) < 1e-9) return -1;
  selectNone();
  items_.push_back({n, true});
  sortKeepSelection();
  for (std::size_t k = 0; k < items_.size(); ++k) if (items_[k].sel) return static_cast<int>(k);
  return -1;
}

PianoRoll::Hit PianoRoll::hitTest(double beat, int pitch, double edge, std::size_t& index) const {
  for (std::size_t k = items_.size(); k-- > 0;) {   // later notes are drawn on top
    const Note& n = items_[k].n;
    if (n.pitch != pitch || beat < n.start || beat > n.start + n.length) continue;
    index = k;
    return n.length > 2 * edge && beat > n.start + n.length - edge ? Hit::ResizeEnd : Hit::Body;
  }
  return Hit::None;
}

void PianoRoll::selectNone() { for (Item& i : items_) i.sel = false; }
void PianoRoll::selectAll() { for (Item& i : items_) i.sel = true; }
void PianoRoll::select(std::size_t k, bool additive) {
  if (k >= items_.size()) return;
  if (!additive) selectNone();
  items_[k].sel = !additive || !items_[k].sel;
}
void PianoRoll::selectRect(double b0, double b1, int p0, int p1, bool additive) {
  if (b1 < b0) std::swap(b0, b1);
  if (p1 < p0) std::swap(p0, p1);
  if (!additive) selectNone();
  for (Item& i : items_) if (i.n.pitch >= p0 && i.n.pitch <= p1 && i.n.start < b1 && i.n.start + i.n.length > b0) i.sel = true;
}

void PianoRoll::moveSelected(double dBeats, int dPitch, double grid) {
  double first = -1;
  for (const Item& i : items_) if (i.sel) { first = i.n.start; break; }
  if (first < 0 && selectedCount() == 0) return;
  double shift = dBeats;
  if (grid > 0) shift = snap(first + dBeats, grid) - first;   // the first selected note lands on a grid line, the others keep their spacing
  double minStart = 1e18; int minP = 127, maxP = 0;
  for (const Item& i : items_) if (i.sel) { minStart = std::min(minStart, i.n.start); minP = std::min(minP, i.n.pitch); maxP = std::max(maxP, i.n.pitch); }
  if (minStart + shift < 0) shift = -minStart;
  dPitch = std::clamp(dPitch, -minP, 127 - maxP);
  for (Item& i : items_) if (i.sel) { i.n.start += shift; i.n.pitch += dPitch; }
  sortKeepSelection();
}

void PianoRoll::resizeSelected(double dBeats, double grid) {
  for (Item& i : items_) if (i.sel) {
    double end = i.n.start + i.n.length + dBeats;
    if (grid > 0) end = snap(end, grid);
    i.n.length = std::max(grid > 0 ? grid : 0.0625, end - i.n.start);
  }
}

void PianoRoll::deleteSelected() { items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Item& i) { return i.sel; }), items_.end()); }

void PianoRoll::quantizeSelected(double grid, double strength) {
  if (grid <= 0) return;
  strength = std::clamp(strength, 0.0, 1.0);
  for (Item& i : items_) if (i.sel) i.n.start += (snap(i.n.start, grid) - i.n.start) * strength;
  sortKeepSelection();
}

void PianoRoll::transposeSelected(int s) {
  int minP = 127, maxP = 0;
  for (const Item& i : items_) if (i.sel) { minP = std::min(minP, i.n.pitch); maxP = std::max(maxP, i.n.pitch); }
  s = std::clamp(s, -minP, 127 - maxP);
  for (Item& i : items_) if (i.sel) i.n.pitch += s;
  sortKeepSelection();
}

void PianoRoll::setVelocity(int v) { for (Item& i : items_) if (i.sel) i.n.velocity = clampVel(v); }
void PianoRoll::scaleVelocity(double f) { for (Item& i : items_) if (i.sel) i.n.velocity = clampVel(i.n.velocity * f); }

void PianoRoll::duplicateSelected() {
  double a = 1e18, b = 0;
  for (const Item& i : items_) if (i.sel) { a = std::min(a, i.n.start); b = std::max(b, i.n.start + i.n.length); }
  if (a > b) return;
  std::vector<Item> copies;
  for (Item& i : items_) if (i.sel) { Item c = i; c.n.start += b - a; copies.push_back(c); i.sel = false; }
  for (Item& c : copies) items_.push_back(c);
  sortKeepSelection();
}

void PianoRoll::copySelected() {
  clipboard_.clear();
  double a = 1e18;
  for (const Item& i : items_) if (i.sel) a = std::min(a, i.n.start);
  for (const Item& i : items_) if (i.sel) { Note n = i.n; n.start -= a; clipboard_.push_back(n); }
}

void PianoRoll::paste(double at) {
  if (clipboard_.empty()) return;
  selectNone();
  for (Note n : clipboard_) {
    n.start += std::max(0.0, at);
    const bool dup = std::any_of(items_.begin(), items_.end(), [&](const Item& i) { return i.n.pitch == n.pitch && std::fabs(i.n.start - n.start) < 1e-9; });
    if (!dup) items_.push_back({n, true});
  }
  sortKeepSelection();
}

void PianoRoll::legatoSelected() {
  for (std::size_t k = 0; k < items_.size(); ++k) {
    if (!items_[k].sel) continue;
    double next = -1;
    for (std::size_t j = k + 1; j < items_.size(); ++j) if (items_[j].n.start > items_[k].n.start + 1e-9) { next = items_[j].n.start; break; }
    if (next > 0) items_[k].n.length = std::max(0.0625, next - items_[k].n.start);
  }
}

double PianoRoll::endBeat() const {
  double e = 0;
  for (const Item& i : items_) e = std::max(e, i.n.start + i.n.length);
  return e;
}

}  // namespace wezt::edit
