# The local assistant

Everything here runs on your computer. Nothing is uploaded, and no cloud service is contacted.

## What it is, honestly

It is **not** a neural network that was trained on other producers' records, and it does not
silently watch you. It is three plain, inspectable parts:

1. **Measurement** (`src/ai/features.*`): loudness after ITU-R BS.1770 (integrated, short-term,
   true peak, loudness range), RMS and crest factor, stereo width and correlation, an
   octave-band tonal balance, spectral centroid, tempo (onset autocorrelation) and key
   (Krumhansl-Kessler profiles on a chroma vector).
2. **Style profiles** (`src/ai/profile.*`): the running mean and spread of those measurements over
   the tracks you give it. The profile `me` is built from your own mixes and masters. A profile
   named after a reference ("warm-pop", "that-club-record") is built from the reference tracks
   you supply. Profiles are plain text files you can read, copy and delete.
3. **Advice and action** (`src/ai/advisor.*`, `master.*`, `src/dsp/*`): compares a track with a
   profile, says what differs ("lows 3 dB above your usual"), and can master a file toward the
   profile or a delivery target using a built-in EQ, compressor and look-ahead limiter. It
   re-measures the result so you can see it moved closer.

**Learning:** every suggestion can be accepted or rejected. The assistant keeps a per-band
preference bias and uses it next time, so it stops suggesting what you keep refusing. It also
stores short notes ("I prefer vocals dry", "never go above -9 LUFS") in `notes.txt`.

**Copying other producers:** you can only study audio you provide. The assistant extracts
*measurements* (tonal balance, loudness, dynamics, width), not the recordings themselves, and
matches your track's measurements toward them. It does not reproduce melodies, samples or
performances. Using commercial recordings as references on your own machine is a common studio
practice; sharing them is your responsibility.

## Talking to it

Open **Assistant** in the Edit window. Examples:

```
analyze "mix.wav"
learn from "master1.wav"                       (adds to your profile "me")
learn from "ref.wav" as warm-pop               (builds a reference profile)
show profile warm-pop
advise "mix.wav" like warm-pop     then:  accept 1 / accept all / reject 2
master "mix.wav" to streaming                   (also: apple, cd, club, broadcast, or a profile)
remember that I like a bright top end
what do you remember
```

Session commands (play, stop, add a track, strip silence, undo, markers...) are handled by the
app's own parser, never by a language model.

## Optional local language model

For open conversation you can point Wezt at a model server running on the same computer (for
example llama.cpp's `llama-server` or Ollama, both of which expose an OpenAI-style
`/v1/chat/completions`). Wezt only connects to `127.0.0.1`, `localhost` or `::1`; any other host
is refused **before a socket is opened**. The model receives your notes and a short profile
summary as context and can only return text. It cannot run commands. No model is bundled; without
one, the assistant still does everything listed above.

## Delivery targets used by the mastering presets

Defaults follow widely published streaming guidance: about -14 LUFS integrated for Spotify and
YouTube, -16 for Apple Music, with a true-peak ceiling of -1 dBTP. These change over time and
platforms normalise loudness anyway, so treat them as starting points and check each platform's
current guidance before delivery.

## Limits

- Tempo and key estimates are good on clear material and unreliable on sparse, rubato or heavily
  modulating music; the assistant reports a confidence.
- Mastering is deliberately conservative (EQ moves are clamped, limiter ceiling honoured). It is a
  fast, repeatable starting point, not a replacement for ears and monitoring.
- Stem separation (the document's "Stem Separator") is **not built**: it needs a trained model
  that we cannot ship or train here. The assistant interface is the place a local separator
  would plug in.
