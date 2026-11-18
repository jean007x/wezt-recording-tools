# Singing with your own recorded voice

The synthesizer can only imitate a voice. The voice bank uses the real thing: your recordings of held vowels, moved to the sung pitch.

## Record
One track per vowel, each with one steady, dry (no reverb), clearly pitched note of 1.5 s or longer, at a pitch comfortable for you. Needed: ah, ee, oh, oo, eh. Optional: ae, ih, uh, er (missing vowels use a similar one, or the synthesizer). Record a vowel at two pitches (say a fourth apart) to cover a wider range.

## Consonants (optional)
Record yourself saying a consonant followed by a held \"ah\" (\"sah\", \"tah\", \"mah\"), one per track. The part before the vowel is cut out and kept at its real loudness next to the vowel, and the singer plays it in place of the synthesized consonant (its length follows your recording; it is sped up slightly, at most 1.6x, if the slot is short). Consonants: p t k b d g f v s z sh ch th h m n l r w y j ng; c and q use k, x uses s. Any consonant you skip is synthesized. With `build_voice_bank` pass the consonant as the `vowel` field. Voiceless ones (p t k f s sh ch th h) are checked: if I cannot hear noise before your voice starts, the recording is refused. Voiced ones (m n l ...) are used at the pitch you recorded them, not retuned to the melody, so keep them short.

## In the app
View > My Voice Bank: choose the track for each vowel, tick the box that the voice is yours, press Build. Then in the Piano Roll pick the bank in the "vowels" menu (and, if you like, a voice character) and press Sing it. Record the vowels as ordinary audio tracks first (arm a track, hold one steady note).

## Build and sing (Claude connector / assistant)
1. `build_voice_bank` with `recordings: [{"vowel":"ah","track":"Take ah"}, ...]`, `name`, `owner`, `consent: true` (only for your own voice or a speaker who agreed).
2. `sing_lyrics` with `voice_bank: "<name>"`. Pitch, vibrato, drift, timing and loudness come from the humanized singer; the vowels come from your audio (PSOLA re-pitching). Consonants are still synthesized.

The bank is stored on this computer in `voices/<name>.wvb`.

## Limits
- Moving a recording more than about 5 semitones makes a smaller or larger sounding person, because the formants move with it (the report says how many notes are that far).
- Consonants you did not record are synthesized and do not sound like you. Recorded voiced consonants (m, n, l ...) keep the pitch they were recorded at.
- Mechanics are tested with held vowels from the built-in singer (pitch within 1 cent, level within 0.5 dB, spectrum kept, damaged files refused). It has not been tried on a real person's recording here, so how natural it sounds with your voice is unverified.
- There is no guided recorder that prompts you vowel by vowel; the window picks the tracks you recorded.

## Recording many sounds in one file

Record the sounds one after another with a pause between them, then split the file:

```
wezt_voice_takes recording.wav                       # prints each take with its measurements
wezt_voice_takes recording.wav --build bank.wvb --owner "Your Name" --i-confirm-this-is-my-voice \
    --use "0=s,1=t,2=m,3=l,4=eh,5=ah" [--base earlier_bank.wvb]
```

Your own list of what you said, in order, is more reliable than the table's guesses. A hiss with a high
spectral centre (about 6 kHz and up) is an "s"; a darker one (about 4 kHz) is an "sh". Consonants cut from
syllables ("tah") are turned down to a typical level under the vowel; hisses are set to a typical level.

Quick stops: "p", "t" and "k" are cut with a different rule from the other consonants, so a short "tah" or "kah"
without a held vowel works. Only the burst (and its breath) is kept, up to where the energy settles under 1.5 kHz,
at most 6 dB under the vowel. A "t" that is followed by a long hiss (it sounds like "tsah") is not a plain "t":
record it again without the hiss.

Letter names work too. For "ef", "en", "em", "el" and "ess" the consonant comes after the vowel, so name the
take with `@tail`: `--use "1=f@tail,12=n@tail"`. The voiced stops b, d, g, j and z are cut like p, t and k.
Quick bursts for b and g can be only a few milliseconds long; if one is missing or very short, say it again
louder with a clear vowel after it.

Letter names also give vowels: add `--short-vowels` (a vowel inside "bee" or "ef" is only about 0.2 s and its pitch
drifts) and the check accepts a steady part of 0.15 s with up to 10 % pitch drift. `--derive` fills consonants nobody
recorded from the person's own sounds already in the bank: w, y and r from the start of their oo, ee and er;
ch from t then sh; b from a little m murmur then p; v from f mixed with n; th from f; ng from n; h from a softened sh.
These are built from the person's own voice, but they are approximations, not recordings of the real sounds.
