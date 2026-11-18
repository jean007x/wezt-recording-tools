#pragma once
// The local assistant: you type (or speak through any speech-to-text you like) and it answers about
// your music. It runs entirely on this computer.
//
// What it can do on its own, with no model at all: measure audio files, build style profiles from
// your own mixes and masters ("me") and from reference tracks you give it, compare a track with a
// profile, explain the differences, master a file toward a profile or a delivery target, and
// remember notes about how you like to work. It learns from every accepted or rejected suggestion.
//
// An optional local language model (see llm.h) is used only for open conversation. It receives your
// notes and a summary of your profile as context. It can only produce text.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ai/advisor.h"
#include "ai/features.h"
#include "ai/llm.h"
#include "ai/profile.h"

namespace wezt::ai {

class Assistant {
 public:
  // `dataDir` holds profiles/, preferences.txt and notes.txt. It is created on first save.
  explicit Assistant(std::string dataDir);

  struct Reply {
    std::string text;
    bool usedModel = false;
  };
  Reply say(const std::string& userText);

  void setModel(std::shared_ptr<ILanguageModel> m) { model_ = std::move(m); }
  // Lets the app take commands about the session itself ("play", "add a track"). Return true when the
  // text was a session command and put the answer in `reply`.
  using SessionHandler = std::function<bool(const std::string& text, std::string& reply)>;
  void setSessionHandler(SessionHandler h) { session_ = std::move(h); }

  // State, for the UI and tests.
  const Preferences& preferences() const { return prefs_; }
  const std::vector<Suggestion>& pendingSuggestions() const { return pending_; }
  const std::vector<std::string>& notes() const { return notes_; }
  ProfileStore& profiles() { return store_; }

 private:
  Reply cmdAnalyze(const std::string& file);
  Reply cmdLearn(const std::string& file, const std::string& profile);
  Reply cmdAdvise(const std::string& file, const std::string& profile);
  Reply cmdAnswer(const std::string& text, bool accept);
  Reply cmdMaster(const std::string& text, const std::string& file, const std::string& profile);
  Reply cmdShowProfile(const std::string& name);
  Reply cmdNotes();
  bool loadFile(const std::string& file, std::string& err);
  void savePrefs();
  void saveNotes();
  std::string profileSummary(const StyleProfile& p) const;
  std::string contextForModel() const;

  std::string dir_;
  ProfileStore store_;
  Preferences prefs_;
  std::vector<std::string> notes_;
  std::shared_ptr<ILanguageModel> model_;
  SessionHandler session_;
  std::vector<ChatMessage> history_;

  // the file being talked about
  std::string lastFile_;
  AudioBuffer audio_;
  double rate_ = 0;
  Features features_;
  // advice waiting for an answer
  std::vector<Suggestion> pending_;
  std::vector<bool> answered_;
  std::string pendingProfile_, pendingFile_;
  std::vector<Suggestion> accepted_;
};

std::string describeFeatures(const Features& f);

}  // namespace wezt::ai
