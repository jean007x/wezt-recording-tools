// Wezt Recording Tools: desktop application (GLFW window, OpenGL 3 drawing, Dear ImGui).
// The loop sleeps until an event arrives unless something is moving, so an idle session costs
// almost no CPU.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "ui/app.h"
#include "ui/intro.h"
#include "ui/intro_gl.h"
#include "ui/intro_sound.h"
#include "ui/logo.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {
// Uploads a picture with its whole mip chain (only OpenGL 1.1 calls, so it works with the stock Windows headers too).
GLuint uploadTexture(const wezt::ui::LogoImage& base) {
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  int level = 0;
  for (const auto& m : wezt::ui::mipChain(base)) glTexImage2D(GL_TEXTURE_2D, level++, GL_RGBA, m.w, m.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, m.rgba.data());
  return id;
}
ImTextureID asTexture(GLuint id) { return (ImTextureID)(intptr_t)id; }
}  // namespace

int main(int argc, char** argv) {
  bool demo = false;
  for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--demo")) demo = true;
  if (!glfwInit()) { std::fprintf(stderr, "Could not start GLFW\n"); return 1; }
#if defined(__APPLE__)
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  const char* glsl = "#version 150";
#else
  // Ask for a modern core context first (the cinematic intro needs OpenGL 3.3); an older driver gets the 3.0 one the rest of the app is happy with.
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  const char* glsl = "#version 150";
#endif
  GLFWwindow* window = glfwCreateWindow(1500, 920, "Wezt Recording Tools", nullptr, nullptr);
#if !defined(__APPLE__)
  if (!window) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glsl = "#version 130";
    window = glfwCreateWindow(1500, 920, "Wezt Recording Tools", nullptr, nullptr);
  }
#endif
  if (!window) { std::fprintf(stderr, "Could not open a window\n"); glfwTerminate(); return 1; }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  float sx = 1.f, sy = 1.f;
  glfwGetWindowContentScale(window, &sx, &sy);
  wezt::ui::App::loadFonts(sx);
  wezt::ui::App::applyTheme();
  ImGui::GetStyle().ScaleAllSizes(sx);
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl);
  {
    // The opening intro (File > Settings turns it off or changes its sound). The studio is created when it opens.
    const wezt::Prefs prefs = wezt::loadPrefs();
    // The logo pictures are built (and uploaded) for the style chosen in Settings; a replay after a change builds them again.
    GLuint goldTex = 0, whiteTex = 0; bool texBlack = true; float texAspect = 1.f;
    auto loadLogo = [&](bool black) {
      if (goldTex && texBlack == black) return;
      if (goldTex) { glDeleteTextures(1, &goldTex); glDeleteTextures(1, &whiteTex); }
      const auto face = wezt::ui::makeLogo(black ? wezt::ui::LogoStyle::Black : wezt::ui::LogoStyle::Gold), white = wezt::ui::makeWhiteLogo();
      goldTex = uploadTexture(face); whiteTex = uploadTexture(white);
      texBlack = black; texAspect = static_cast<float>(face.w) / static_cast<float>(face.h);
    };
    wezt::ui::Intro intro;
    // The cinematic (GPU) intro. If this machine's OpenGL cannot do it, the plain 2D intro runs instead.
    wezt::ui::IntroRenderer introGl;
    bool introGlBlack = true;
    auto prepareGl = [&](bool black) {
      if (introGl.ready() && introGlBlack == black) return;
      std::string e;
      if (introGl.ready()) introGl.shutdown();
      introGl.init([](const char* n) -> void* { return reinterpret_cast<void*>(glfwGetProcAddress(n)); }, black, &e);
      if (!introGl.ready()) std::fprintf(stderr, "Cinematic intro unavailable (%s); using the simple one.\n", e.c_str());
      introGlBlack = black;
      intro.setGlMode(introGl.ready());
    };
    wezt::ui::IntroPlayer introSound;
    auto startIntro = [&] {
      const wezt::Prefs now = wezt::loadPrefs();   // re-read: a replay picks up a sound or logo style chosen meanwhile
      prepareGl(now.introLogoBlack);
      if (!introGl.ready()) loadLogo(now.introLogoBlack);
      if (!introGl.ready()) intro.setTextures(asTexture(goldTex), asTexture(whiteTex), texAspect, now.introLogoBlack);
      std::string m;
      introSound.start(now.introSoundPath, &m);
      intro.begin();
    };
    if (prefs.introEnabled) startIntro();

    std::unique_ptr<wezt::ui::App> appPtr;
    auto openStudio = [&] {
      appPtr = std::make_unique<wezt::ui::App>();
      std::string msg;
      appPtr->startAudio(&msg);
      appPtr->setStatus(msg);
      if (demo) appPtr->loadDemo();
    };
    if (!prefs.introEnabled) openStudio();
    while (!glfwWindowShouldClose(window) && !(appPtr && appPtr->wantsQuit())) {
      if (intro.active() || (appPtr && appPtr->animating())) glfwPollEvents(); else glfwWaitEventsTimeout(0.25);
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      if (!appPtr && !intro.running()) openStudio();   // the logo is back to a speck: the studio opens under the flash
      if (appPtr && appPtr->takeIntroReplay()) startIntro();
      if (appPtr && !intro.running()) appPtr->frame();
      intro.draw();
      if (intro.takeSkipped()) introSound.stop(0.3);
      ImGui::Render();
      int w = 0, h = 0;
      glfwGetFramebufferSize(window, &w, &h);
      glViewport(0, 0, w, h);
      glClearColor(0.09f, 0.10f, 0.12f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      if (intro.running() && introGl.ready()) introGl.render(intro.time(), w, h);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      glfwSwapBuffers(window);
    }
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
