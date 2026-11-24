// Drawing helpers for the interface: gradients, faders, LED meters, transport icons.
// All original artwork drawn with ImDrawList primitives; nothing here is taken from another product.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

namespace wezt::ui::style {

inline ImU32 rgba(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }

// Track colours (ABGR as ImGui stores them).
inline ImU32 trackColor(int id) {
  static const ImU32 c[] = {0xFFD9A441, 0xFF4DA8E8, 0xFF6CCB5F, 0xFF4F8CF0, 0xFFD066C4, 0xFFC8C743, 0xFF7B6CE8, 0xFF3FC7E0};
  return c[((id % 8) + 8) % 8];
}
// Multiply the rgb of a colour (k > 1 brightens, towards white when it saturates).
inline ImU32 shade(ImU32 c, float k, int alpha = -1) {
  const auto ch = [&](int shift) { return static_cast<int>(std::clamp(static_cast<float>((c >> shift) & 0xFF) * k, 0.f, 255.f)); };
  const int a = alpha >= 0 ? alpha : static_cast<int>((c >> 24) & 0xFF);
  return (static_cast<ImU32>(a) << 24) | (static_cast<ImU32>(ch(16)) << 16) | (static_cast<ImU32>(ch(8)) << 8) | static_cast<ImU32>(ch(0));
}
inline void vgrad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) { dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom); }

// A raised, softly lit button face.
inline void face(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 base, bool pressed, bool hovered, float round = 3.f) {
  const float k = pressed ? 0.75f : hovered ? 1.15f : 1.f;
  dl->AddRectFilled(a, b, shade(base, 0.62f * k), round);
  dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, (a.y + b.y) * 0.5f), shade(base, 1.25f * k, 70), round, ImDrawFlags_RoundCornersTop);
  dl->AddRect(a, b, rgba(0.f, 0.f, 0.f, 0.7f), round);
  dl->AddLine(ImVec2(a.x + round, a.y + 1), ImVec2(b.x - round, a.y + 1), rgba(1.f, 1.f, 1.f, pressed ? 0.04f : 0.14f));
}

enum class Icon { ToStart, Rewind, Play, Stop, Forward, Record, Loop };

// Transport button with a drawn icon. `lit` shows the on-state colour with a glow.
inline bool iconButton(const char* id, Icon icon, bool lit, ImU32 litColor, ImVec2 size) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, size);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q(p.x + size.x, p.y + size.y);
  face(dl, p, q, lit ? litColor : 0xFF4A4C52, act, hov);
  if (lit) dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(q.x + 1, q.y + 1), shade(litColor, 1.2f, 90), 4.f, 0, 2.f);
  const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
  const float h = std::min(size.x, size.y) * 0.27f;
  const ImU32 ink = lit ? 0xFFFFFFFF : 0xFFD8DADF;
  auto tri = [&](float cx, float dir) { dl->AddTriangleFilled(ImVec2(cx + dir * h, c.y), ImVec2(cx - dir * h * 0.8f, c.y - h), ImVec2(cx - dir * h * 0.8f, c.y + h), ink); };
  switch (icon) {
    case Icon::ToStart: dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x - h + 2.5f, c.y + h), ink); tri(c.x + h * 0.25f, -1.f); break;
    case Icon::Rewind: tri(c.x - h * 0.45f, -1.f); tri(c.x + h * 0.55f, -1.f); break;
    case Icon::Forward: tri(c.x - h * 0.45f, 1.f); tri(c.x + h * 0.55f, 1.f); break;
    case Icon::Play: tri(c.x + h * 0.1f, 1.f); break;
    case Icon::Stop: dl->AddRectFilled(ImVec2(c.x - h * 0.9f, c.y - h * 0.9f), ImVec2(c.x + h * 0.9f, c.y + h * 0.9f), ink, 1.5f); break;
    case Icon::Record: dl->AddCircleFilled(c, h * 0.95f, lit ? 0xFFFFFFFF : 0xFF3A3AE8, 20); break;
    case Icon::Loop:
      dl->PathArcTo(c, h * 0.95f, 0.6f, 6.2f, 20);
      dl->PathStroke(ink, 0, 2.2f);
      dl->AddTriangleFilled(ImVec2(c.x + h * 1.2f, c.y - h * 0.2f), ImVec2(c.x + h * 0.45f, c.y - h * 0.35f), ImVec2(c.x + h * 0.95f, c.y + h * 0.65f), ink);
      break;
  }
  return pressed;
}

// Segmented text button used for modes, tools and window switches.
inline bool chip(const char* label, bool on, ImU32 onColor, ImVec2 size = ImVec2(0, 0), ImU32 onText = 0xFFFFFFFF) {
  const char* labelEnd = std::strstr(label, "##");   // text after "##" is only an id
  const ImVec2 ts = ImGui::CalcTextSize(label, labelEnd);
  if (size.x <= 0) size.x = ts.x + 18.f;
  if (size.y <= 0) size.y = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(label, size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  face(dl, p, ImVec2(p.x + size.x, p.y + size.y), on ? onColor : 0xFF3C3E44, ImGui::IsItemActive(), ImGui::IsItemHovered());
  dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), on ? onText : 0xFFC9CBD0, label, labelEnd);
  return pressed;
}

// LCD style read-out box; returns the inner rectangle's origin.
inline void lcd(const char* text, const char* widest, ImU32 ink, float scale = 1.9f) {
  ImFont* font = ImGui::GetFont();
  const float fs = ImGui::GetFontSize() * scale;
  const ImVec2 w = font->CalcTextSizeA(fs, 1e9f, 0.f, widest);
  const ImVec2 t = font->CalcTextSizeA(fs, 1e9f, 0.f, text);
  const ImVec2 pad(12.f, 5.f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 sz(w.x + pad.x * 2, w.y + pad.y * 2);
  ImGui::InvisibleButton("##lcd", sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q(p.x + sz.x, p.y + sz.y);
  dl->AddRectFilled(p, q, rgba(0.02f, 0.03f, 0.035f), 4.f);
  vgrad(dl, ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, q.y - 1), rgba(0.f, 0.f, 0.f, 0.55f), rgba(0.f, 0.f, 0.f, 0.f));
  dl->AddRect(p, q, rgba(0.30f, 0.32f, 0.36f), 4.f);
  const ImVec2 at(q.x - pad.x - t.x, p.y + pad.y);
  dl->AddText(font, fs, ImVec2(at.x, at.y + 1), shade(ink, 0.35f, 200), text);   // glow
  dl->AddText(font, fs, at, ink, text);
}

// Vertical fader with a dB scale. Returns true when the value changed. Alt-click or double click = 0 dB.
inline bool fader(const char* id, float* db, ImVec2 size, float lo = -60.f, float hi = 12.f) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, size);
  const bool active = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  bool changed = false;
  const float capH = 22.f, track = size.y - capH;
  auto yOf = [&](float v) { return p.y + capH * 0.5f + track * (1.f - (v - lo) / (hi - lo)); };
  if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
    const float v = lo + (1.f - (ImGui::GetIO().MousePos.y - p.y - capH * 0.5f) / track) * (hi - lo);
    *db = std::clamp(v, lo, hi); changed = true;
  }
  if (hov) {   // fine control with the wheel
    const float w = ImGui::GetIO().MouseWheel;
    if (w != 0.f) { *db = std::clamp(*db + w * (ImGui::GetIO().KeyShift ? 0.1f : 1.f), lo, hi); changed = true; }
  }
  if ((ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) || (hov && ImGui::IsMouseDoubleClicked(0))) { *db = 0.f; changed = true; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = p.x + size.x * 0.5f;
  // groove
  dl->AddRectFilled(ImVec2(cx - 2.f, p.y + capH * 0.5f), ImVec2(cx + 2.f, p.y + size.y - capH * 0.5f), rgba(0.02f, 0.02f, 0.03f), 2.f);
  dl->AddRect(ImVec2(cx - 3.f, p.y + capH * 0.5f - 1), ImVec2(cx + 3.f, p.y + size.y - capH * 0.5f + 1), rgba(1.f, 1.f, 1.f, 0.07f), 3.f);
  // scale
  static const float marks[] = {12.f, 6.f, 0.f, -6.f, -12.f, -24.f, -40.f, -60.f};
  for (float m : marks) {
    const float y = yOf(m);
    dl->AddLine(ImVec2(cx - 15.f, y), ImVec2(cx - 8.f, y), m == 0.f ? rgba(1.f, 1.f, 1.f, 0.8f) : rgba(1.f, 1.f, 1.f, 0.35f));
  }
  // cap
  const float y = yOf(*db);
  const ImVec2 a(cx - 13.f, y - 9.f), b(cx + 13.f, y + 9.f);
  face(dl, a, b, active ? 0xFF6A6D75 : hov ? 0xFF5A5D65 : 0xFF4E5058, false, false, 3.f);
  dl->AddLine(ImVec2(a.x + 2, y), ImVec2(b.x - 2, y), 0xFFFFFFFF, 1.5f);
  return changed;
}

// Segmented LED meter, -60 .. +6 dBFS.
inline void ledMeter(ImDrawList* dl, ImVec2 pos, ImVec2 size, float level, bool clipped) {
  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), rgba(0.02f, 0.02f, 0.025f), 1.5f);
  const float db = level > 1e-5f ? 20.f * std::log10(level) : -100.f;
  const float seg = 3.f, gap = 1.f;
  const int n = std::max(1, static_cast<int>(size.y / (seg + gap)));
  for (int i = 0; i < n; ++i) {
    const float segDb = -60.f + 66.f * static_cast<float>(i) / static_cast<float>(n - 1);
    const bool lit = db >= segDb;
    const ImU32 on = segDb > -3.f ? rgba(1.f, 0.22f, 0.18f) : segDb > -12.f ? rgba(1.f, 0.82f, 0.15f) : rgba(0.2f, 0.9f, 0.4f);
    const float y = pos.y + size.y - (static_cast<float>(i) + 1.f) * (seg + gap);
    dl->AddRectFilled(ImVec2(pos.x + 1, y), ImVec2(pos.x + size.x - 1, y + seg), lit ? on : shade(on, 0.17f, 255));
  }
  if (clipped) dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + 4), rgba(1.f, 0.1f, 0.1f));
}


// ---- instrument widgets ---------------------------------------------------------------------
inline const ImU32 kGold = 0xFF3FB8E8;   // active / value colour of the instrument panels
inline const ImU32 kNavy = 0xFF2E1D10;   // display background

// Rotary knob. Drag up/down (Shift = fine), wheel, double-click or Alt-click = default.
// `centered` draws the value arc from the middle (for bipolar controls); `logScale` maps the travel logarithmically.
inline bool knob(const char* id, const char* label, float* v, float lo, float hi, float def, const char* fmt, ImU32 arc = kGold,
                 float diameter = 44.f, bool centered = false, bool logScale = false) {
  const float labelW = ImGui::CalcTextSize(label).x;
  const ImVec2 size(std::max(diameter + 12.f, labelW + 4.f), diameter + 18.f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, size);
  const bool active = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  auto toT = [&](float x) { return logScale ? std::log(x / lo) / std::log(hi / lo) : (x - lo) / (hi - lo); };
  auto fromT = [&](float t) { t = std::clamp(t, 0.f, 1.f); return logScale ? lo * std::pow(hi / lo, t) : lo + t * (hi - lo); };
  bool changed = false;
  float t = std::clamp(toT(*v), 0.f, 1.f);
  if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
    const float d = -ImGui::GetIO().MouseDelta.y * (ImGui::GetIO().KeyShift ? 0.0008f : 0.006f);
    if (d != 0.f) { t = std::clamp(t + d, 0.f, 1.f); *v = fromT(t); changed = true; }
  }
  if (hov && ImGui::GetIO().MouseWheel != 0.f) { t = std::clamp(t + ImGui::GetIO().MouseWheel * 0.02f, 0.f, 1.f); *v = fromT(t); changed = true; }
  if ((hov && ImGui::IsMouseDoubleClicked(0)) || (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt)) { *v = def; t = std::clamp(toT(def), 0.f, 1.f); changed = true; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p.x + size.x * 0.5f, p.y + diameter * 0.5f + 2.f);
  const float r = diameter * 0.5f;
  const float a0 = 0.75f * 3.14159265f, a1 = 2.25f * 3.14159265f;
  dl->PathArcTo(c, r, a0, a1, 32); dl->PathStroke(rgba(0.f, 0.f, 0.f, 0.55f), 0, 4.5f);
  dl->PathArcTo(c, r, a0, a1, 32); dl->PathStroke(rgba(0.30f, 0.32f, 0.36f), 0, 2.5f);
  const float aMid = 1.5f * 3.14159265f, aVal = a0 + (a1 - a0) * t;
  const float from = centered ? aMid : a0, to = aVal;
  if (std::fabs(to - from) > 0.01f) { dl->PathArcTo(c, r, std::min(from, to), std::max(from, to), 32); dl->PathStroke(arc, 0, 3.f); }
  dl->AddCircleFilled(c, r - 6.f, rgba(0.10f, 0.105f, 0.12f), 24);
  dl->AddCircleFilled(ImVec2(c.x, c.y - 1.f), r - 7.f, hov || active ? rgba(0.30f, 0.31f, 0.35f) : rgba(0.24f, 0.25f, 0.28f), 24);
  dl->AddCircleFilled(ImVec2(c.x, c.y - 3.f), r - 12.f, rgba(1.f, 1.f, 1.f, 0.05f), 20);
  const ImVec2 tip(c.x + std::cos(aVal) * (r - 8.f), c.y + std::sin(aVal) * (r - 8.f)), root(c.x + std::cos(aVal) * (r * 0.35f), c.y + std::sin(aVal) * (r * 0.35f));
  dl->AddLine(root, tip, 0xFFF2F2F5, 2.f);
  char buf[40];
  const bool showValue = active || hov;
  if (showValue) std::snprintf(buf, sizeof buf, fmt, static_cast<double>(*v));
  const char* txt = showValue ? buf : label;
  const ImVec2 ts = ImGui::CalcTextSize(txt);
  dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + diameter + 3.f), showValue ? kGold : rgba(0.70f, 0.72f, 0.76f), txt);
  return changed;
}

// Thin vertical slider with a gold handle (used for the velocity amount next to envelopes).
inline bool thinVSlider(const char* id, float* v, float lo, float hi, float h) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(16.f, h));
  bool changed = false;
  if (ImGui::IsItemActive()) { *v = std::clamp(lo + (1.f - (ImGui::GetIO().MousePos.y - p.y - 4.f) / (h - 8.f)) * (hi - lo), lo, hi); changed = true; }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { *v = lo; changed = true; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = p.x + 8.f, y = p.y + 4.f + (h - 8.f) * (1.f - (*v - lo) / (hi - lo));
  dl->AddRectFilled(ImVec2(cx - 2.f, p.y + 2.f), ImVec2(cx + 2.f, p.y + h - 2.f), rgba(0.03f, 0.03f, 0.04f), 2.f);
  dl->AddRectFilled(ImVec2(cx - 2.f, y), ImVec2(cx + 2.f, p.y + h - 2.f), shade(kGold, 0.55f, 255), 2.f);
  dl->AddRectFilled(ImVec2(cx - 6.f, y - 3.f), ImVec2(cx + 6.f, y + 3.f), kGold, 2.f);
  return changed;
}

// Small on/off switch with a light, used for panel power buttons and toggles.
inline bool powerButton(const char* id, bool* on, ImU32 color = 0xFFE89A2F) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(18.f, 18.f));
  if (pressed) *on = !*on;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p.x + 9.f, p.y + 9.f);
  const ImU32 ink = *on ? color : rgba(0.45f, 0.47f, 0.52f);
  dl->PathArcTo(c, 5.5f, -1.0f, 4.2f, 14); dl->PathStroke(ink, 0, 1.8f);
  dl->AddLine(ImVec2(c.x, c.y - 7.f), ImVec2(c.x, c.y - 1.f), ink, 1.8f);
  return pressed;
}

// Envelope drawing (attack, hold, decay, sustain, release) on a navy field. Returns true when changed.
// The five read-outs below are draggable numbers in ms / percent.
inline bool envelopeGraph(const char* id, float* a, float* hd, float* d, float* sus, float* r, float width, float height, ImU32 line = kGold) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), kNavy, 3.f);
  dl->AddRect(p, ImVec2(p.x + width, p.y + height), rgba(0.f, 0.f, 0.f, 0.8f), 3.f);
  const float hold = hd ? *hd : 0.f;
  const float total = std::max(0.08f, *a + hold + *d + 0.35f * (*a + hold + *d + *r + 0.2f) + *r);
  const float top = p.y + 8.f, bot = p.y + height - 6.f;
  const float x0 = p.x + 4.f, w = width - 8.f;
  const float xa = x0 + w * *a / total, xh = xa + w * hold / total, xd = xh + w * *d / total;
  const float xs = xd + w * 0.35f * (*a + hold + *d + *r + 0.2f) / total, xr = std::min(p.x + width - 4.f, xs + w * *r / total);
  const float ys = bot - (bot - top) * *sus;
  for (int i = 1; i < 4; ++i) dl->AddLine(ImVec2(p.x + 4.f, top + (bot - top) * static_cast<float>(i) / 4.f), ImVec2(p.x + width - 4.f, top + (bot - top) * static_cast<float>(i) / 4.f), rgba(1.f, 1.f, 1.f, 0.05f));
  ImVec2 pts[6] = {ImVec2(x0, bot), ImVec2(xa, top), ImVec2(xh, top), ImVec2(xd, ys), ImVec2(xs, ys), ImVec2(xr, bot)};
  dl->AddConvexPolyFilled(pts, 6, shade(line, 0.5f, 60));
  dl->AddPolyline(pts, 6, line, 0, 1.8f);
  for (const ImVec2& q : pts) dl->AddCircleFilled(q, 2.2f, 0xFFFFFFFF);
  ImGui::Dummy(ImVec2(width, height));
  bool changed = false;
  const int n = hd ? 5 : 4;
  const float cw = (width - 2.f * static_cast<float>(n - 1)) / static_cast<float>(n);
  auto cell = [&](const char* tag, float* val, bool percent, bool first) {
    if (!first) ImGui::SameLine(0, 2.f);
    float shown = percent ? *val * 100.f : *val * 1000.f;
    ImGui::SetNextItemWidth(cw);
    char f[24]; char idb[8]; std::snprintf(idb, sizeof idb, "##%s", tag); std::snprintf(f, sizeof f, percent ? "%s %%.0f%%%%" : "%s %%.0f", tag);
    if (ImGui::DragFloat(idb, &shown, percent ? 0.5f : (shown > 500.f ? 8.f : 1.f), 0.f, percent ? 100.f : 10000.f, f, ImGuiSliderFlags_NoRoundToFormat)) { *val = percent ? shown / 100.f : shown / 1000.f; changed = true; }
  };
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 2.f));
  cell("A", a, false, true);
  if (hd) cell("H", hd, false, false);
  cell("D", d, false, false);
  cell("S", sus, true, false);
  cell("R", r, false, false);
  ImGui::PopStyleVar();
  ImGui::PopID();
  return changed;
}


// ---- toolbar icons -----------------------------------------------------------------------------
enum class TIcon { Trim, Selector, Grabber, Zoom, Smart, Scrub, Pencil, Left, Right, Wave, Bars, Marker, Follow, Link };

// Square tool button with a drawn icon. `enabled` = false draws it dimmed and ignores clicks.
inline bool toolButton(const char* id, TIcon icon, bool lit, ImU32 litColor, ImVec2 size, bool enabled = true) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, size) && enabled;
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive() && enabled;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q(p.x + size.x, p.y + size.y);
  face(dl, p, q, lit ? litColor : 0xFF3A3C42, act, hov && enabled, 2.5f);
  const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
  const float h = std::min(size.x, size.y) * 0.30f;
  const ImU32 ink = !enabled ? 0xFF6A6C72 : lit ? 0xFFFFFFFF : 0xFFD0D2D8;
  const float w = 1.7f;
  switch (icon) {
    case TIcon::Trim:
      dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x - h, c.y + h), ink, 2.2f);
      dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), ink, w);
      dl->AddTriangleFilled(ImVec2(c.x + h + 1, c.y), ImVec2(c.x + h * 0.2f, c.y - h * 0.6f), ImVec2(c.x + h * 0.2f, c.y + h * 0.6f), ink);
      break;
    case TIcon::Selector:
      dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y + h), ink, w);
      dl->AddLine(ImVec2(c.x - h * 0.55f, c.y - h), ImVec2(c.x + h * 0.55f, c.y - h), ink, w);
      dl->AddLine(ImVec2(c.x - h * 0.55f, c.y + h), ImVec2(c.x + h * 0.55f, c.y + h), ink, w);
      break;
    case TIcon::Grabber:
      dl->AddRectFilled(ImVec2(c.x - h * 0.9f, c.y - h * 0.15f), ImVec2(c.x + h * 0.9f, c.y + h), ink, 2.5f);
      for (int k = 0; k < 4; ++k) { const float x = c.x - h * 0.75f + static_cast<float>(k) * h * 0.5f; dl->AddLine(ImVec2(x, c.y - h * 0.15f), ImVec2(x, c.y - h * (k == 1 || k == 2 ? 1.f : 0.75f)), ink, 2.f); }
      break;
    case TIcon::Zoom:
      dl->AddCircle(ImVec2(c.x - h * 0.2f, c.y - h * 0.2f), h * 0.7f, ink, 16, 1.8f);
      dl->AddLine(ImVec2(c.x + h * 0.3f, c.y + h * 0.3f), ImVec2(c.x + h, c.y + h), ink, 2.4f);
      break;
    case TIcon::Smart:
      dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), ink, w);
      dl->AddTriangleFilled(ImVec2(c.x - h - 1, c.y), ImVec2(c.x - h * 0.4f, c.y - h * 0.5f), ImVec2(c.x - h * 0.4f, c.y + h * 0.5f), ink);
      dl->AddTriangleFilled(ImVec2(c.x + h + 1, c.y), ImVec2(c.x + h * 0.4f, c.y - h * 0.5f), ImVec2(c.x + h * 0.4f, c.y + h * 0.5f), ink);
      break;
    case TIcon::Scrub:
      dl->AddRectFilled(ImVec2(c.x - h, c.y - h * 0.4f), ImVec2(c.x - h * 0.3f, c.y + h * 0.4f), ink);
      dl->AddTriangleFilled(ImVec2(c.x - h * 0.3f, c.y - h * 0.4f), ImVec2(c.x + h * 0.3f, c.y - h * 0.9f), ImVec2(c.x + h * 0.3f, c.y + h * 0.9f), ink);
      dl->AddTriangleFilled(ImVec2(c.x - h * 0.3f, c.y + h * 0.4f), ImVec2(c.x - h * 0.3f, c.y - h * 0.4f), ImVec2(c.x + h * 0.3f, c.y + h * 0.9f), ink);
      dl->PathArcTo(c, h * 0.9f, -0.7f, 0.7f, 8); dl->PathStroke(ink, 0, 1.4f);
      break;
    case TIcon::Pencil:
      dl->AddLine(ImVec2(c.x + h, c.y - h), ImVec2(c.x - h * 0.5f, c.y + h * 0.5f), ink, 3.f);
      dl->AddTriangleFilled(ImVec2(c.x - h, c.y + h), ImVec2(c.x - h * 0.7f, c.y + h * 0.2f), ImVec2(c.x - h * 0.2f, c.y + h * 0.7f), ink);
      break;
    case TIcon::Left: dl->AddTriangleFilled(ImVec2(c.x - h, c.y), ImVec2(c.x + h * 0.7f, c.y - h), ImVec2(c.x + h * 0.7f, c.y + h), ink); break;
    case TIcon::Right: dl->AddTriangleFilled(ImVec2(c.x + h, c.y), ImVec2(c.x - h * 0.7f, c.y - h), ImVec2(c.x - h * 0.7f, c.y + h), ink); break;
    case TIcon::Wave:
      for (int k = -3; k <= 3; ++k) { const float x = c.x + static_cast<float>(k) * h * 0.32f, a = h * (1.f - std::fabs(static_cast<float>(k)) * 0.25f); dl->AddLine(ImVec2(x, c.y - a), ImVec2(x, c.y + a), ink, 1.5f); }
      break;
    case TIcon::Bars:
      for (int k = -1; k <= 1; ++k) dl->AddRectFilled(ImVec2(c.x - h, c.y + static_cast<float>(k) * h * 0.7f - h * 0.2f), ImVec2(c.x + h, c.y + static_cast<float>(k) * h * 0.7f + h * 0.2f), ink, 1.f);
      break;
    case TIcon::Marker:
      dl->AddLine(ImVec2(c.x - h * 0.6f, c.y - h), ImVec2(c.x - h * 0.6f, c.y + h), ink, w);
      dl->AddTriangleFilled(ImVec2(c.x - h * 0.6f, c.y - h), ImVec2(c.x + h, c.y - h * 0.4f), ImVec2(c.x - h * 0.6f, c.y + h * 0.2f), ink);
      break;
    case TIcon::Follow:
      dl->AddRect(ImVec2(c.x - h, c.y - h * 0.7f), ImVec2(c.x + h, c.y + h * 0.7f), ink, 1.f, 0, w);
      dl->AddLine(ImVec2(c.x, c.y - h * 0.7f), ImVec2(c.x, c.y + h * 0.7f), ink, w);
      dl->AddTriangleFilled(ImVec2(c.x + h * 0.9f, c.y), ImVec2(c.x + h * 0.2f, c.y - h * 0.4f), ImVec2(c.x + h * 0.2f, c.y + h * 0.4f), ink);
      break;
    case TIcon::Link:
      dl->AddRect(ImVec2(c.x - h, c.y - h * 0.4f), ImVec2(c.x + h * 0.2f, c.y + h * 0.4f), ink, h * 0.4f, 0, w);
      dl->AddRect(ImVec2(c.x - h * 0.2f, c.y - h * 0.4f), ImVec2(c.x + h, c.y + h * 0.4f), ink, h * 0.4f, 0, w);
      break;
  }
  return pressed;
}

}  // namespace wezt::ui::style
