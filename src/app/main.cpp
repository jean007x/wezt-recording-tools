// Wezt Recording Tools: desktop application (GLFW window, OpenGL 3 drawing, Dear ImGui).
// The loop sleeps until an event arrives unless something is moving, so an idle session costs
// almost no CPU.
#include <cstdio>
#include <cstring>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "ui/app.h"

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
    wezt::ui::App app;
    std::string msg;
    app.startAudio(&msg);
    app.setStatus(msg);
    if (demo) app.loadDemo();
    while (!glfwWindowShouldClose(window) && !app.wantsQuit()) {
      if (app.animating()) glfwPollEvents(); else glfwWaitEventsTimeout(0.25);
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      app.frame();
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
