// File > Settings...: the start-up intro switch and the sound it plays (built-in, or the user's own WAV/MP3).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "ui/app.h"

namespace wezt::ui {
namespace fs = std::filesystem;

namespace {
std::string homeDir() {
  for (const char* n : {"USERPROFILE", "HOME"}) if (const char* v = std::getenv(n)) if (*v) return v;
  return ".";
}
bool isAudioName(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".wav" || e == ".wave" || e == ".mp3";
}
}  // namespace

void App::setIntroEnabled(bool on) {
  prefs_.introEnabled = on;
  savePrefs(prefs_);
}

void App::setIntroLogoBlack(bool black) {
  prefs_.introLogoBlack = black;
  savePrefs(prefs_);
}

bool App::setIntroSound(const std::string& path, std::string* message) {
  if (path.empty()) {
    prefs_.introSoundPath.clear();
    savePrefs(prefs_);
    if (message) *message = "Using the built-in intro sound.";
    return true;
  }
  io::StereoClip clip; std::string err;
  if (!io::loadStereoClip(path, clip, &err)) { if (message) *message = "Cannot use that file: " + err + "."; return false; }
  prefs_.introSoundPath = path;
  savePrefs(prefs_);
  if (message) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "Using your file (%.1f s, %u Hz).%s", clip.seconds(), clip.rate, clip.seconds() >= 29.99 ? " Only the first 30 seconds play." : "");
    *message = buf;
  }
  return true;
}

void App::drawSettings() {
  if (!showSettings_) return;
  ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 120.f), ImGuiCond_Appearing, ImVec2(0.5f, 0.f));
  if (!ImGui::Begin("Settings", &showSettings_, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }

  ImGui::SeparatorText("Start-up intro");
  bool on = prefs_.introEnabled;
  if (ImGui::Checkbox("Play the opening intro when Wezt starts (spinning gold logo, cinematic sound)", &on)) setIntroEnabled(on);
  ImGui::TextDisabled("Click or press any key during the intro to skip it. Turn it off here to open straight to the studio.");

  ImGui::Spacing();
  ImGui::TextUnformatted("Logo in the intro");
  if (ImGui::RadioButton("Polished black with a gold rim", prefs_.introLogoBlack)) setIntroLogoBlack(true);
  ImGui::SameLine();
  if (ImGui::RadioButton("Polished gold", !prefs_.introLogoBlack)) setIntroLogoBlack(false);

  ImGui::Spacing();
  ImGui::TextUnformatted("Intro sound");
  const bool custom = !prefs_.introSoundPath.empty();
  if (ImGui::RadioButton("Built-in (cinematic rise, impact and a metal clash)", !custom)) { setIntroSound("", &introSoundMsg_); browsing_ = false; }
  if (ImGui::RadioButton("My own file (WAV or MP3)", custom) && !custom) { browsing_ = true; if (browseDir_.empty()) browseDir_ = homeDir(); }
  if (custom) ImGui::TextDisabled("%s", prefs_.introSoundPath.c_str());

  if (ImGui::Button("Choose a file...")) {
    browsing_ = true;
    if (browseDir_.empty()) {
      std::error_code ec;
      const fs::path cur = prefs_.introSoundPath;
      browseDir_ = custom && fs::exists(cur.parent_path(), ec) ? cur.parent_path().string() : homeDir();
    }
  }
  ImGui::SameLine();
  const bool playing = introPreview_ && introPreview_->playing();
  if (!playing) {
    if (ImGui::Button("Play the sound")) {
      if (!introPreview_) introPreview_ = std::make_unique<IntroPlayer>();
      std::string m;
      if (!introPreview_->start(prefs_.introSoundPath, &m) || !m.empty()) introSoundMsg_ = m;
    }
  } else if (ImGui::Button("Stop")) introPreview_->stop(0.2);
  ImGui::SameLine();
  if (ImGui::Button("Watch the intro again")) introReplay_ = true;
  if (!introSoundMsg_.empty()) ImGui::TextWrapped("%s", introSoundMsg_.c_str());

  if (browsing_) {
    ImGui::Separator();
    ImGui::SetNextItemWidth(-60.f);
    ImGui::InputText("##dir", &browseDir_);
    ImGui::SameLine();
    if (ImGui::Button("Up")) { const fs::path p = fs::path(browseDir_).parent_path(); if (!p.empty()) browseDir_ = p.string(); }
    std::error_code ec;
    std::vector<fs::directory_entry> dirs, files;
    for (fs::directory_iterator it(browseDir_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      const std::string name = it->path().filename().string();
      if (name.empty() || name[0] == '.') continue;
      if (it->is_directory(e2)) dirs.push_back(*it);
      else if (isAudioName(it->path())) files.push_back(*it);
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path().filename().string() < b.path().filename().string(); };
    std::sort(dirs.begin(), dirs.end(), byName); std::sort(files.begin(), files.end(), byName);
    if (ImGui::BeginChild("##files", ImVec2(0, 230), ImGuiChildFlags_Borders)) {
      if (ec) ImGui::TextDisabled("Cannot read this folder.");
      std::string go;
      for (const auto& d : dirs) if (ImGui::Selectable(("[folder]  " + d.path().filename().string()).c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups)) go = d.path().string();
      for (const auto& f : files) if (ImGui::Selectable(f.path().filename().string().c_str())) {
        if (setIntroSound(f.path().string(), &introSoundMsg_)) browsing_ = false;
      }
      if (dirs.empty() && files.empty() && !ec) ImGui::TextDisabled("No folders or WAV/MP3 files here.");
      if (!go.empty()) browseDir_ = go;
    }
    ImGui::EndChild();
    ImGui::TextDisabled("WAV and MP3 files are listed. The first 30 seconds are used.");
  }
  ImGui::End();
}

}  // namespace wezt::ui
