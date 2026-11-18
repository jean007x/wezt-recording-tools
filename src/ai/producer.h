#pragma once
// The "producer" actions behind the assistant and the Claude connector: generate a song or a single
// part, split a track into stems, turn a hummed or beatboxed track into notes, find frequency clashes,
// duck one track under another, and master the result. They work on a Session and write the audio
// they create into a work folder, so every result is an ordinary track with ordinary clips.
#include <string>
#include <vector>

#include "ai/mix_assistant.h"
#include "music/song.h"
#include "session/session.h"

namespace wezt::ai {

struct Outcome {
  bool ok = false;
  std::string message;     // what happened, or why not; written for people
};

class Producer {
 public:
  // `workDir` receives rendered audio and MIDI files; it is created on demand.
  Producer(session::Session& session, std::string workDir) : s_(session), dir_(std::move(workDir)) {}

  // Track names containing a role word ("drums", "kick", "bass", "vocal" ...). Case-insensitive.
  std::vector<session::TrackId> tracksByRole(Role role) const;
  session::TrackId trackByName(const std::string& name) const;   // exact (case-insensitive) first, then substring; -1 if none

  // Composes a whole song and adds one track per part after the existing tracks. Returns the chart.
  Outcome generateSong(const music::SongSpec& spec);
  // One part only, e.g. a dreamy pad: kind is drums, bass, keys, pad or melody.
  Outcome addPart(const std::string& kind, const music::Key& key, const std::string& style, const std::string& mood, int bars, std::uint64_t seed, double bpm, const std::string& trackName = {});

  // Splits the first clip of a track into drums, bass, vocals and other (a neural separator if one
  // is installed and `preferNeural`, otherwise the built-in DSP one) and adds four tracks.
  Outcome separateStems(const std::string& trackName, bool preferNeural = true);
  // Hum or beatbox: finds the notes in the first clip of the track, saves them as a MIDI file and adds
  // a track that plays them with a built-in instrument ("keys", "bass", "pad", "lead", "pluck", or "drums" for beatboxing).
  Outcome audioToMidi(const std::string& trackName, const std::string& instrument, double bpm, bool beatbox);

  // Measures every track and reports clashes. The proposed fixes are kept so `applyMixFixes` can use them.
  // Your own recorded vocal, retuned and retimed to a melody (the last generated one, or a .mid file).
  // Only your own recording is used; no voice model is created.
  Outcome fitVocal(const std::string& vocalTrack, const std::string& midiFile = {}, double bpm = 0, bool harmony = true);

  // One take of your voice becomes a stereo choir (new track).
  Outcome makeChoir(const std::string& vocalTrack, const music::Key& key, int voices = 4);

  // Elastic Audio on every clip of a track. Give `ratio` (new/old length), or `targetBpm` (the
  // source tempo is measured unless `sourceBpm` > 0). mode: polyphonic, rhythmic, monophonic, varispeed.
  Outcome stretchTrack(const std::string& track, double ratio, double sourceBpm, double targetBpm, const std::string& mode);
  Outcome pitchTrack(const std::string& track, double semitones, const std::string& mode);

  // Voice models (see voice/voice_model.h): trained from scratch on recordings on this computer.
  // `consent` must be true: the voice is the user's own, or the speaker agreed.
  Outcome trainVoice(const std::vector<std::string>& tracks, const std::string& modelName, const std::string& owner, bool consent);
  // Re-voices a track (a vocal sung by anyone, or by an AI) into the trained voice; adds a new track.
  Outcome convertVoice(const std::string& track, const std::string& modelName, double semitones = 0);
  std::vector<std::string> voiceModels() const;
  std::vector<std::string> voiceBanks() const;
  // vowel + track pairs: each track holds one held, dry, steady note of that vowel (about 1.5 s or longer); consent as for voice models
  // One recording of many separate sounds (vowels, syllables, hisses, letter names). splitVoiceTakes lists the takes with
  // measurements; buildVoiceBankFromTakes names them (use = "idx=name,idx=name", see voice/voice_takes.h) and saves a bank.
  Outcome splitVoiceTakes(const std::string& wavFile);
  Outcome buildVoiceBankFromTakes(const std::string& wavFile, const std::string& use, const std::string& name, const std::string& owner, bool consent, bool derive, bool shortVowels);
  Outcome buildVoiceBank(const std::vector<std::pair<std::string, std::string>>& recordings, const std::string& name, const std::string& owner, bool consent);

  // Singing synthesis from lyrics (formant synthesizer, no trained model) over the last generated melody or a .mid file.
  // humanize 0..1 (how much pitch, timing, loudness and vibrato wander like a real singer), room 0..1 (a small room around the voice).
  Outcome singLyrics(const std::string& lyrics, const std::string& voice = "alto", const std::string& midiFile = {}, double bpm = 0, int choirVoices = 1, double humanize = 0.7, double room = 0.15, const std::string& character = {}, const std::string& voiceModel = {}, const std::string& voiceBank = {});

  // Beat Detective: cuts the track's clips at their hits and moves the slices toward a grid.
  Outcome beatDetective(const std::string& track, double bpm, double gridBeats, double strength, double swing = 0);
  // Piano-roll style editing of a .mid file; writes a new .mid and can add a track that plays it.
  // ops: describe, quantize (a = grid beats, b = strength), transpose (a = semitones), humanize (a = timing beats, b = velocity range),
  // legato, velocity (a = factor, b = offset), crop (a = from beat, b = to beat), snap_scale.
  Outcome editMidi(const std::string& inFile, const std::string& outFile, const std::string& op, double a, double b, const music::Key& key, const std::string& instrument, double bpm);

  Outcome analyzeMix();
  Outcome applyMixFixes(const std::vector<int>& indices = {});   // empty = all
  // Bass (or any track) ducks under the kick (or any track). The target's clips are replaced by processed copies.
  Outcome sidechain(const std::string& keyTrack, const std::string& targetTrack, const SidechainParams& p = {});

  // Bounces the session and masters it toward a delivery target ("streaming", "apple", "cd", "club", "broadcast").
  Outcome masterSession(const std::string& target, const std::string& outPath);

  const std::string& workDir() const { return dir_; }

 private:
  std::string path(const std::string& file) const;
  bool addStemTrack(const std::string& name, const AudioBuffer& audio, float gainDb, float pan, std::string& err);
  bool firstClipAudio(session::TrackId t, AudioBuffer& out, double& rate, std::int64_t& start) const;

  session::Session& s_;
  std::string dir_;
  MixReport lastReport_;
  music::MidiClip lastMelody_;
  double lastBpm_ = 0;
  music::Key lastKey_;
};

}  // namespace wezt::ai
