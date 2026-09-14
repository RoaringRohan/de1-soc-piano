# DE1-SoC Piano

A playable polyphonic synthesiser on a DE1-SoC board. A PC keyboard becomes a piano, notes are
generated sample-by-sample in software and pushed into the board's audio CODEC, a visual keyboard
is drawn to a VGA display, and performances can be recorded and played back.

Everything here is about one constraint: **the CODEC consumes 8,000 samples per second and will not
wait.** If the program stops producing samples — because it is polling a keyboard, or drawing to a
screen — the audio glitches audibly. Most of the design follows from refusing to let that happen.

> **Demo video:** *(to be added)*
>
> This needs the physical board and a pair of speakers, so there is no recording in this README yet
> — see [Why there is no demo here](#why-there-is-no-demo-here).

## Making a note

There is no MIDI synthesiser and no sound library. Every sample is computed:

```c
sample = volume * sin(n * radians_per_sample)
```

where `radians_per_sample = 2π · frequency / 8000`. Incrementing *n* once per sample walks the phase
of a sine wave at exactly the right rate, and the result is written to the CODEC's left and right
data registers.

The twelve semitones of an octave plus the octave itself are defined by frequency, in equal
temperament from middle C to the C above it:

| Note | Hz | | Note | Hz |
|---|---|---|---|---|
| C4 | 261.626 | | F#4 | 369.994 |
| C#4 | 277.183 | | G4 | 391.995 |
| D4 | 293.665 | | G#4 | 415.305 |
| D#4 | 311.127 | | A4 | 440.000 |
| E4 | 329.628 | | A#4 | 466.164 |
| F4 | 349.228 | | B4 | 493.883 |
| | | | C5 | 523.251 |

**Chords come free.** Because a sample is just a number, playing several notes at once is summing
their sine values before writing — which is what superposition means physically. Part 2 takes a
13-bit vector on the command line, one bit per note, and mixes every enabled tone into each sample.

## Not glitching: FIFO backpressure

The audio core exposes a `fifospace` register whose upper bytes report how much room remains in the
left and right output FIFOs. Before writing a sample the code reads it, extracts the two counts, and
spins until both have space:

```c
fifospace = *(AUDIO_PTR + 1);
wslc = (fifospace & 0xFF000000) >> 24;   // write space, left channel
wsrc = (fifospace & 0x00FF0000) >> 16;   // write space, right channel
```

That spin is what couples the program's speed to the hardware's. The CODEC drains the FIFO at 8 kHz,
so the loop naturally runs at 8 kHz without any timer — the hardware sets the pace and the software
follows it.

## Not glitching: threads

Spinning on a FIFO works until the program also has to do something else. Reading a keyboard is a
blocking read on a device file; drawing a keyboard diagram means writing a few hundred pixels. Do
either of those on the same thread that feeds the CODEC and the audio stutters.

From part 3 onward the program is threaded:

- a **main thread** that blocks reading PS/2 keyboard events,
- an **audio thread** that does nothing but generate samples and feed the FIFO,
- and from part 4, a **video thread** that redraws the on-screen keyboard.

They share one small piece of state — a 13-element array of per-note volumes — protected by a
mutex. The keyboard thread sets a note's volume when a key goes down and clears it when the key
comes up; the audio thread reads the array each sample and mixes whatever is currently non-zero.
That is the entire synchronisation design, and it is the right size for the problem: the shared
state is tiny, the lock is held briefly, and neither slow thread can stall the fast one.

## How it builds up

| Part | What it adds |
|---|---|
| **1** | Plays the chromatic scale, 300 ms per note — proves the sample loop and the FIFO handshake |
| **2** | Chords, from a 13-bit command-line vector, by summing tones into each sample |
| **3** | Live playing — PS/2 keyboard input, with the audio moved onto its own thread |
| **4** | A VGA keyboard display on a third thread, showing which notes are sounding |
| **5** | Record and playback, with the board's pushbuttons and LEDs as transport controls and a stopwatch driver providing the timebase |
| **6** | Refinement of the recorder |

`part5.c` and `part6.c` are ~830 lines each; the whole set is about 2,600 lines.

## Recording

Part 5 is where it becomes an instrument rather than a demo. A stopwatch kernel module gives a
centisecond timebase, so a recording is a list of note events stamped with the time they happened
relative to the start. Playback replays them against the same clock. The board's KEY pushbuttons
start and stop recording and trigger playback, and the LEDR row shows the current mode — so once it
is running the program needs no console at all.

## Building and running

On a DE1-SoC running the course Linux image, with speakers or headphones in the line-out jack:

```bash
./runall.sh 1                      # chromatic scale
./runall.sh 2 1000100100000        # a chord - 13 bits, one per note (this is C-E-G, a C major triad)
./runall.sh 3 /dev/input/eventX    # live playing; X is your PS/2 keyboard
./runall.sh 5 /dev/input/eventX    # record and playback
./runall.sh clean
```

`runall.sh` loads the kernel modules each part needs — audio, video, KEY, LEDR and stopwatch — in
the right order, then builds and runs that part.

## Why there is no demo here

This is embedded work against physical hardware: an audio CODEC and a VGA controller at fixed
addresses in a Cyclone V FPGA, reached from an ARM Cortex-A9 running Linux. It cannot run anywhere
else, and a container does not help. Nothing in this repository was executed during its publication.

It is also the project in this group where that bites hardest, because the output is *sound*. A
screenshot would show a keyboard diagram and tell you nothing about whether the notes were in tune
or whether the audio glitched — which is the whole engineering question. That is what the demo video
is for, and it will be linked at the top of this README when it exists.

## A note on what is here

The six `partN.c` files are the project's own work — about 2,600 lines against a course template
that supplied roughly 117 lines of empty stubs.

Everything else is course-supplied scaffolding, included because the code does not build or run
without it: `include/address_map_arm.h` (the standard DE1-SoC address map), `include/defines.h`
(note frequencies and audio register offsets), `physical.c` and `include/physical.h` (the `/dev/mem`
mapping helpers), `stopwatch_wrappers.c` and `include/stopwatch.h` (the stopwatch driver API), the
per-part `Makefile`s, and `runall.sh` — which the team modified, and from which a coursework
submission target has been removed here.

This project shares its DE1-SoC hardware context and its `address_map_arm.h` with the other
DE1-SoC projects published from the same course, so that header is not original to any of them.

Built with a partner.

---

*Originally built as a graduate course project.*
