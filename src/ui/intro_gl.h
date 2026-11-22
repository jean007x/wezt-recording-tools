#pragma once
// The cinematic version of the start-up intro, drawn on the GPU with shaders.
//
// The logo is rendered as a real 3D solid: its outline (a signed distance field, logo_sdf.h) extruded into a slab with rounded edges and
// ray-marched per pixel. It is polished metal lit by a procedural photo studio (soft boxes, strip lights, a warm back light), so the
// reflections slide across it as it turns. Around it: a black polished floor that reflects it, beams of light through haze with drifting
// dust, then bloom, anamorphic light streaks, god rays, lens fringing, grain and a wide-screen frame. Timeline: intro_sound.h.
//
// It needs OpenGL 3.3 (3.2 on macOS) with float render targets. init() returns false when that is not available or a shader does not
// compile, and the caller then uses the plain 2D intro in intro.h instead. GL functions are looked up through `loader` (for GLFW,
// glfwGetProcAddress), so this file needs no GL headers and builds the same on every platform.
#include <memory>
#include <string>

namespace wezt::ui {

class IntroRenderer {
 public:
  using Loader = void* (*)(const char*);
  IntroRenderer();
  ~IntroRenderer();
  IntroRenderer(const IntroRenderer&) = delete;
  IntroRenderer& operator=(const IntroRenderer&) = delete;

  // Needs a current GL context. `black` = satin-black lacquer face with gold edges; false = all polished gold.
  bool init(Loader loader, bool black, std::string* error);
  bool ready() const;
  void shutdown();   // needs the GL context to still be current

  // Draws the intro as it looks `t` seconds in, into the framebuffer that is bound now (clears all of it to black first and keeps the
  // picture in a 2.39:1 band, centred). fbW/fbH = the framebuffer's size in pixels.
  void render(double t, int fbW, int fbH);

  // Slow graphics card? By default the renderer watches the time between frames and, if the intro runs below about 40 frames per
  // second, lowers the internal resolution step by step (never below 640 px wide); the picture is simply a little softer.
  // Switch it off for reproducible pictures (the snapshot tool does).
  void setAdaptive(bool on);
  int internalWidth() const;   // the width the picture is drawn at now (0 before the first frame)

  // The wide-screen band (x, y, w, h in pixels) render() draws into for a framebuffer of this size.
  static void band(int fbW, int fbH, int* x, int* y, int* w, int* h);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wezt::ui
