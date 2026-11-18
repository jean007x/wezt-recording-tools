#pragma once
// What parameters MEAN, so an agent (or a person typing "make it darker") can steer plugins it has never
// seen. Every parameter can carry a semantic tag ("cutoff", "attack", "treble", ...). Built-in plugins set
// them; for third-party plugins the tag is guessed from the parameter's name, which is a heuristic and is
// reported as such. Intents ("brighter", "tighter", "more reverb") are small sets of moves on tags.
#include <string>
#include <vector>

namespace wezt::agent {

// Lower-case letters and digits only, for forgiving name matching ("Filter Cutoff" ~ "filtercutoff").
std::string normalizeName(const std::string& s);

// A tag guessed from a parameter's name and unit; empty when nothing fits. Heuristic.
std::string guessSemantic(const std::string& paramName, const std::string& unit);

// True when a parameter tagged `paramTag` answers to the intent tag `tag` ("comp_attack" answers to "attack";
// a tag written "=attack" matches only that exact tag).
bool tagMatches(const std::string& tag, const std::string& paramTag);

struct Move { std::string tag; int dir = 1; };   // dir +1 raises the parameter, -1 lowers it
struct Intent {
  std::vector<Move> moves;
  double amount = 0.15;                          // fraction of the parameter's range (normalized) per move
  std::vector<std::string> matched;              // the phrases that were understood
};

// "brighter", "a little punchier", "much less reverb", "boost the bass", "lower the cutoff" ...
Intent parseIntent(const std::string& text);

// Every tag the built-in vocabulary knows, for documentation and error messages.
std::vector<std::string> knownTags();

}  // namespace wezt::agent
