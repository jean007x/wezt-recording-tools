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
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  const char* glsl = "#version 130";
#endif
  GLFWwindow* window = glfwCreateWindow(1500, 920, "Wezt Recording Tools", nullptr, nullptr);
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
    const auto goldImg = wezt::ui::makeGoldLogo(), whiteImg = wezt::ui::makeWhiteLogo();
    const GLuint goldTex = uploadTexture(goldImg), whiteTex = uploadTexture(whiteImg);
    wezt::ui::Intro intro;
    wezt::ui::IntroPlayer introSound;
    auto startIntro = [&] {
      intro.setTextures(asTexture(goldTex), asTexture(whiteTex), static_cast<float>(goldImg.w) / static_cast<float>(goldImg.h));
      std::string m;
      introSound.start(wezt::loadPrefs().introSoundPath, &m);   // re-read: a replay picks up a sound chosen meanwhile
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
