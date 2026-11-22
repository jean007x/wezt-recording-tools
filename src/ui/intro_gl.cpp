#include "ui/intro_gl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "ui/intro.h"
#include "ui/intro_sound.h"
#include "ui/logo_sdf.h"

#if defined(_WIN32)
#define WEZT_GLAPI __stdcall
#else
#define WEZT_GLAPI
#endif

namespace wezt::ui {
namespace {
#include "ui/intro_gl_shaders.inc"

using GLenum = unsigned; using GLuint = unsigned; using GLint = int; using GLsizei = int; using GLfloat = float; using GLboolean = unsigned char;
using GLbitfield = unsigned; using GLchar = char; using GLsizeiptr = std::ptrdiff_t;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
                 GL_RGBA16F = 0x881A, GL_RGBA = 0x1908, GL_HALF_FLOAT = 0x140B, GL_FLOAT = 0x1406, GL_R32F = 0x822E, GL_RED = 0x1903,
                 GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_LINEAR = 0x2601, GL_TEXTURE_WRAP_S = 0x2802,
                 GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F, GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30,
                 GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_TRIANGLES = 0x0004, GL_TEXTURE0 = 0x84C0,
                 GL_COLOR_BUFFER_BIT = 0x4000, GL_BLEND = 0x0BE2, GL_DEPTH_TEST = 0x0B71, GL_SCISSOR_TEST = 0x0C11, GL_CULL_FACE = 0x0B44,
                 GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_VIEWPORT = 0x0BA2, GL_VERSION = 0x1F02, GL_UNPACK_ALIGNMENT = 0x0CF5,
                 GL_INFO_LOG_LENGTH = 0x8B84, GL_CURRENT_PROGRAM = 0x8B8D, GL_VERTEX_ARRAY_BINDING = 0x85B5, GL_TEXTURE_BINDING_2D = 0x8069,
                 GL_ACTIVE_TEXTURE = 0x84E0, GL_ONE = 1, GL_ZERO = 0;

struct Gl {
  void(WEZT_GLAPI* GenTextures)(GLsizei, GLuint*) = nullptr;
  void(WEZT_GLAPI* DeleteTextures)(GLsizei, const GLuint*) = nullptr;
  void(WEZT_GLAPI* BindTexture)(GLenum, GLuint) = nullptr;
  void(WEZT_GLAPI* TexParameteri)(GLenum, GLenum, GLint) = nullptr;
  void(WEZT_GLAPI* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
  void(WEZT_GLAPI* ActiveTexture)(GLenum) = nullptr;
  void(WEZT_GLAPI* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
  void(WEZT_GLAPI* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
  void(WEZT_GLAPI* BindFramebuffer)(GLenum, GLuint) = nullptr;
  void(WEZT_GLAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
  GLenum(WEZT_GLAPI* CheckFramebufferStatus)(GLenum) = nullptr;
  GLuint(WEZT_GLAPI* CreateShader)(GLenum) = nullptr;
  void(WEZT_GLAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
  void(WEZT_GLAPI* CompileShader)(GLuint) = nullptr;
  void(WEZT_GLAPI* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
  void(WEZT_GLAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void(WEZT_GLAPI* DeleteShader)(GLuint) = nullptr;
  GLuint(WEZT_GLAPI* CreateProgram)() = nullptr;
  void(WEZT_GLAPI* AttachShader)(GLuint, GLuint) = nullptr;
  void(WEZT_GLAPI* LinkProgram)(GLuint) = nullptr;
  void(WEZT_GLAPI* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
  void(WEZT_GLAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void(WEZT_GLAPI* DeleteProgram)(GLuint) = nullptr;
  void(WEZT_GLAPI* UseProgram)(GLuint) = nullptr;
  GLint(WEZT_GLAPI* GetUniformLocation)(GLuint, const GLchar*) = nullptr;
  void(WEZT_GLAPI* Uniform1i)(GLint, GLint) = nullptr;
  void(WEZT_GLAPI* Uniform1f)(GLint, GLfloat) = nullptr;
  void(WEZT_GLAPI* Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
  void(WEZT_GLAPI* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
  void(WEZT_GLAPI* UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
  void(WEZT_GLAPI* GenVertexArrays)(GLsizei, GLuint*) = nullptr;
  void(WEZT_GLAPI* DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
  void(WEZT_GLAPI* BindVertexArray)(GLuint) = nullptr;
  void(WEZT_GLAPI* DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
  void(WEZT_GLAPI* Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
  void(WEZT_GLAPI* Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
  void(WEZT_GLAPI* ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
  void(WEZT_GLAPI* Clear)(GLbitfield) = nullptr;
  void(WEZT_GLAPI* Enable)(GLenum) = nullptr;
  void(WEZT_GLAPI* Disable)(GLenum) = nullptr;
  void(WEZT_GLAPI* BlendFunc)(GLenum, GLenum) = nullptr;
  void(WEZT_GLAPI* GetIntegerv)(GLenum, GLint*) = nullptr;
  void(WEZT_GLAPI* PixelStorei)(GLenum, GLint) = nullptr;
  const unsigned char*(WEZT_GLAPI* GetString)(GLenum) = nullptr;

  bool load(IntroRenderer::Loader l) {
    bool ok = true;
    auto get = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(l(name));
      if (!fn) ok = false;
    };
    get(GenTextures, "glGenTextures"); get(DeleteTextures, "glDeleteTextures"); get(BindTexture, "glBindTexture");
    get(TexParameteri, "glTexParameteri"); get(TexImage2D, "glTexImage2D"); get(ActiveTexture, "glActiveTexture");
    get(GenFramebuffers, "glGenFramebuffers"); get(DeleteFramebuffers, "glDeleteFramebuffers"); get(BindFramebuffer, "glBindFramebuffer");
    get(FramebufferTexture2D, "glFramebufferTexture2D"); get(CheckFramebufferStatus, "glCheckFramebufferStatus");
    get(CreateShader, "glCreateShader"); get(ShaderSource, "glShaderSource"); get(CompileShader, "glCompileShader");
    get(GetShaderiv, "glGetShaderiv"); get(GetShaderInfoLog, "glGetShaderInfoLog"); get(DeleteShader, "glDeleteShader");
    get(CreateProgram, "glCreateProgram"); get(AttachShader, "glAttachShader"); get(LinkProgram, "glLinkProgram");
    get(GetProgramiv, "glGetProgramiv"); get(GetProgramInfoLog, "glGetProgramInfoLog"); get(DeleteProgram, "glDeleteProgram");
    get(UseProgram, "glUseProgram"); get(GetUniformLocation, "glGetUniformLocation"); get(Uniform1i, "glUniform1i");
    get(Uniform1f, "glUniform1f"); get(Uniform2f, "glUniform2f"); get(Uniform3f, "glUniform3f"); get(UniformMatrix3fv, "glUniformMatrix3fv");
    get(GenVertexArrays, "glGenVertexArrays"); get(DeleteVertexArrays, "glDeleteVertexArrays"); get(BindVertexArray, "glBindVertexArray");
    get(DrawArrays, "glDrawArrays"); get(Viewport, "glViewport"); get(Scissor, "glScissor"); get(ClearColor, "glClearColor");
    get(Clear, "glClear"); get(Enable, "glEnable"); get(Disable, "glDisable"); get(BlendFunc, "glBlendFunc");
    get(GetIntegerv, "glGetIntegerv"); get(PixelStorei, "glPixelStorei"); get(GetString, "glGetString");
    return ok;
  }
};

struct Target { GLuint tex = 0, fbo = 0; int w = 0, h = 0; };

float smooth01(double a, double b, double x) {
  const double u = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return static_cast<float>(u * u * (3.0 - 2.0 * u));
}

constexpr int kLevels = 6;
constexpr double kAspect = 2.39;
}  // namespace

struct IntroRenderer::Impl {
  Gl gl;
  bool ready = false, black = true;
  GLuint vao = 0, sdfTex = 0;
  GLuint progScene = 0, progDown = 0, progUp = 0, progStreak = 0, progComp = 0;
  Target scene, lv[kLevels], up[kLevels], streak;
  int bw = 0, bh = 0;
  bool adaptive = true;
  int maxW = 1920, curW = 0, framesSeen = 0, sinceChange = 0;
  double lastWall = 0, avgDt = 0.016;
  float halfX = 1.f, halfY = 0.35f, logoHalfY = 0.33f;

  GLuint compile(GLenum type, const std::string& src, std::string* err) {
    GLuint s = gl.CreateShader(type);
    const char* p = src.c_str();
    gl.ShaderSource(s, 1, &p, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048] = {};
      gl.GetShaderInfoLog(s, sizeof log - 1, nullptr, log);
      if (err) *err = std::string("shader: ") + log;
      gl.DeleteShader(s);
      return 0;
    }
    return s;
  }
  GLuint program(const std::string& header, const std::string& frag, std::string* err) {
    const GLuint vs = compile(GL_VERTEX_SHADER, header + kVertex, err);
    if (!vs) return 0;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, header + frag, err);
    if (!fs) { gl.DeleteShader(vs); return 0; }
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs); gl.AttachShader(p, fs);
    gl.LinkProgram(p);
    gl.DeleteShader(vs); gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[2048] = {};
      gl.GetProgramInfoLog(p, sizeof log - 1, nullptr, log);
      if (err) *err = std::string("link: ") + log;
      gl.DeleteProgram(p);
      return 0;
    }
    return p;
  }
  bool makeTarget(Target& t, int w, int h) {
    t.w = std::max(1, w); t.h = std::max(1, h);
    gl.GenTextures(1, &t.tex);
    gl.BindTexture(GL_TEXTURE_2D, t.tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, t.w, t.h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.GenFramebuffers(1, &t.fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }
  void freeTarget(Target& t) {
    if (t.fbo) gl.DeleteFramebuffers(1, &t.fbo);
    if (t.tex) gl.DeleteTextures(1, &t.tex);
    t = Target{};
  }
  void freeTargets() {
    freeTarget(scene); freeTarget(streak);
    for (auto& t : lv) freeTarget(t);
    for (auto& t : up) freeTarget(t);
    bw = bh = 0;
  }
  bool sizeTo(int w, int h) {
    if (w == bw && h == bh) return true;
    freeTargets();
    bool ok = makeTarget(scene, w, h);
    int lw = w, lh = h;
    for (int i = 0; i < kLevels && ok; ++i) {
      lw = std::max(1, lw / 2); lh = std::max(1, lh / 2);
      ok = makeTarget(lv[i], lw, lh) && makeTarget(up[i], lw, lh);
    }
    ok = ok && makeTarget(streak, std::max(1, w / 4), std::max(1, h / 4));
    if (!ok) { freeTargets(); return false; }
    bw = w; bh = h;
    return true;
  }
  void tri() { gl.DrawArrays(GL_TRIANGLES, 0, 3); }
  void bindTex(int unit, GLuint tex) { gl.ActiveTexture(GL_TEXTURE0 + unit); gl.BindTexture(GL_TEXTURE_2D, tex); }
  void setI(GLuint p, const char* n, int v) { gl.Uniform1i(gl.GetUniformLocation(p, n), v); }
  void setF(GLuint p, const char* n, float v) { gl.Uniform1f(gl.GetUniformLocation(p, n), v); }
  void set2(GLuint p, const char* n, float a, float b) { gl.Uniform2f(gl.GetUniformLocation(p, n), a, b); }
};

IntroRenderer::IntroRenderer() : impl_(std::make_unique<Impl>()) {}
IntroRenderer::~IntroRenderer() = default;
bool IntroRenderer::ready() const { return impl_->ready; }

void IntroRenderer::band(int fbW, int fbH, int* x, int* y, int* w, int* h) {
  int bw = fbW, bh = static_cast<int>(std::lround(fbW / kAspect));
  if (bh > fbH) { bh = fbH; bw = static_cast<int>(std::lround(fbH * kAspect)); }
  if (x) *x = (fbW - bw) / 2;
  if (y) *y = (fbH - bh) / 2;
  if (w) *w = bw;
  if (h) *h = bh;
}

bool IntroRenderer::init(Loader loader, bool black, std::string* error) {
  Impl& m = *impl_;
  if (m.ready) shutdown();
  if (!loader || !m.gl.load(loader)) { if (error) *error = "missing OpenGL entry points"; return false; }
  const char* ver = reinterpret_cast<const char*>(m.gl.GetString(GL_VERSION));
  int major = 0, minor = 0;
  if (ver) { const char* p = ver; while (*p && (*p < '0' || *p > '9')) ++p; major = std::atoi(p); while (*p && *p != '.') ++p; if (*p) minor = std::atoi(p + 1); }
  if (major < 3 || (major == 3 && minor < 2)) { if (error) *error = "OpenGL 3.2 or newer is needed"; return false; }
  m.black = black;
#if defined(__APPLE__)
  const std::string header = "#version 150\n";
#else
  const std::string header = "#version 330 core\n";
#endif
  m.progScene = m.program(header, std::string(kSceneA) + kSceneB, error);
  m.progDown = m.progScene ? m.program(header, kDown, error) : 0;
  m.progUp = m.progDown ? m.program(header, kUp, error) : 0;
  m.progStreak = m.progUp ? m.program(header, kStreak, error) : 0;
  m.progComp = m.progStreak ? m.program(header, kComposite, error) : 0;
  if (!m.progComp) { shutdown(); return false; }

  const LogoSdf sdf = makeLogoSdf();
  GLint oldTex = 0, oldAlign = 4;
  m.gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
  m.gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
  m.gl.GenTextures(1, &m.sdfTex);
  m.gl.BindTexture(GL_TEXTURE_2D, m.sdfTex);
  m.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R32F, sdf.w, sdf.h, 0, GL_RED, GL_FLOAT, sdf.d.data());
  m.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  m.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  m.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  m.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  m.gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTex));
  (void)oldAlign;
  m.gl.GenVertexArrays(1, &m.vao);
  m.halfX = sdf.halfX; m.halfY = sdf.halfY; m.logoHalfY = sdf.logoHalfY;
  m.ready = true;
  return true;
}

void IntroRenderer::shutdown() {
  Impl& m = *impl_;
  if (m.gl.DeleteTextures) {
    m.freeTargets();
    if (m.sdfTex) m.gl.DeleteTextures(1, &m.sdfTex);
    if (m.vao) m.gl.DeleteVertexArrays(1, &m.vao);
    for (GLuint* p : {&m.progScene, &m.progDown, &m.progUp, &m.progStreak, &m.progComp}) if (*p) m.gl.DeleteProgram(*p);
  }
  m.sdfTex = m.vao = m.progScene = m.progDown = m.progUp = m.progStreak = m.progComp = 0;
  m.ready = false;
}

void IntroRenderer::setAdaptive(bool on) { impl_->adaptive = on; }
int IntroRenderer::internalWidth() const { return impl_->curW; }

void IntroRenderer::render(double t, int fbW, int fbH) {
  Impl& m = *impl_;
  if (!m.ready || fbW < 16 || fbH < 16) return;
  Gl& gl = m.gl;
  GLint oldFbo = 0, oldVp[4] = {}, oldProg = 0, oldVao = 0, oldTex = 0, oldActive = 0;
  gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo); gl.GetIntegerv(GL_VIEWPORT, oldVp); gl.GetIntegerv(GL_CURRENT_PROGRAM, &oldProg);
  gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao); gl.GetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
  gl.ActiveTexture(GL_TEXTURE0); gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);

  int bx, by, bw, bh;
  band(fbW, fbH, &bx, &by, &bw, &bh);
  if (m.adaptive) {   // lower the resolution if the card cannot keep up
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m.lastWall > 0 && wall - m.lastWall < 1.0) {   // a longer gap is a stall (window drag, debugger), not the card
      m.avgDt += (std::min(wall - m.lastWall, 0.2) - m.avgDt) * 0.25;
      ++m.framesSeen; ++m.sinceChange;
      if (m.framesSeen > 8 && m.sinceChange > 10 && m.avgDt > 0.025 && m.maxW > 640) {
        m.maxW = std::max(640, static_cast<int>(m.maxW * 0.75));
        m.sinceChange = 0;
      }
    }
    m.lastWall = wall;
  }
  int iw = bw, ih = bh;
  if (iw > m.maxW) { ih = static_cast<int>(std::lround(ih * static_cast<double>(m.maxW) / iw)); iw = m.maxW; }
  m.curW = iw;
  if (!m.sizeTo(iw, ih)) {
    m.ready = false;
  } else {
    gl.Disable(GL_BLEND); gl.Disable(GL_DEPTH_TEST); gl.Disable(GL_SCISSOR_TEST); gl.Disable(GL_CULL_FACE);
    gl.BindVertexArray(m.vao);

    const IntroPose pose = introPose(t);
    const float scale = std::max(0.025f, pose.scale);
    const float dist = 5.4f / scale;
    const float cy = std::cos(pose.yaw), sy = std::sin(pose.yaw), cx = std::cos(pose.tilt), sx = std::sin(pose.tilt);
    // object -> world = Ry(yaw) * Rx(tilt), column-major; world -> object is its transpose
    const float R[9] = {cy, 0.f, -sy,   sy * sx, cx, cy * sx,   sy * cx, -sx, cy * cx};
    float Rt[9];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) Rt[i * 3 + j] = R[j * 3 + i];
    const float tf = static_cast<float>(t);
    float sweep = -100.f;
    if (t >= 2.7 && t < 3.7) sweep = -1.6f + (tf - 2.7f) * 3.4f;
    else if (t >= 3.9 && t < 4.8) sweep = -1.6f + (tf - 3.9f) * 3.9f;
    const float impact = t >= kIntroGrowEnd ? std::exp(-(tf - static_cast<float>(kIntroGrowEnd)) * 2.6f) : 0.f;
    const float fade = smooth01(0.0, 0.5, t) * (1.f - smooth01(5.6, 6.0, t));
    const float beam = smooth01(0.2, 2.6, t) * (1.f - smooth01(5.4, 6.0, t));

    // 1. the scene, in floating point so highlights can go far past white
    gl.BindFramebuffer(GL_FRAMEBUFFER, m.scene.fbo);
    gl.Viewport(0, 0, m.scene.w, m.scene.h);
    gl.UseProgram(m.progScene);
    m.bindTex(0, m.sdfTex);
    m.setI(m.progScene, "uSdf", 0);
    m.set2(m.progScene, "uRes", static_cast<float>(m.scene.w), static_cast<float>(m.scene.h));
    m.set2(m.progScene, "uHalf", m.halfX, m.halfY);
    m.setF(m.progScene, "uLogoHalfY", m.logoHalfY);
    m.setF(m.progScene, "uTime", tf);
    m.setF(m.progScene, "uDist", dist);
    gl.UniformMatrix3fv(gl.GetUniformLocation(m.progScene, "uRotOW"), 1, 0, R);
    gl.UniformMatrix3fv(gl.GetUniformLocation(m.progScene, "uRotWO"), 1, 0, Rt);
    m.setF(m.progScene, "uBlack", m.black ? 1.f : 0.f);
    m.setF(m.progScene, "uSweep", sweep);
    m.setF(m.progScene, "uImpact", impact);
    m.setF(m.progScene, "uFade", fade);
    m.setF(m.progScene, "uBeam", beam);
    gl.Uniform3f(gl.GetUniformLocation(m.progScene, "uDrift"), 0.10f * std::sin(tf * 0.45f), 0.04f * std::sin(tf * 0.3f + 1.f), 0.f);
    m.tri();

    // 2. bloom: down the chain, then back up adding each level
    gl.UseProgram(m.progDown);
    m.setI(m.progDown, "uSrc", 0);
    GLuint src = m.scene.tex; int sw = m.scene.w, sh = m.scene.h;
    for (int i = 0; i < kLevels; ++i) {
      gl.BindFramebuffer(GL_FRAMEBUFFER, m.lv[i].fbo);
      gl.Viewport(0, 0, m.lv[i].w, m.lv[i].h);
      m.bindTex(0, src);
      m.set2(m.progDown, "uTexel", 1.f / sw, 1.f / sh);
      m.setF(m.progDown, "uFirst", i == 0 ? 1.f : 0.f);
      m.tri();
      src = m.lv[i].tex; sw = m.lv[i].w; sh = m.lv[i].h;
    }
    gl.UseProgram(m.progUp);
    m.setI(m.progUp, "uSrc", 0);
    GLuint carry = m.lv[kLevels - 1].tex; int cw = m.lv[kLevels - 1].w, ch = m.lv[kLevels - 1].h;
    for (int i = kLevels - 2; i >= 0; --i) {
      gl.BindFramebuffer(GL_FRAMEBUFFER, m.up[i].fbo);
      gl.Viewport(0, 0, m.up[i].w, m.up[i].h);
      m.bindTex(0, carry);
      m.set2(m.progUp, "uTexel", 1.f / cw, 1.f / ch);
      m.setF(m.progUp, "uGain", 1.f);
      m.tri();
      // add this level's own detail by drawing it additively on top
      gl.Enable(GL_BLEND); gl.BlendFunc(GL_ONE, GL_ONE);
      gl.UseProgram(m.progUp);
      m.bindTex(0, m.lv[i].tex);
      m.set2(m.progUp, "uTexel", 1.f / m.lv[i].w, 1.f / m.lv[i].h);
      m.setF(m.progUp, "uGain", 0.9f);
      m.tri();
      gl.Disable(GL_BLEND);
      carry = m.up[i].tex; cw = m.up[i].w; ch = m.up[i].h;
    }
    // 3. the horizontal streak, from a small copy of the bloom's second level
    gl.BindFramebuffer(GL_FRAMEBUFFER, m.streak.fbo);
    gl.Viewport(0, 0, m.streak.w, m.streak.h);
    gl.UseProgram(m.progStreak);
    m.bindTex(0, m.lv[1].tex);
    m.setI(m.progStreak, "uSrc", 0);
    m.set2(m.progStreak, "uTexel", 1.f / m.streak.w, 1.f / m.streak.h);
    m.tri();

    // 4. the picture, into the wide-screen band of the real framebuffer
    gl.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(oldFbo));
    gl.Viewport(0, 0, fbW, fbH);
    gl.ClearColor(0.f, 0.f, 0.f, 1.f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Viewport(bx, by, bw, bh);
    gl.UseProgram(m.progComp);
    m.bindTex(0, m.scene.tex); m.bindTex(1, carry); m.bindTex(2, m.streak.tex);
    m.setI(m.progComp, "uScene", 0); m.setI(m.progComp, "uBloom", 1); m.setI(m.progComp, "uStreak", 2);
    m.set2(m.progComp, "uOut", static_cast<float>(bw), static_cast<float>(bh));
    m.setF(m.progComp, "uTime", tf);
    m.setF(m.progComp, "uImpact", impact);
    m.setF(m.progComp, "uMotes", beam);
    m.setF(m.progComp, "uFlare", smooth01(5.3, 5.85, t) * (1.f - smooth01(5.85, 6.0, t)));
    m.setF(m.progComp, "uExposure", 1.0f);
    m.tri();
  }
  gl.UseProgram(static_cast<GLuint>(oldProg));
  gl.BindVertexArray(static_cast<GLuint>(oldVao));
  gl.ActiveTexture(GL_TEXTURE0 + 2); gl.BindTexture(GL_TEXTURE_2D, 0);
  gl.ActiveTexture(GL_TEXTURE0 + 1); gl.BindTexture(GL_TEXTURE_2D, 0);
  gl.ActiveTexture(GL_TEXTURE0); gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTex));
  gl.ActiveTexture(static_cast<GLenum>(oldActive));
  gl.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(oldFbo));
  gl.Viewport(oldVp[0], oldVp[1], oldVp[2], oldVp[3]);
}

}  // namespace wezt::ui
