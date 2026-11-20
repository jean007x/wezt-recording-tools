// Dev tool (Linux): renders frames of the GL intro offscreen through EGL (Mesa software rendering works) and writes PPM files.
// usage: wezt_intro_gl_snapshot out_prefix black|gold t1 t2 ...
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>
#include "ui/intro_gl.h"
typedef void* EGLDisplay; typedef void* EGLConfig; typedef void* EGLContext; typedef void* EGLSurface; typedef int EGLint; typedef unsigned EGLBoolean; typedef unsigned EGLenum;
int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage\n"); return 2; }
  void* lib = dlopen("libEGL.so.1", RTLD_NOW); if (!lib) { std::fprintf(stderr, "no EGL\n"); return 1; }
  #define F(r, n, a) auto n = reinterpret_cast<r(*)a>(dlsym(lib, #n))
  F(void*, eglGetProcAddress, (const char*)); F(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void*, const EGLint*));
  F(EGLBoolean, eglInitialize, (EGLDisplay, EGLint*, EGLint*)); F(EGLBoolean, eglBindAPI, (EGLenum));
  F(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*));
  F(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint*));
  F(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
  const EGLint cfg[] = {0x3033, 0x1, 0x3040, 0x8, 0x3038};   // surface type pbuffer, renderable GL
  EGLDisplay d = eglGetPlatformDisplay(0x31DD, nullptr, nullptr);   // EGL_PLATFORM_SURFACELESS_MESA
  EGLint a, b; if (!d || !eglInitialize(d, &a, &b)) { std::fprintf(stderr, "egl init\n"); return 1; }
  eglBindAPI(0x30A2);
  EGLConfig c; EGLint n = 0; const EGLint cfg2[] = {0x3038};
  if (!eglChooseConfig(d, cfg2, &c, 1, &n) || !n) c = nullptr;   // surfaceless: a context without a config (EGL_KHR_no_config_context)
  const EGLint ca[] = {0x3098, 3, 0x30FB, 3, 0x3038};
  (void)cfg;
  EGLContext ctx = eglCreateContext(d, c, nullptr, ca);
  if (!ctx || !eglMakeCurrent(d, nullptr, nullptr, ctx)) { std::fprintf(stderr, "ctx\n"); return 1; }
  auto ld = [](const char* nme) -> void* { static void* l = dlopen("libEGL.so.1", RTLD_NOW); auto g = reinterpret_cast<void*(*)(const char*)>(dlsym(l, "eglGetProcAddress")); return g(nme); };
  wezt::ui::IntroRenderer r; std::string err;
  if (!r.init(ld, std::strcmp(argv[2], "black") == 0, &err)) { std::fprintf(stderr, "init: %s\n", err.c_str()); return 1; }
  const int W = argc > 0 && std::getenv("W") ? std::atoi(std::getenv("W")) : 1280, H = std::getenv("H") ? std::atoi(std::getenv("H")) : 720;
  typedef unsigned GLuint; typedef int GLint;
  auto genT = reinterpret_cast<void(*)(int, GLuint*)>(ld("glGenTextures")); auto bindT = reinterpret_cast<void(*)(unsigned, GLuint)>(ld("glBindTexture"));
  auto img = reinterpret_cast<void(*)(unsigned, GLint, GLint, int, int, GLint, unsigned, unsigned, const void*)>(ld("glTexImage2D"));
  auto genF = reinterpret_cast<void(*)(int, GLuint*)>(ld("glGenFramebuffers")); auto bindF = reinterpret_cast<void(*)(unsigned, GLuint)>(ld("glBindFramebuffer"));
  auto fbt = reinterpret_cast<void(*)(unsigned, unsigned, unsigned, GLuint, GLint)>(ld("glFramebufferTexture2D"));
  auto rd = reinterpret_cast<void(*)(int, int, int, int, unsigned, unsigned, void*)>(ld("glReadPixels"));
  GLuint tex, fbo; genT(1, &tex); bindT(0x0DE1, tex); img(0x0DE1, 0, 0x8058, W, H, 0, 0x1908, 0x1401, nullptr);
  genF(1, &fbo); bindF(0x8D40, fbo); fbt(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
  for (int i = 3; i < argc; ++i) {
    double t = std::atof(argv[i]);
    r.render(t, W, H);
    std::vector<unsigned char> px(static_cast<size_t>(W) * H * 4);
    rd(0, 0, W, H, 0x1908, 0x1401, px.data());
    char name[512]; std::snprintf(name, sizeof name, "%s_%s.ppm", argv[1], argv[i]);
    FILE* f = std::fopen(name, "wb"); std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; --y) for (int x = 0; x < W; ++x) std::fwrite(&px[(static_cast<size_t>(y) * W + x) * 4], 1, 3, f);
    std::fclose(f);
  }
  return 0;
}
