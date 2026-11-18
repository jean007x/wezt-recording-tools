#include "ai/assistant.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ai/audio_file.h"
#include "ai/master.h"
#include "io/wav.h"

namespace wezt::ai {
namespace fs = std::filesystem;
namespace {

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && (std::isspace(static_cast<unsigned char>(s[b - 1])) || s[b - 1] == '.' || s[b - 1] == '?' || s[b - 1] == '!')) --b;
  return s.substr(a, b - a);
}
bool has(const std::string& low, const char* w) { return low.find(w) != std::string::npos; }
bool hasWord(const std::string& low, const std::string& w) {
  for (std::size_t p = low.find(w); p != std::string::npos; p = low.find(w, p + 1)) {
    const bool l = p == 0 || !std::isalnum(static_cast<unsigned char>(low[p - 1]));
    const bool r = p + w.size() >= low.size() || !std::isalnum(static_cast<unsigned char>(low[p + w.size()]));
    if (l && r) return true;
  }
  return false;
}
template <class... A>
std::string fmt(const char* f, A... a) { char b[400]; std::snprintf(b, sizeof b, f, a...); return b; }  // NOLINT

// A file name in the text: quoted, or a word ending in .wav
std::string findFile(const std::string& text) {
  for (char q : {'"', '\''}) {
    const std::size_t a = text.find(q);
    if (a == std::string::npos) continue;
    const std::size_t b = text.find(q, a + 1);
    if (b != std::string::npos && b > a + 1) return text.substr(a + 1, b - a - 1);
  }
  std::istringstream is(text);
  std::string w;
  while (is >> w) {
    while (!w.empty() && (w.back() == ',' || w.back() == ';' || w.back() == '?' || w.back() == '!')) w.pop_back();
    const std::string lw = lower(w);
    if (lw.size() > 4 && (lw.substr(lw.size() - 4) == ".wav" || lw.substr(lw.size() - 5) == ".wave")) return w;
  }
  return {};
}
std::string removeFile(std::string text, const std::string& file) {
  if (file.empty()) return text;
  for (const std::string& pat : {"\"" + file + "\"", "'" + file + "'", file}) {
    const std::size_t p = text.find(pat);
    if (p != std::string::npos) { text.erase(p, pat.size()); break; }
  }
  return text;
}
// The word(s) after one of the keywords, e.g. "as my-lofi mixes".
std::string nameAfter(const std::string& text, const std::vector<std::string>& keys) {
  const std::string low = lower(text);
  std::size_t best = std::string::npos, klen = 0;
  for (auto& k : keys) {
    const std::string pat = " " + k + " ";
    const std::size_t p = low.find(pat);
    if (p != std::string::npos && (best == std::string::npos || p < best)) { best = p; klen = pat.size(); }
  }
  if (best == std::string::npos) return {};
  std::string rest = trim(text.substr(best + klen));
  for (const char* drop : {"the ", "my ", "profile ", "called ", "named "}) {
    const std::string d = drop;
    if (lower(rest).rfind(d, 0) == 0 && d != "my ") rest.erase(0, d.size());
  }
  if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) { const char q = rest.front(); const std::size_t e = rest.find(q, 1); return e == std::string::npos ? rest.substr(1) : rest.substr(1, e - 1); }
  // stop at a following keyword
  const std::string lr = lower(rest);
  std::size_t cut = rest.size();
  for (const char* stop : {" and ", " then ", " save ", " as ", " to ", " into "}) { const std::size_t p = lr.find(stop); if (p != std::string::npos) cut = std::min(cut, p); }
  return trim(rest.substr(0, cut));
}
std::string stem(const std::string& path) { return fs::path(path).stem().string(); }

}  // namespace

std::string describeFeatures(const Features& f) {
  std::ostringstream o;
  o << fmt("%.0f s, loudness %.1f LUFS (range %.1f LU), true peak %.1f dBTP, peak-to-RMS %.1f dB.\n", f.seconds, f.lufs, f.loudnessRangeLu, f.truePeakDb, f.crestDb);
  o << fmt("Stereo width %.2f (L/R correlation %.2f), brightness (centroid) %.0f Hz.\n", f.stereoWidth, f.correlation, f.centroidHz);
  if (f.bpm > 0) o << fmt("Tempo about %.0f BPM (confidence %.0f%%). ", f.bpm, 100.f * f.bpmConfidence);
  if (f.keyRoot >= 0) o << "Key probably " << keyName(f.keyRoot, f.minor) << fmt(" (confidence %.0f%%).", 100.f * f.keyConfidence);
  o << "\nBalance by octave band (dB relative to the total): ";
  for (int b = 0; b < kBands; ++b) o << kBandNames[b] << " " << fmt("%.0f", f.bandDb[static_cast<std::size_t>(b)]) << (b + 1 < kBands ? ", " : "");
  return o.str();
}

Assistant::Assistant(std::string dataDir) : dir_(std::move(dataDir)), store_((fs::path(dir_) / "profiles").string()) {
  std::ifstream pf(fs::path(dir_) / "preferences.txt", std::ios::binary);
  if (pf) { std::ostringstream ss; ss << pf.rdbuf(); Preferences p; if (Preferences::parse(ss.str(), p)) prefs_ = p; }
  std::ifstream nf(fs::path(dir_) / "notes.txt");
  std::string line;
  while (std::getline(nf, line)) if (!trim(line).empty()) notes_.push_back(line);
}

void Assistant::savePrefs() {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const fs::path p = fs::path(dir_) / "preferences.txt", t = fs::path(dir_) / "preferences.txt.tmp";
  { std::ofstream f(t, std::ios::binary | std::ios::trunc); f << prefs_.serialize(); }
  fs::rename(t, p, ec);
}
void Assistant::saveNotes() {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  std::ofstream f(fs::path(dir_) / "notes.txt", std::ios::trunc);
  for (auto& n : notes_) f << n << "\n";
}

bool Assistant::loadFile(const std::string& file, std::string& err) {
  if (file == lastFile_ && features_.valid()) return true;
  AudioBuffer a;
  double sr = 0;
  if (!loadAudioFile(file, a, sr, &err)) return false;
  Features f = analyze(a, sr);
  if (!f.valid()) { err = "the file is too short to analyse"; return false; }
  audio_ = std::move(a); rate_ = sr; features_ = f; lastFile_ = file;
  return true;
}

std::string Assistant::profileSummary(const StyleProfile& p) const {
  if (p.count() == 0) return "profile \"" + p.name() + "\" is empty";
  const Features m = p.mean();
  std::ostringstream o;
  o << "Profile \"" << p.name() << "\" from " << p.count() << (p.count() == 1 ? " track" : " tracks") << ": ";
  o << fmt("typically %.1f LUFS, peak-to-RMS %.1f dB, width %.2f, brightness %.0f Hz", m.lufs, m.crestDb, m.stereoWidth, m.centroidHz);
  const auto k = p.commonKey();
  if (k.first >= 0) o << ", most often in " << keyName(k.first, k.second);
  if (p.maxBpm() > 0) o << fmt(", tempos %.0f-%.0f BPM", p.minBpm(), p.maxBpm());
  o << ".";
  return o.str();
}

std::string Assistant::contextForModel() const {
  std::ostringstream o;
  o << "You are the assistant inside Wezt Recording Tools, a digital audio workstation. You run on the user's own computer. "
       "You cannot run actions or touch files; if the user wants something done, tell them the plain command to type "
       "(for example: analyze \"song.wav\", learn from \"mix.wav\", advise \"song.wav\" like my-profile, master \"song.wav\" to streaming). "
       "Be concise and honest. Do not claim to hear audio: you only know the measurements you are given.\n";
  for (auto& n : notes_) o << "User note: " << n << "\n";
  StyleProfile me;
  if (store_.load("me", me)) o << profileSummary(me) << "\n";
  if (features_.valid()) o << "Last file " << lastFile_ << ": " << describeFeatures(features_) << "\n";
  return o.str();
}

Assistant::Reply Assistant::cmdAnalyze(const std::string& file) {
  std::string err;
  if (file.empty() && lastFile_.empty()) return {"Which file? Give me a WAV file, for example: analyze \"my song.wav\"."};
  const std::string f = file.empty() ? lastFile_ : file;
  if (!loadFile(f, err)) return {"I could not open " + f + ": " + err};
  return {"Here is what I measure in " + fs::path(f).filename().string() + ":\n" + describeFeatures(features_)};
}

Assistant::Reply Assistant::cmdLearn(const std::string& file, const std::string& profile) {
  std::string err;
  const std::string f = file.empty() ? lastFile_ : file;
  if (f.empty()) return {"Which file should I learn from?"};
  if (!loadFile(f, err)) return {"I could not open " + f + ": " + err};
  const std::string name = ProfileStore::safeName(profile.empty() ? "me" : profile);
  if (name.empty()) return {"That profile name has no usable characters."};
  StyleProfile p(name);
  store_.load(name, p);
  p.add(features_);
  std::string se;
  if (!store_.save(p, &se)) return {"I could not save the profile: " + se};
  return {"Added " + fs::path(f).filename().string() + " to profile \"" + name + "\". " + profileSummary(p)};
}

Assistant::Reply Assistant::cmdAdvise(const std::string& file, const std::string& profile) {
  std::string err;
  const std::string f = file.empty() ? lastFile_ : file;
  if (f.empty()) return {"Which file should I look at?"};
  if (!loadFile(f, err)) return {"I could not open " + f + ": " + err};
  std::string name = ProfileStore::safeName(profile);
  if (name.empty()) {
    const auto all = store_.list();
    if (std::find(all.begin(), all.end(), "me") != all.end()) name = "me";
    else if (!all.empty()) name = all.front();
    else return {"I have no profile to compare with yet. Teach me first: learn from \"one of your finished mixes.wav\"  (or give a reference track: learn from \"ref.wav\" as reference-name)."};
  }
  StyleProfile target;
  if (!store_.load(name, target)) return {"I do not know a profile called \"" + name + "\". Known profiles: " + [&] { std::string s; for (auto& n : store_.list()) s += (s.empty() ? "" : ", ") + n; return s.empty() ? std::string("none") : s; }() + "."};
  pending_ = advise(features_, target, prefs_);
  answered_.assign(pending_.size(), false);
  accepted_.clear();
  pendingProfile_ = name; pendingFile_ = f;
  if (pending_.empty()) return {"Compared with \"" + name + "\" (" + std::to_string(target.count()) + " tracks), " + fs::path(f).filename().string() + " is already close: I have nothing worth changing."};
  std::ostringstream o;
  o << "Compared with \"" << name << "\" (" << target.count() << (target.count() == 1 ? " track" : " tracks") << "), " << pending_.size() << " thing" << (pending_.size() == 1 ? "" : "s") << " stand out:\n";
  for (std::size_t i = 0; i < pending_.size(); ++i) o << (i + 1) << ". " << pending_[i].text << fmt("  [%.0f%% sure]", 100.f * pending_[i].confidence) << "\n";
  o << "Say \"accept 1\", \"accept all\", \"accept 2 with 3.5\" (your own value), or \"reject 3\". I learn from every answer.";
  return {o.str()};
}

Assistant::Reply Assistant::cmdAnswer(const std::string& text, bool accept) {
  if (pending_.empty()) return {"There is no advice waiting. Ask me to advise a file first."};
  const std::string low = lower(text);
  std::vector<std::size_t> which;
  if (hasWord(low, "all") || hasWord(low, "everything")) for (std::size_t i = 0; i < pending_.size(); ++i) which.push_back(i);
  else {
    std::istringstream is(low);
    std::string w;
    bool afterWith = false;
    while (is >> w) {
      if (w == "with" || w == "at" || w == "to" || w == "by") afterWith = true;
      if (afterWith) continue;
      while (!w.empty() && !std::isdigit(static_cast<unsigned char>(w.back()))) w.pop_back();
      if (!w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const std::size_t n = static_cast<std::size_t>(std::atoi(w.c_str()));
        if (n >= 1 && n <= pending_.size()) which.push_back(n - 1);
      }
    }
  }
  if (which.empty()) return {"Which one? Say a number from the list, or \"all\"."};
  float custom = 0.f;
  bool hasCustom = false;
  if (accept && which.size() == 1) {
    const std::size_t p = low.find(" with ");
    if (p != std::string::npos) { char* end = nullptr; const std::string t = low.substr(p + 6); custom = std::strtof(t.c_str(), &end); hasCustom = end != t.c_str() && std::isfinite(custom); }
  }
  std::ostringstream o;
  for (std::size_t i : which) {
    if (answered_[i]) continue;
    answered_[i] = true;
    Suggestion s = pending_[i];
    const float applied = hasCustom ? custom : s.value;
    learn(prefs_, s, accept, applied);
    if (accept) { s.value = applied; accepted_.push_back(s); }
  }
  savePrefs();
  o << (accept ? "Noted, accepted " : "Noted, declined ") << which.size() << (which.size() == 1 ? " suggestion" : " suggestions") << ". ";
  o << (accept ? "I will weigh that kind of advice higher and aim where you aim. " : "I will offer that kind of advice less often and aim closer to how you left it. ");
  if (accept) o << "Say \"master\" to apply what you accepted.";
  return {o.str()};
}

Assistant::Reply Assistant::cmdMaster(const std::string& text, const std::string& fileIn, const std::string& profile) {
  std::string err;
  const std::string f = fileIn.empty() ? lastFile_ : fileIn;
  if (f.empty()) return {"Which file should I master?"};
  if (!loadFile(f, err)) return {"I could not open " + f + ": " + err};
  const std::string low = lower(text);
  // delivery target
  float lufs = -14.f, ceiling = -1.f;
  std::string label = "streaming (-14 LUFS)";
  bool preset = false;
  for (auto& d : deliveryTargets()) if (hasWord(low, d.name)) { lufs = d.lufs; ceiling = d.truePeakDb; label = std::string(d.name) + fmt(" (%.0f LUFS)", d.lufs); preset = true; }

  std::vector<Suggestion> sug;
  if (!profile.empty()) {  // an explicitly named profile must exist, whatever else is pending
    StyleProfile probe;
    if (ProfileStore::safeName(profile).empty() || !store_.load(profile, probe)) return {"I do not know a profile called \"" + profile + "\"."};
  }
  if (pendingFile_ == f && !accepted_.empty() && profile.empty()) sug = accepted_;
  else {
    std::string pname = ProfileStore::safeName(profile);
    if (pname.empty() && !preset) { const auto all = store_.list(); if (std::find(all.begin(), all.end(), "me") != all.end()) pname = "me"; }
    StyleProfile t;
    if (!pname.empty() && store_.load(pname, t)) {
      sug = advise(features_, t, prefs_);
      if (!preset) { label = "profile \"" + pname + "\""; }
      // the loudness suggestion decides the target when no delivery preset was named
      if (preset) sug.erase(std::remove_if(sug.begin(), sug.end(), [](const Suggestion& s) { return s.kind == SuggestionKind::Loudness; }), sug.end());
    } else if (!profile.empty() && !preset) {
      return {"I do not know a profile called \"" + profile + "\"."};
    }
  }
  MasterSettings ms = planMaster(features_, sug, lufs, ceiling);
  if (preset) { ms.targetLufs = lufs; ms.ceilingDb = ceiling; }
  MasterResult r = applyMaster(audio_, rate_, ms);
  if (!r.ok) return {"The mastering run failed."};

  fs::path out;
  const std::string outName = nameAfter(text, {"as", "into", "output"});
  if (!outName.empty() && lower(outName).find(".wav") != std::string::npos) out = outName;
  else out = fs::path(f).parent_path() / (stem(f) + "_master.wav");
  std::error_code ec;
  if (fs::equivalent(out, f, ec) || (fs::exists(out, ec) && !has(low, "overwrite"))) {
    return {"I did not write anything: " + out.string() + " already exists (or is the original). Choose another name with \"as new-name.wav\", or say \"overwrite\"."};
  }
  if (!io::writeWavFloat(out.string(), r.audio, static_cast<unsigned>(rate_), "Wezt local master")) return {"I could not write " + out.string() + "."};
  std::ostringstream o;
  o << "Mastered toward " << label << " and saved " << out.string() << ".\n";
  o << fmt("Loudness %.1f -> %.1f LUFS, true peak %.1f -> %.1f dBTP, peak-to-RMS %.1f -> %.1f dB, width %.2f -> %.2f.\n", r.before.lufs, r.after.lufs, r.before.truePeakDb, r.after.truePeakDb, r.before.crestDb, r.after.crestDb, r.before.stereoWidth, r.after.stereoWidth);
  if (!ms.eq.empty()) o << ms.eq.size() << " EQ move" << (ms.eq.size() == 1 ? "" : "s") << ", ";
  o << (ms.compress ? "compression, " : "") << "limiter at " << fmt("%.1f", r.limiterCeilingDb) << " dB.";
  if (std::fabs(r.after.lufs - ms.targetLufs) > 0.5f) o << fmt(" I could not reach %.1f LUFS without breaking the peak ceiling.", ms.targetLufs);
  return {o.str()};
}

Assistant::Reply Assistant::cmdShowProfile(const std::string& name) {
  StyleProfile p;
  const std::string n = ProfileStore::safeName(name.empty() ? "me" : name);
  if (n.empty() || !store_.load(n, p)) return {"I have no profile called \"" + (name.empty() ? std::string("me") : name) + "\" yet."};
  std::ostringstream o;
  o << profileSummary(p) << "\nTypical balance: ";
  const Features m = p.mean();
  for (int b = 0; b < kBands; ++b) o << kBandNames[b] << " " << fmt("%.0f", m.bandDb[static_cast<std::size_t>(b)]) << (b + 1 < kBands ? ", " : "");
  return {o.str()};
}

Assistant::Reply Assistant::cmdNotes() {
  std::ostringstream o;
  if (notes_.empty()) o << "I have no notes yet. Tell me things with \"remember that ...\".\n";
  else { o << "What I remember:\n"; for (std::size_t i = 0; i < notes_.size(); ++i) o << (i + 1) << ". " << notes_[i] << "\n"; }
  const auto all = store_.list();
  if (!all.empty()) { o << "Profiles: "; for (std::size_t i = 0; i < all.size(); ++i) o << all[i] << (i + 1 < all.size() ? ", " : ""); }
  else o << "No profiles yet.";
  return {o.str()};
}

Assistant::Reply Assistant::say(const std::string& userText) {
  const std::string text = trim(userText);
  const std::string low = lower(text);
  if (low.empty()) return {"I am listening."};
  const std::string file = findFile(text);
  const std::string rest = removeFile(text, file);
  const std::string lrest = lower(rest);

  auto firstWord = [&](std::initializer_list<const char*> ws) { for (auto w : ws) if (low.rfind(w, 0) == 0) return true; return false; };

  if (low == "help" || low == "what can you do" || low == "commands")
    return {"I run on this computer only. I can:\n"
            "- analyze \"file.wav\"  (loudness, tonal balance, tempo, key, width)\n"
            "- learn from \"file.wav\"  (adds it to your profile \"me\"), or: learn from \"ref.wav\" as <name> for a reference you want to study\n"
            "- profiles / show profile <name> / delete profile <name>\n"
            "- advise \"file.wav\" like <profile>  then accept 1 / accept all / accept 2 with 3.5 / reject 3\n"
            "- master \"file.wav\" to streaming | apple | cd | broadcast | club | <profile>  [as out.wav]\n"
            "- remember that ... / what do you remember / forget note 2\n"
            "Anything else goes to your local language model if you connected one."};

  if (firstWord({"remember that ", "remember: ", "note that ", "note: "})) {
    std::string n = trim(text.substr(text.find(' ') + 1));
    if (lower(n).rfind("that ", 0) == 0) n.erase(0, 5);
    if (n.empty()) return {"What should I remember?"};
    notes_.push_back(n);
    if (notes_.size() > 200) notes_.erase(notes_.begin());
    saveNotes();
    return {"I will remember that: " + n};
  }
  if (has(low, "what do you remember") || has(low, "what do you know about me") || low == "notes" || low == "show notes") return cmdNotes();
  if (firstWord({"forget note ", "delete note "})) {
    const int n = std::atoi(low.substr(low.rfind(' ') + 1).c_str());
    if (n < 1 || static_cast<std::size_t>(n) > notes_.size()) return {"There is no such note."};
    notes_.erase(notes_.begin() + (n - 1));
    saveNotes();
    return {"Forgotten."};
  }
  if (low == "profiles" || has(low, "list profiles") || has(low, "list my profiles")) return cmdNotes();
  if (firstWord({"delete profile ", "remove profile "})) {
    const std::string n = trim(text.substr(low.find("profile ") + 8));
    return {store_.remove(n) ? "Deleted profile \"" + n + "\"." : "I have no profile called \"" + n + "\"."};
  }
  if (firstWord({"show profile", "describe profile"}) || has(low, "how do i usually") || has(low, "what is my style") || has(low, "my usual mix")) {
    const std::string n = nameAfter(text, {"profile"});
    return cmdShowProfile(n);
  }
  if (firstWord({"accept", "yes", "apply"}) && !pending_.empty()) return cmdAnswer(low, true);
  if (firstWord({"reject", "decline", "no ", "skip", "ignore"}) && !pending_.empty()) return cmdAnswer(low, false);

  const bool mentionsAudio = !file.empty() || !lastFile_.empty();
  if (firstWord({"master", "auto-master", "automaster"}) || has(low, "master this") || has(low, "master it")) {
    return cmdMaster(rest, file, nameAfter(rest, {"like", "toward", "towards", "to match", "against", "with"}));
  }
  if (firstWord({"learn", "study", "add to my profile", "add this to"}) || has(low, "learn from") || has(low, "this is my mix") || has(low, "this is my master")) {
    std::string prof = nameAfter(rest, {"as", "into", "for"});
    return cmdLearn(file, prof);
  }
  if (!has(low, "chord") && !has(low, "progression") && (firstWord({"advise", "suggest", "compare", "what should i change", "how does"}) || has(low, "what should i change") || has(low, "compare "))) {
    return cmdAdvise(file, nameAfter(rest, {"like", "to", "with", "against", "toward", "towards"}));
  }
  if (firstWord({"analyze", "analyse", "check", "listen to", "measure"}) || ((has(low, "what key") || has(low, "what tempo") || has(low, "how loud") || has(low, "bpm of")) && mentionsAudio)) {
    return cmdAnalyze(file);
  }

  if (session_) {
    std::string reply;
    if (session_(text, reply)) return {reply};
  }
  if (model_) {
    history_.push_back({"user", text});
    if (history_.size() > 12) history_.erase(history_.begin(), history_.end() - 12);
    std::vector<ChatMessage> chat{{"system", contextForModel()}};
    chat.insert(chat.end(), history_.begin(), history_.end());
    std::string out, err;
    if (model_->complete(chat, out, err)) {
      history_.push_back({"assistant", out});
      Reply r{out};
      r.usedModel = true;
      return r;
    }
    history_.pop_back();
    return {"My language model is not reachable (" + err + "), but I can still analyze, learn, advise and master. Say \"help\" to see how."};
  }
  return {"I did not catch a command in that. Say \"help\" for what I can do on my own. For open conversation, connect a local language model in the settings."};
}

}  // namespace wezt::ai
