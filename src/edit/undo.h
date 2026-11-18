#pragma once
// Multi-level undo/redo. Each step is a pair of functions that change the same state in opposite
// directions. The stack owns no data: the lambdas capture what they need (usually a copy of the
// clips or values before and after), so any part of the app can record a step.
#include <functional>
#include <string>
#include <vector>

namespace wezt::edit {

struct UndoStep {
  std::string label;
  std::function<void()> undo, redo;
  std::string mergeKey;  // consecutive steps with the same non-empty key collapse (e.g. dragging a fader)
};

class UndoStack {
 public:
  explicit UndoStack(std::size_t depth = 512) : depth_(depth ? depth : 1) {}

  // Records a step that has ALREADY been applied. Clears the redo list.
  void record(UndoStep s) {
    redo_.clear();
    if (!s.mergeKey.empty() && !undo_.empty() && undo_.back().mergeKey == s.mergeKey) {
      undo_.back().redo = std::move(s.redo);  // keep the first undo, take the latest redo
      return;
    }
    undo_.push_back(std::move(s));
    if (undo_.size() > depth_) undo_.erase(undo_.begin());
  }
  // Applies `redo` now, then records it.
  void perform(UndoStep s) { if (s.redo) s.redo(); record(std::move(s)); }

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
  std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }

  bool undo() {
    if (undo_.empty()) return false;
    UndoStep s = std::move(undo_.back());
    undo_.pop_back();
    if (s.undo) s.undo();
    redo_.push_back(std::move(s));
    return true;
  }
  bool redo() {
    if (redo_.empty()) return false;
    UndoStep s = std::move(redo_.back());
    redo_.pop_back();
    if (s.redo) s.redo();
    undo_.push_back(std::move(s));
    return true;
  }
  void clear() { undo_.clear(); redo_.clear(); }
  // The Undo History list, oldest first.
  std::vector<std::string> history() const {
    std::vector<std::string> h;
    for (auto& s : undo_) h.push_back(s.label);
    return h;
  }

 private:
  std::size_t depth_;
  std::vector<UndoStep> undo_, redo_;
};

}  // namespace wezt::edit
