# ORBITAL

A poly-rhythmic FM sequencer for the M-VAVE FM-1, ported from the ORBITAL HTML toy. Eight lanes, one per scale
degree, each looping its own length (2-24 steps) against one clock. Every note is a 2-op or 4-op FM voice with a
feedback shaper and a sweeping contour filter, sent into a four-tap echo field. Made with
[foma](https://github.com/konsumer/foma).

**Try it in the browser, and install it on the FM-1 from Chrome or Edge:** <https://sunjaculley.github.io/orbital-fm1/>

## Playing it

| Control | What it does |
| --- | --- |
| White keys | Steps of the selected lane. Tap cycles the note length: short, medium, long, longest, off |
| OCT- / OCT+ | White keys reach steps 1-16 / 17-24 (the pink bar on screen shows which) |
| Hold a step | KNOB1-4 lock PITCH, VELOCITY, FM INDEX offset, SEND to it (a mint mark shows a lock). EDIT turns its lock on/off |
| F# G# A# C# D# F# G# A# (first eight black keys) | Lane select. Hold one and turn KNOB1: that lane's length (2-24). PRESETS also steps through lanes |
| C#5 | RANDOM the lane. Hold 1 s: every lane |
| D#5 | CLEAR the lane (with steps held: clear their locks) |
| F#5 | Hold 1 s: INIT |
| EDIT · ENV · SEL · FX · GLO | Pages: VOICE (ratio, index, feedback, 2-op / 4-op) · ENV (attack, decay, sub) · TONE (cutoff, contour, resonance) · SPACE (space, gravity, trail) · GLOBAL (tempo, scale, root, MIDI sync). SELECT cycles them |
| ALGORITHM | Tempo |
| PLAY / SAVE | Start, stop / store everything in flash (when stopped) |

The screen shows all eight lanes at once, falling top to bottom: bar width is the note length, the brighter cell
is each lane's playhead.

## What changed for the FM-1

- **Up to 12 notes at once.** When more are asked for, the quietest gives way. A CPU meter in the header lowers that
  to 10, 8 or 6 when the chip runs short, and raises it again when there is room.
- **The echo field is mono**, its four taps panned left, right, left, right.
- **The step grid** reaches 24 steps through two key pages (OCT- / OCT+), since the FM-1 has 16 white keys.
- **New:** PRESETS steps through lanes; hold RANDOM for every lane at once; MIDI clock sync on the GLOBAL page.

## Develop

Only Docker is needed:

```sh
./fm1 dev          # http://localhost:8000: the emulator, the installer, the rescue; rebuilds when you save
./fm1 build        # build/orbital-VERSION.fwsc
```

The firmware is in `src/main.c`; what it can call is in [platform/fm1.h](platform/fm1.h). Every push to `main`
publishes the emulator and the installer to GitHub Pages; a tag (`v0.2`) also makes a release with the `.fwsc`.

Installing firmware is at your own risk; back up your FM-1 first. If it does not start, hold OCT- while switching it
on (the USB rescue), then install from the page.

## License

GPL-3.0-only. Credits and third-party parts: [LICENSING.md](LICENSING.md).
