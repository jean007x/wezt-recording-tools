#include "ai/tutor.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace wezt::ai {
namespace {

std::vector<std::string> tokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (cur.size() > 2) {
      if (cur.size() > 4 && cur.compare(cur.size() - 3, 3, "ing") == 0) cur.resize(cur.size() - 3);
      else if (cur.size() > 3 && cur.back() == 's') cur.pop_back();
      out.push_back(cur);
    }
    cur.clear();
  };
  for (char c : s) { if (std::isalnum(static_cast<unsigned char>(c))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); else flush(); }
  flush();
  return out;
}

}  // namespace

Tutor::Tutor() {
  topics_ = {
      {"sidechain", "Sidechain compression (make the bass duck under the kick)",
       {"sidechain", "duck", "pump", "kick", "bass", "pumping", "compression"},
       {"Say \"set up sidechain between the kick and bass\" to the assistant: it finds the two tracks by name, lets the kick's low end drive the reduction and replaces the bass clips with processed copies.",
        "Or by hand with a compressor plug-in that has a sidechain input: put it on the bass, choose the kick track as the sidechain source, a fast attack (1-10 ms) and a release near 100-200 ms.",
        "Adjust until the bass dips just as the kick hits and recovers before the next beat."},
       "Ducking 3-6 dB is usually enough; deeper reads as an effect, not as clean low end."},
      {"eq-clash", "Fix frequencies that fight (muddy mix)",
       {"muddy", "mud", "clash", "masking", "frequency", "fight", "eq", "clear", "mix", "vocal", "guitar", "conflict"},
       {"Say \"find frequency clashes\". The assistant measures every track and lists where two tracks are strong in the same band.",
        "Each fix cuts the less important track (the one whose role is lower: vocals and leads win, pads lose) by 1.5-4 dB at the clashing band.",
        "Say \"apply the fixes\" to accept all of them, or \"apply 2\" for one. Listen with and without; undo if it sounds worse."},
       "Cutting to make room is usually better than boosting. Low-end energy on tracks other than kick and bass is also worth a high-pass."},
      {"master", "Master a finished mix",
       {"master", "mastering", "loud", "loudness", "lufs", "streaming", "limiter", "one-click", "final"},
       {"Bounce your mix, or say \"master mix.wav to streaming\" with a WAV file.",
        "The assistant measures loudness, tonal balance and dynamics, applies gentle EQ and compression where needed, then a limiter, and re-measures the result.",
        "Targets: streaming about -14 LUFS, apple about -16, cd about -9, club about -8, broadcast -23; true peak is kept at or below the target's ceiling."},
       "Leave 1-3 dB of headroom in your mix (peaks around -3 dBFS) before mastering."},
      {"generate-chords", "Get chord ideas",
       {"chord", "progression", "harmony", "idea", "generate", "key", "mood"},
       {"Ask: \"suggest a sad chord progression in A minor, lofi style, 8 bars\".",
        "The result is a diatonic progression that starts and ends on the tonic, with richer chords (7ths, 9ths) for jazz, lofi and rnb styles or dreamy moods.",
        "The chords are saved as a track you can hear; the MIDI file can be dragged into any instrument track."},
       "Run it with different seeds (\"another one\") to hear more variations."},
      {"melody", "Write a melody over chords",
       {"melody", "tune", "lead", "topline", "hook"},
       {"Generate or write your chords first.", "Ask for a melody: \"add a melody in C major, happy\". Downbeats land on chord tones and the last note resolves to the tonic.",
        "Edit the result in your MIDI editor; use it as a starting point rather than a finished hook."},
       "Sparser melodies (lower density) usually sit better under a vocal."},
      {"hum", "Turn humming or beatboxing into notes",
       {"hum", "humming", "sing", "beatbox", "audio-to-midi", "midi", "convert", "whistle", "tap"},
       {"Record yourself on an audio track, one voice, one note at a time, in a quiet room.",
        "Say \"turn the hum track into MIDI with a pad\" or \"turn the beatbox track into drums\".",
        "A new track plays the notes with a built-in instrument and a .mid file is saved; send the MIDI to any instrument."},
       "Sing clearly and slightly loud; vibrato is fine, chords and fast runs are not detected."},
      {"stems", "Split a song into drums, bass, vocals and other",
       {"stem", "split", "separate", "isolate", "vocal", "acapella", "instrumental", "remix", "sample"},
       {"Put the song on a track, then say \"split the song track into stems\".",
        "If Demucs is installed on your computer it is used (best quality). Otherwise the built-in DSP splitter is used: drums and bass come out usefully, vocals only when they are centred.",
        "Four new tracks appear: drums, bass, vocals, other."},
       "Only split music you have the right to use."},
      {"group", "Group tracks", {"group", "link", "drum", "tracks", "together"},
       {"Say \"group the drum tracks\" (or the vocals, bass, ...). Tracks are matched by words in their names.", "Or select tracks and press Ctrl+G.", "Mute, solo and record-arm then apply to the whole group."},
       "Name tracks clearly (Kick, Snare, Bass DI) so grouping by name works."},
      {"strip-silence", "Remove silence from a clip", {"silence", "strip", "gap", "quiet", "clean", "noise"},
       {"Select the clip.", "Press Ctrl+U (Strip Silence) or say \"strip silence\".", "The clip is split where the level stays under the threshold (-50 dB by default), keeping a little padding and fades."},
       "Use it on spoken or tracked parts before editing; it leaves the gaps so everything stays in place."},
      {"undo", "Undo and redo", {"undo", "redo", "mistake", "history", "revert"},
       {"Press Ctrl+Z to undo, Ctrl+Shift+Z to redo.", "A drag counts as one step."}, "Undo covers clip edits. Saved files are not changed until you save."},
      {"markers", "Markers and memory locations", {"marker", "memory", "location", "locate", "verse", "chorus", "navigate", "cue"},
       {"Open Memory Locations from the Window menu.", "Press Add at cursor, then name the marker.", "Go jumps there; Previous and Next step through them."},
       "Generated songs get one marker per section automatically."},
      {"gain-staging", "Gain staging (levels)", {"gain", "level", "headroom", "clip", "distort", "loud", "volume", "staging"},
       {"Keep every track's peaks around -12 to -6 dBFS before the faders.", "Use clip gain to even out loud and quiet phrases before compression.", "Keep the master bus peaking below about -3 dBFS so mastering has room."},
       "Digital clipping on a track cannot be fixed later; turn it down at the source."},
      {"compression", "Use a compressor", {"compress", "compressor", "dynamics", "punch", "squash", "ratio", "threshold", "attack", "release"},
       {"Set a ratio of 2:1 to 4:1.", "Lower the threshold until the loudest notes drop 3-6 dB.", "Fast attack calms transients, slow attack keeps punch; release should follow the tempo of the part.", "Add make-up gain until level matches the bypassed sound, then compare."},
       "If you cannot hear a difference with it bypassed, back off."},
      {"reverb-delay", "Reverb and delay as sends", {"reverb", "delay", "echo", "space", "send", "aux", "bus", "effect"},
       {"Add a bus and put a reverb on it, set to 100% wet.", "On each track, raise a send to that bus.", "High-pass and low-pass the reverb return so it stays out of the low end."},
       "One reverb shared by several tracks glues them together."},
      {"record", "Record a track", {"record", "recording", "arm", "input", "microphone", "mic", "take"},
       {"Choose your audio device in the engine settings.", "Add an audio track, set its input and press the record-arm button.", "Press Ctrl+Space (or F12) to record, Space to stop. The take is saved as a WAV file."},
       "Check the input level while you perform; peaks near -6 dBFS leave room."},
      {"sampler", "Chop a sample with the Sampler", {"sampler", "slice", "chop", "sample", "transient", "keys", "zone"},
       {"Open the Sampler from the bottom tabs.", "Drop in a recording and pick Slice mode.", "Choose Transient mode and raise Sensitivity until each hit has a marker; slices are mapped to consecutive keys from the start key."},
       "Use Follow Tempo only after the slices are right."},
      {"immersive", "Mix in 3D (immersive)", {"immersive", "atmos", "3d", "surround", "object", "binaural", "adm"},
       {"Open the Immersive panner from the bottom tabs.", "Choose a speaker layout (up to 9.1.6) and place objects.", "Monitor in binaural on headphones; export an ADM BWF file for delivery."},
       "Wezt writes the open ADM format; Dolby's own renderer is not part of it."},
      {"save-export", "Save and export", {"save", "export", "bounce", "wav", "midi", "file", "session"},
       {"Ctrl+S saves the session.", "Bounce Mix renders the whole mix to a WAV file faster than real time.", "Generated songs also save a standard .mid file next to the audio."}, ""},
  };
}

const TutorTopic* Tutor::find(const std::string& question) const {
  const auto q = tokens(question);
  if (q.empty()) return nullptr;
  const TutorTopic* best = nullptr;
  double bestScore = 0;
  for (auto& t : topics_) {
    std::set<std::string> kw;
    for (auto& k : t.keywords) for (auto& tk : tokens(k)) kw.insert(tk);
    for (auto& tk : tokens(t.title)) kw.insert(tk);
    double score = 0;
    for (auto& w : q) if (kw.count(w)) score += 1.0;
    // an exact topic id or its first keyword is a strong hit
    for (auto& w : q) if (w == t.id || (!t.keywords.empty() && !tokens(t.keywords[0]).empty() && w == tokens(t.keywords[0])[0])) score += 1.0;
    if (score > bestScore) { bestScore = score; best = &t; }
  }
  return bestScore >= 1.0 ? best : nullptr;
}

std::string Tutor::answer(const std::string& question) const {
  std::ostringstream o;
  if (const TutorTopic* t = find(question)) {
    o << t->title << "\n";
    for (std::size_t i = 0; i < t->steps.size(); ++i) o << (i + 1) << ". " << t->steps[i] << "\n";
    if (!t->tip.empty()) o << "Tip: " << t->tip << "\n";
    return o.str();
  }
  o << "I do not have a guide for that. I can walk you through:";
  for (auto& t : topics_) o << "\n- " << t.title;
  return o.str();
}

}  // namespace wezt::ai
