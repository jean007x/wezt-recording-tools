// "My Voice Bank": pick the track that holds each recorded vowel, confirm the voice is yours, build.
// Recording itself is the normal workflow: arm a track, hold one steady note ("ahhh") for 2 seconds, repeat.
#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ai/producer.h"
#include "ui/app.h"
#include "voice/voice_bank.h"

namespace wezt::ui {

namespace fs = std::filesystem;

std::string App::voiceDir() const {
  std::string dir = assistantDir_;
  if (dir.empty()) { const char* h = std::getenv("HOME"); if (!h) h = std::getenv("USERPROFILE"); dir = std::string(h ? h : ".") + "/.wezt"; }
  return dir + "/generated/voices";
}

void App::drawVoiceBankWindow() {
  ImGui::SetNextWindowSize(ImVec2(520, 470), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("My Voice Bank", &showVoiceBank_)) { ImGui::End(); return; }
  ImGui::TextWrapped("Record each vowel on its own track: one steady, dry note of 2 seconds or more, at a comfortable pitch (for example a long \"ahhh\"). "
                     "Then choose the track for each vowel below. The singer uses your real audio for the vowels, moved to the melody's pitch.");
  ImGui::TextDisabled("Needed: ah, ee, oh, oo, eh. The rest are optional (a similar vowel is used when one is missing).");
  ImGui::Separator();
  struct V { const char* id; const char* hint; };
  static const V vowels[9] = {{"ah", "as in father"}, {"ee", "as in see"}, {"oh", "as in go"}, {"oo", "as in too"}, {"eh", "as in bed"},
                              {"ae", "as in cat"}, {"ih", "as in sit"}, {"uh", "as in cut"}, {"er", "as in her"}};
  const auto ids = session_->trackIds();
  for (int k = 0; k < 9; ++k) {
    ImGui::PushID(k);
    ImGui::Text("%s  %-12s", vowels[k].id, vowels[k].hint);
    ImGui::SameLine(160); ImGui::SetNextItemWidth(260);
    const int cur = bankTrack_[k];
    const session::Track* ct = cur >= 0 ? session_->track(static_cast<session::TrackId>(cur)) : nullptr;
    if (ImGui::BeginCombo("##t", ct ? ct->name.c_str() : (k < 5 ? "(choose a track)" : "(optional)"))) {
      if (ImGui::Selectable("(none)", cur < 0)) bankTrack_[k] = -1;
      for (auto id : ids) { const session::Track* t = session_->track(id); if (t && ImGui::Selectable((t->name + "##" + std::to_string(id)).c_str(), cur == static_cast<int>(id))) bankTrack_[k] = static_cast<int>(id); }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
  static const char* consNames[22] = {"s", "t", "k", "p", "m", "n", "l", "r", "sh", "f", "b", "d", "g", "h", "z", "v", "th", "ch", "w", "y", "j", "ng"};
  if (ImGui::CollapsingHeader("Consonants (optional: your own t, s, m ... instead of synthesized ones)")) {
    ImGui::TextWrapped("Record yourself saying the consonant followed by a held \"ah\" (\"sah\", \"tah\", \"mah\"), one per track. Only the part before the vowel is used. Each one you skip is sung by the synthesizer.");
    for (int k = 0; k < 22; ++k) {
      ImGui::PushID(100 + k);
      ImGui::Text("%-3s", consNames[k]); ImGui::SameLine(60); ImGui::SetNextItemWidth(260);
      const int cur = bankCons_[k];
      const session::Track* ct = cur >= 0 ? session_->track(static_cast<session::TrackId>(cur)) : nullptr;
      if (ImGui::BeginCombo("##c", ct ? ct->name.c_str() : "(optional)")) {
        if (ImGui::Selectable("(none)", cur < 0)) bankCons_[k] = -1;
        for (auto id : ids) { const session::Track* t = session_->track(id); if (t && ImGui::Selectable((t->name + "##" + std::to_string(id)).c_str(), cur == static_cast<int>(id))) bankCons_[k] = static_cast<int>(id); }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
  }
  ImGui::Separator();
  ImGui::SetNextItemWidth(220); ImGui::InputText("Bank name", &bankName_);
  ImGui::Checkbox("This is my own voice (or the singer agreed)", &bankConsent_);
  bool ready = bankConsent_;
  int chosen = 0; for (int k = 0; k < 9; ++k) if (bankTrack_[k] >= 0) ++chosen; for (int k = 0; k < 22; ++k) if (bankCons_[k] >= 0) ++chosen;
  ready = ready && chosen >= 1 && !bankName_.empty();
  if (!ready) ImGui::BeginDisabled();
  if (ImGui::Button("Build voice bank")) {
    std::vector<std::pair<std::string, std::string>> recs;
    for (int k = 0; k < 9; ++k) if (bankTrack_[k] >= 0) if (const session::Track* t = session_->track(static_cast<session::TrackId>(bankTrack_[k]))) recs.push_back({vowels[k].id, t->name});
    for (int k = 0; k < 22; ++k) if (bankCons_[k] >= 0) if (const session::Track* t = session_->track(static_cast<session::TrackId>(bankCons_[k]))) recs.push_back({consNames[k], t->name});
    ai::Producer prod(*session_, voiceDir().substr(0, voiceDir().size() - std::string("/voices").size()));
    const auto out = prod.buildVoiceBank(recs, bankName_, "", bankConsent_);
    bankStatus_ = out.message;
    setStatus(out.ok ? "Voice bank built" : "Voice bank: " + out.message);
    if (out.ok) rollBank_ = bankName_;
  }
  if (!ready) ImGui::EndDisabled();
  if (!bankConsent_) ImGui::TextDisabled("Tick the box to confirm the voice is yours.");
  if (!bankStatus_.empty()) { ImGui::Separator(); ImGui::TextWrapped("%s", bankStatus_.c_str()); }
  ImGui::Separator();
  ImGui::TextDisabled("Then open the Piano Roll, choose this bank in the \"vowels\" menu and press Sing it.");
  ImGui::End();
}

}  // namespace wezt::ui
