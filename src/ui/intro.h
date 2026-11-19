#pragma once
// The start-up intro: the logo (gold) grows from a speck while spinning, lands, then spins back down to a speck, and the
// main window opens on a flash. Timeline in intro_sound.h. Drawing is a pure function of the time (paint), so it can be
// rendered headless; draw() adds the clock and the "any key skips" input.
#include <imgui.h>

#include "ui/intro_sound.h"

namespace wezt::ui {

class Intro {
 public:
  void setTextures(ImTextureID gold, ImTextureID white, float aspect) { gold_ = gold; white_ = white; aspect_ = aspect; }
  void begin() { t_ = 0; active_ = true; skipped_ = false; }
  bool active() const { return active_; }                 // until the flash is over
  bool running() const { return active_ && t_ < kIntroOpen; }    // full-screen part: the main window is not drawn yet
  double time() const { return t_; }
  void skip();                                            // jump to the opening; the owner should silence the sound
  bool takeSkipped() { const bool s = skipped_; skipped_ = false; return s; }
  // Call once per frame between NewFrame() and Render(): advances the clock, reads input, paints.
  void draw();
  // Paints the intro as it looks `t` seconds in (no clock, no input).
  void paint(double t) const;

 private:
  ImTextureID gold_{}, white_{};
  float aspect_ = 640.f / 216.f;
  double t_ = 0;
  bool active_ = false, skipped_ = false;
};

}  // namespace wezt::ui
