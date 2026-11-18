#pragma once
// A built-in guide: ask how to do something in Wezt or in music production and get step-by-step
// instructions. It is a small library of written answers searched by keywords, so it only says what
// is in it; when nothing matches it says so and lists topics. It runs offline.
#include <string>
#include <vector>

namespace wezt::ai {

struct TutorTopic {
  std::string id, title;
  std::vector<std::string> keywords;
  std::vector<std::string> steps;
  std::string tip;
};

class Tutor {
 public:
  Tutor();
  // Best matching topic, or nullptr when nothing is close enough.
  const TutorTopic* find(const std::string& question) const;
  // A ready-to-show answer (title, numbered steps, tip), or a list of topics when nothing matches.
  std::string answer(const std::string& question) const;
  const std::vector<TutorTopic>& topics() const { return topics_; }

 private:
  std::vector<TutorTopic> topics_;
};

}  // namespace wezt::ai
