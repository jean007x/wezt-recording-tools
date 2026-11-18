#pragma once
// The editing model behind the piano roll: a list of notes with a selection, and the operations a
// person does with the mouse and keyboard (add, move, resize, delete, quantize, transpose,
// copy/paste, velocity). It has no drawing code, so it can be tested without a window.
#include <vector>

#include "music/midi.h"

namespace wezt::edit {

class PianoRoll {
 public:
  PianoRoll() = default;
  explicit PianoRoll(const music::MidiClip& c) { load(c); }

  void load(const music::MidiClip& c);
  music::MidiClip clip() const;                 // the notes, sorted by start then pitch
  std::size_t size() const { return items_.size(); }
  const music::Note& note(std::size_t i) const { return items_[i].n; }
  bool selected(std::size_t i) const { return items_[i].sel; }
  std::size_t selectedCount() const;

  // grid in beats (0 = off). All times are beats, all pitches MIDI numbers.
  int addNote(int pitch, double start, double length, int velocity, double grid = 0);   // returns its index, -1 if it duplicates a note
  enum class Hit { None, Body, ResizeEnd };
  // Which note is under (beat, pitch)? `edgeBeats` is the width of the resize zone at the right end.
  Hit hitTest(double beat, int pitch, double edgeBeats, std::size_t& index) const;

  void selectNone();
  void selectAll();
  void select(std::size_t i, bool additive);
  void selectRect(double beat0, double beat1, int pitch0, int pitch1, bool additive);   // any note overlapping the rectangle

  void moveSelected(double dBeats, int dPitch, double grid = 0);   // with a grid, the first selected note lands on a grid line
  void resizeSelected(double dBeats, double grid = 0);             // changes the length of each selected note
  void deleteSelected();
  void quantizeSelected(double grid, double strength = 1.0);
  void transposeSelected(int semitones);
  void setVelocity(int velocity);                                  // selected notes
  void scaleVelocity(double factor);
  void duplicateSelected();                                        // copies placed right after the selection; the copies become selected
  void copySelected();
  void paste(double atBeat);                                       // the clipboard, first note at `atBeat`; pasted notes become selected
  bool hasClipboard() const { return !clipboard_.empty(); }
  void legatoSelected();

  double endBeat() const;

 private:
  struct Item { music::Note n; bool sel = false; };
  void sortKeepSelection();
  std::vector<Item> items_;
  std::vector<music::Note> clipboard_;
  std::string name_;
  int channel_ = 0;
};

}  // namespace wezt::edit
