#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "io/wav.h"
#include "mcp/server.h"
#include "voice/singing.h"
#include "util/json.h"

using wezt::json::Value;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static Value rpc(wezt::mcp::Server& s, const std::string& line) {
  Value v; const std::string r = s.handle(line);
  if (r.empty()) return Value::null();
  std::string e; CHECK(wezt::json::parse(r, v, &e));
  return v;
}
static std::string call(wezt::mcp::Server& s, const std::string& tool, const std::string& args, bool* isErr = nullptr) {
  Value v = rpc(s, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool + "\",\"arguments\":" + args + "}}");
  const Value* r = v.get("result");
  if (!r) return "<no result>";
  if (isErr) *isErr = r->getBool("isError");
  const Value* c = r->get("content");
  return c && !c->items().empty() ? c->items()[0].getString("text") : "";
}

int main() {
  namespace fs = std::filesystem;
  const std::string dir = (fs::temp_directory_path() / "wezt_mcp_test").string();
  fs::remove_all(dir);
  wezt::mcp::Server s(dir, 22050.0);

  Value v = rpc(s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}");
  CHECK(v.get("result") && v.get("result")->get("serverInfo"));
  CHECK(s.handle("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}").empty());
  v = rpc(s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
  CHECK(v.get("result") && v.get("result")->get("tools")->items().size() >= 15);
  v = rpc(s, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"nope\"}");
  CHECK(v.get("error") != nullptr);
  v = rpc(s, "not json");
  CHECK(v.get("error") != nullptr);

  bool err = false;
  std::string t = call(s, "suggest_chords", "{\"key\":\"A minor\",\"style\":\"pop\",\"mood\":\"sad\",\"bars\":4}", &err);
  CHECK(!err && t.find('|') != std::string::npos);
  call(s, "suggest_chords", "{\"key\":\"H# lydian9\"}", &err); CHECK(err);

  t = call(s, "compose_song", "{\"title\":\"Test\",\"key\":\"A minor\",\"style\":\"lofi\",\"mood\":\"chill\",\"bpm\":90,\"seed\":3,"
                              "\"sections\":[{\"name\":\"Verse\",\"kind\":\"verse\",\"bars\":4},{\"name\":\"Chorus\",\"kind\":\"chorus\",\"bars\":4}]}", &err);
  CHECK(!err);
  t = call(s, "get_state", "{}"); CHECK(t.find("Drums") != std::string::npos && t.find("Bass") != std::string::npos);
  t = call(s, "analyze_mix", "{}", &err); CHECK(!err);
  t = call(s, "sidechain", "{\"key_track\":\"Drums\",\"target_track\":\"Bass\"}", &err); CHECK(!err);
  t = call(s, "add_part", "{\"kind\":\"pad\",\"mood\":\"dreamy\",\"bars\":4,\"name\":\"Dreamy Pad\"}", &err); CHECK(!err);
  t = call(s, "set_track", "{\"track\":\"Dreamy Pad\",\"gain_db\":-6}", &err); CHECK(!err);
  t = call(s, "master", "{\"target\":\"streaming\",\"output\":\"out/master.wav\"}", &err); CHECK(!err);
  CHECK(fs::exists(fs::path(dir) / "out" / "master.wav"));
  call(s, "master", "{\"output\":\"../escape.wav\"}", &err); CHECK(err);
  call(s, "master", "{\"output\":\"/etc/escape.wav\"}", &err); CHECK(err);
  call(s, "add_audio_file", "{\"path\":\"/etc/passwd\"}", &err); CHECK(err);
  t = call(s, "stretch_audio", "{\"track\":\"Bass\",\"ratio\":1.25,\"mode\":\"polyphonic\"}", &err); CHECK(!err);
  call(s, "stretch_audio", "{\"track\":\"Bass\",\"ratio\":9}", &err); CHECK(err);
  call(s, "stretch_audio", "{\"track\":\"Nope\",\"ratio\":1.2}", &err); CHECK(err);
  t = call(s, "pitch_audio", "{\"track\":\"Keys\",\"semitones\":2,\"mode\":\"rhythmic\"}", &err); CHECK(!err);
  call(s, "train_voice_model", "{\"tracks\":[\"Melody\"],\"name\":\"x\",\"consent\":false}", &err); CHECK(err);   // refuses without consent
  call(s, "train_voice_model", "{\"tracks\":[\"Melody\"],\"name\":\"x\",\"consent\":true}", &err);    // a synth melody is long enough to train on (it is not a voice, but the model does not know)
  t = call(s, "list_voice_models", "{}", &err); CHECK(!err && (t.find("x") != std::string::npos || t.find("No voice models") != std::string::npos));
  call(s, "convert_voice", "{\"track\":\"Melody\",\"model\":\"missing\"}", &err); CHECK(err);
  call(s, "make_choir", "{\"track\":\"Melody\",\"key\":\"A minor\"}", &err);   // synth melody: either result is fine, it must not crash
  call(s, "fit_vocal", "{\"track\":\"Nope\"}", &err); CHECK(err);
  {  // the voice-bank path end to end: held vowels in the work folder -> tracks -> bank -> sung line
    namespace v = wezt::voice;
    for (const char* vw : {"ah", "ee", "oo"}) {
      v::SingParams sp; sp.bpm = 120; sp.sampleRate = 22050; sp.humanize = 0; sp.vibratoCents = 0; sp.fitRange = false; sp.voice = v::VoiceType::Tenor; sp.room = 0;
      wezt::music::MidiClip c; wezt::music::Note n; n.start = 0; n.length = 5; n.pitch = 57; n.velocity = 90; c.notes.push_back(n);
      auto r = v::sing(std::string("la[") + vw + "]", c, sp); CHECK(r.ok);
      wezt::io::writeWavFloat((fs::path(dir) / (std::string("take_") + vw + ".wav")).string(), r.audio, 22050);
      call(s, "add_audio_file", std::string("{\"path\":\"") + (fs::path(dir) / (std::string("take_") + vw + ".wav")).generic_string() + "\",\"track_name\":\"Take " + vw + "\"}", &err); CHECK(!err);
    }
    t = call(s, "build_voice_bank", "{\"recordings\":[{\"vowel\":\"ah\",\"track\":\"Take ah\"},{\"vowel\":\"ee\",\"track\":\"Take ee\"},{\"vowel\":\"oo\",\"track\":\"Take oo\"}],\"name\":\"me\",\"owner\":\"tester\",\"consent\":true}", &err);
    CHECK(!err); if (err) std::printf("BANK: %s\n", t.c_str());
    CHECK(fs::exists(fs::path(dir) / "audio" / "voices" / "me.wvb"));
    {  // a consonant: "sah" recorded on a track; the bank gets an "s" and singing says so
      v::SingParams sp; sp.bpm = 120; sp.sampleRate = 22050; sp.humanize = 0; sp.vibratoCents = 0; sp.fitRange = false; sp.voice = v::VoiceType::Tenor; sp.room = 0;
      wezt::music::MidiClip c; wezt::music::Note n; n.start = 0.2; n.length = 5; n.pitch = 57; n.velocity = 90; c.notes.push_back(n);
      auto r = v::sing("sa[ah]", c, sp); CHECK(r.ok);
      const std::string f = (fs::path(dir) / "take_sah.wav").generic_string();   // forward slashes: the path goes into JSON
      wezt::io::writeWavFloat(f, r.audio, 22050);
      call(s, "add_audio_file", "{\"path\":\"" + f + "\",\"track_name\":\"Take sah\"}", &err); CHECK(!err);
      t = call(s, "build_voice_bank", "{\"recordings\":[{\"vowel\":\"ah\",\"track\":\"Take ah\"},{\"vowel\":\"s\",\"track\":\"Take sah\"}],\"name\":\"me2\",\"owner\":\"tester\",\"consent\":true}", &err);
      CHECK(!err); if (err) std::printf("BANK2: %s\n", t.c_str());
      CHECK(t.find("1 consonant") != std::string::npos);
      t = call(s, "sing_lyrics", "{\"lyrics\":\"sa sa\",\"voice_bank\":\"me2\"}", &err); CHECK(!err);
      CHECK(t.find("Consonants: 2 from your recordings") != std::string::npos || t.find("Consonants: 1 from your recordings") != std::string::npos);
      // a recording with no consonant in front of the vowel is refused for a voiceless consonant
      call(s, "build_voice_bank", "{\"recordings\":[{\"vowel\":\"s\",\"track\":\"Take ah\"}],\"name\":\"bad\",\"consent\":true}", &err); CHECK(err);
    }
    {  // one recording of several sounds: split it, then name the takes
      std::vector<float> rec(static_cast<std::size_t>(22050 * 0.4), 0.0f);
      for (const char* vw : {"ah", "ee"}) {
        v::SingParams sp; sp.bpm = 120; sp.sampleRate = 22050; sp.humanize = 0; sp.vibratoCents = 0; sp.fitRange = false; sp.voice = v::VoiceType::Tenor; sp.room = 0;
        wezt::music::MidiClip c; wezt::music::Note n; n.start = 0; n.length = 3; n.pitch = 55; n.velocity = 90; c.notes.push_back(n);
        auto r = v::sing(std::string("la[") + vw + "]", c, sp); CHECK(r.ok);
        rec.insert(rec.end(), r.audio.channel(0) + 2205, r.audio.channel(0) + 2205 + 22050); rec.insert(rec.end(), static_cast<std::size_t>(22050 * 0.6), 0.0f);
      }
      wezt::AudioBuffer ab(1, rec.size()); std::copy(rec.begin(), rec.end(), ab.channel(0));
      wezt::io::writeWavFloat((fs::path(dir) / "takes.wav").string(), ab, 22050);
      t = call(s, "split_voice_takes", "{\"file\":\"takes.wav\"}", &err); CHECK(!err && t.find("2 takes") != std::string::npos); if (err || t.find("2 takes") == std::string::npos) std::printf("SPLIT: %s\n", t.c_str());
      call(s, "build_voice_bank_from_takes", "{\"file\":\"takes.wav\",\"use\":\"0=ah,1=ee\",\"name\":\"tk\",\"consent\":false}", &err); CHECK(err);            // no consent
      call(s, "build_voice_bank_from_takes", "{\"file\":\"takes.wav\",\"use\":\"\",\"name\":\"tk\",\"consent\":true}", &err); CHECK(err);                       // nothing named
      const std::string outside = (fs::temp_directory_path() / "wezt_outside_takes.wav").generic_string();
      fs::copy_file(fs::path(dir) / "takes.wav", outside, fs::copy_options::overwrite_existing);
      call(s, "build_voice_bank_from_takes", "{\"file\":\"" + outside + "\",\"use\":\"0=ah\",\"name\":\"tk\",\"consent\":true}", &err); CHECK(err);   // outside the work folder
      call(s, "split_voice_takes", "{\"file\":\"" + outside + "\"}", &err); CHECK(err);
      fs::remove(outside);
      call(s, "build_voice_bank_from_takes", "{\"file\":\"takes.wav\",\"use\":\"7=ah,0=zz\",\"name\":\"tk\",\"consent\":true}", &err); CHECK(err);               // nothing usable
      t = call(s, "build_voice_bank_from_takes", "{\"file\":\"takes.wav\",\"use\":\"0=ah,1=ee\",\"name\":\"tk\",\"owner\":\"tester\",\"consent\":true}", &err);
      CHECK(!err && t.find("2 vowel") != std::string::npos); if (err) std::printf("TAKES: %s\n", t.c_str());
      CHECK(fs::exists(fs::path(dir) / "audio" / "voices" / "tk.wvb"));
      t = call(s, "sing_lyrics", "{\"lyrics\":\"la lee\",\"voice_bank\":\"tk\"}", &err); CHECK(!err);
    }
    t = call(s, "list_voice_models", "{}", &err); CHECK(t.find("me") != std::string::npos);
    t = call(s, "sing_lyrics", "{\"lyrics\":\"la lee loo\",\"voice_bank\":\"me\"}", &err); CHECK(!err); if (err) std::printf("BANKSING: %s\n", t.c_str());
    CHECK(t.find("voice bank") != std::string::npos);
  }
  t = call(s, "save_session", "{\"name\":\"song\"}", &err); CHECK(!err && fs::exists(fs::path(dir) / "song.wezt"));
  t = call(s, "ask_tutor", "{\"question\":\"how do I sidechain?\"}", &err); CHECK(!err && !t.empty());
  t = call(s, "beat_detective", "{\"track\":\"Drums\",\"bpm\":90,\"grid\":0.25,\"strength\":0.8}", &err);   // synthetic drums are already on the grid; either outcome must be handled
  call(s, "beat_detective", "{\"track\":\"Nope\"}", &err); CHECK(err);
  {  // MIDI editing works on files inside the work folder only
    t = call(s, "edit_midi", "{\"file\":\"Test.mid\",\"operation\":\"describe\"}", &err);
    CHECK(!err && t.find("Bar 1") != std::string::npos);
    t = call(s, "edit_midi", "{\"file\":\"Test.mid\",\"operation\":\"quantize\",\"a\":0.5,\"output\":\"q.mid\"}", &err);
    CHECK(!err && fs::exists(fs::path(dir) / "q.mid"));
    call(s, "edit_midi", "{\"file\":\"/etc/passwd\",\"operation\":\"describe\"}", &err); CHECK(err);
    call(s, "edit_midi", "{\"file\":\"../x.mid\",\"operation\":\"describe\"}", &err); CHECK(err);
  }
  t = call(s, "sing_lyrics", "{\"lyrics\":\"la la la la\",\"voice\":\"soprano\"}", &err); CHECK(!err); if (err) std::printf("SING: %s\n", t.c_str());
  t = call(s, "get_state", "{}"); CHECK(t.find("Vocal (sung)") != std::string::npos);
  call(s, "sing_lyrics", "{\"lyrics\":\"la\",\"voice\":\"robot\"}", &err); CHECK(err);
  call(s, "sing_lyrics", "{\"lyrics\":\"\"}", &err); CHECK(err);
  t = call(s, "sing_lyrics", "{\"lyrics\":\"la la la la\",\"character\":\"gritty_rock\"}", &err); CHECK(!err); if (err) std::printf("CHARACTER: %s\n", t.c_str());
  call(s, "sing_lyrics", "{\"lyrics\":\"la\",\"character\":\"robot king\"}", &err); CHECK(err);
  call(s, "sing_lyrics", "{\"lyrics\":\"la la\",\"voice_model\":\"nobody\"}", &err); CHECK(err);
  call(s, "sing_lyrics", "{\"lyrics\":\"la la\",\"voice_bank\":\"nobody\"}", &err); CHECK(err);
  call(s, "build_voice_bank", "{\"recordings\":[{\"vowel\":\"ah\",\"track\":\"Melody\"}],\"name\":\"b\",\"consent\":false}", &err); CHECK(err);       // refuses without consent
  call(s, "build_voice_bank", "{\"recordings\":[{\"vowel\":\"zz\",\"track\":\"Melody\"}],\"name\":\"b\",\"consent\":true}", &err); CHECK(err);        // unknown vowel
  call(s, "build_voice_bank", "{\"recordings\":[{\"vowel\":\"ah\",\"track\":\"No such\"}],\"name\":\"b\",\"consent\":true}", &err); CHECK(err);       // unknown track
  call(s, "build_voice_bank", "{\"recordings\":[],\"name\":\"b\",\"consent\":true}", &err); CHECK(err);
  t = call(s, "sing_lyrics", "{\"lyrics\":\"la la la la\",\"voice_model\":\"x\"}", &err); if (!err) CHECK(t.find("voice model") != std::string::npos);   // the model trained above, if it exists
  call(s, "bogus", "{}", &err); CHECK(err);
  call(s, "new_session", "{}");
  t = call(s, "get_state", "{}"); CHECK(t.find("Tracks: 0") != std::string::npos);

  if (fails) { std::printf("%d failures\n", fails); return 1; }
  std::printf("mcp tests passed\n");
  return 0;
}
