#pragma once
// A few per-user settings kept in a small text file (key=value lines) in the user's config folder:
//   Windows: %APPDATA%\Wezt\settings.txt   macOS: ~/Library/Application Support/Wezt/settings.txt
//   Linux: $XDG_CONFIG_HOME/wezt/settings.txt (or ~/.config/wezt). WEZT_CONFIG_DIR overrides the folder (tests).
#include <string>
#include <vector>

namespace wezt {

struct Prefs {
  bool introEnabled = true;        // play the opening animation and sound at start-up
  bool introLogoBlack = true;      // the logo in polished black (true) or polished gold (false)
  bool clickOn = false;            // metronome click on
  int countOffBars = 0;            // count-off before recording: 0, 1, 2 or 4 bars
  int preRollSeconds = 0;          // pre-roll before recording from the playhead: 0, 2, 4 or 8 seconds
  std::vector<std::string> recent; // recently saved or opened sessions, newest first (at most 8)
  std::string introSoundPath;      // empty = the built-in sound; otherwise a WAV or MP3 the user chose
};

void addRecent(Prefs& p, const std::string& path);         // moves/puts a path to the front, keeps 8
std::string prefsPath();                                   // full path of the settings file ("" if no home folder is known)
Prefs loadPrefs(const std::string& path = prefsPath());    // defaults for anything missing or unreadable
bool savePrefs(const Prefs& p, const std::string& path = prefsPath());   // creates the folder; false if it cannot be written

}  // namespace wezt
