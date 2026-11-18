# Immersive audio

What exists, and what it is not.

**Not included:** anything from Dolby. The Dolby Atmos Renderer, its encoders (Dolby Digital Plus JOC,
TrueHD), its master file format and its certification are proprietary and licensed. Wezt contains no
Dolby code, assets or names in its software, and does not claim to be a Dolby product or to be certified.

**Included (open, clean-room):**

- Speaker layouts after the ITU-R BS.2051 conventions: 2.0, 5.1, 7.1, 5.1.2, 7.1.2, 7.1.4, 9.1.6.
- An object renderer: beds and objects in a unit room (x left to right, y back to front, z ear level to ceiling),
  panned with VBAP (constant power, continuous, exact at a speaker), spread by size or when an object is
  inside the room. Position automation follows the timeline.
- A binaural headphone monitor (spherical-head model; not measured HRTFs).
- Loudness metering after ITU-R BS.1770-4 (K-weighting, gating, momentary, short-term, integrated)
  and an approximate true peak.
- ADM BWF export (ITU-R BS.2076 metadata in `chna` and `axml` chunks, as in ITU-R BS.2088) with beds as
  DirectSpeakers and objects as Objects with position blocks. Object-based mastering tools import this
  format. The XML is well formed and parses; it has **not** been validated against the EBU schema or
  imported into any Dolby or other tool, so do that before relying on it.

**Not done yet:** routing the renderer and binaural monitor into the live audio path (the Immersive window
edits object metadata and shows levels estimated from the track meters), and the per-object position
automation lanes in the UI.

Known limitation: with several top speakers in one plane (7.1.4, 9.1.6) the overhead centre is
triangulated along one diagonal, so a source exactly overhead favours two of the four tops.
