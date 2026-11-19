#pragma once
// A few per-user settings kept in a small text file (key=value lines) in the user's config folder:
//   Windows: %APPDATA%\Wezt\settings.txt   macOS: ~/Library/Application Support/Wezt/settings.txt
//   Linux: $XDG_CONFIG_HOME/wezt/settings.txt (or ~/.config/wezt). WEZT_CONFIG_DIR overrides the folder (tests).
#include <string>

namespace wezt {

struct Prefs {
  bool introEnabled = true;        // play the opening animation and sound at start-up
  bool introLogoBlack = true;      // the logo in polished black (true) or polished gold (false)
  std::string introSoundPath;      // empty = the built-in sound; otherwise a WAV or MP3 the user chose
};

std::string prefsPath();                                   // full path of the settings file ("" if no home folder is known)
Prefs loadPrefs(const std::string& path = prefsPath());    // defaults for anything missing or unreadable
bool savePrefs(const Prefs& p, const std::string& path = prefsPath());   // creates the folder; false if it cannot be written

}  // namespace wezt
