# voicefx

A sound filter plugin for the Kirikiri Z engine (krkrz_dev): `VoiceFX`, an effect for a
`WaveSoundBuffer`'s `filters`, for the voices of a visual novel as commercial games have them:
an echo on a memory or an inner voice, a thin voice on the phone, a muffled one through a door,
a hall, a cave, a radio. Written from the filter designs in the textbooks (RBJ's biquads,
Butterworth's poles, Schroeder's reverb), with no code from elsewhere. The presets are meant to be
heard without getting in the way of the line: a voice keeps its loudness, and an echo or a room
sits about 10 dB under it (a cave's about 6).

Written for [KAGEX](https://github.com/krkren/kagex), whose engines have it built in and whose
World sets it per character with a tag, but it works with any game on those engines.

## For scripts

```
Plugins.link("voicefx.dll");

var fx = new VoiceFX();
fx.preset = "phone";          // or the settings one by one
buffer.filters.add(fx);       // before the sound opens: the engine builds a buffer's filter
buffer.open("ai0001");        // chain as a sound is opened
buffer.play();
```

A `VoiceFX` serves one buffer at a time. Its settings may change while it plays (from the next
block of samples on). With an echo or a reverb, the sound plays on after its end until they've
died away (at most `tail` ms); meanwhile the buffer's `position` stays at the end.

It may be invalidated while a buffer has it: the buffer still holds the filter until it opens
another sound or goes itself, and the filter goes then.

| Member | |
|---|---|
| `preset` | a preset's name; setting it sets every setting: `none`, `echo`, `phone`, `muffled`, `hall`, `cave`, `radio` |
| `VoiceFX.presets()` | the presets' names |
| `load(d)` | a dictionary: its `preset` first, then each setting it has (or a preset's name) |
| `set(name, value)`, `get(name)` | a setting by name (false, void for another name) |
| `interface` | for the engine (`WaveSoundBuffer.filters` reads it) |
| `connected` | whether a buffer is using it |

| Setting | | Off |
|---|---|---|
| `lowcut`, `highcut` | Butterworth high- and low-pass filters, Hz | 0 |
| `order` | 2 or 4 (12 or 24 dB an octave) | 2 |
| `drive` | soft clipping before the filters, 0-100: a line's crunch; quiet parts stay as they are, loud ones flatten | 0 |
| `mono` | both channels the same | 0 |
| `delay`, `feedback`, `echo`, `damping` | an echo: its delay (ms), how much comes round again (0-95 %), its level (0-100 %), each repeat darker by damping (0-100 %) | 0 |
| `room`, `reverb` | a reverb: room size 0-100, its level 0-100 % (100: about as loud as the voice, whatever the room's size; with `damping`) | 0 |
| `dry` | the processed voice's own level, % | 100 |
| `gain` | the whole effect's level, % | 100 |
| `tail` | the most the effect rings on after the sound, ms | 8000 |

The output is 32-bit float, as the engine's own phase vocoder gives.

## Building

As part of the engine: add this repository's `plugins` folder to krkrz_dev's
`TVP_PLUGIN_FOLDERS` (after krkrz_dev's own `src/plugins`, whose tp_stub and ncbind it uses) and
`voicefx` to `TVP_PLUGINS`, or to a platform kit's `pluginFolders` and `plugins`.

On its own, with its tests:

```
cmake -B build -DKRKRZ_DEV_DIR=/path/to/krkrz_dev && cmake --build build && ctest --test-dir build
```

`dsp.cpp` has no engine in it; `tests/dsp_test.cpp` checks that `none` changes nothing, the
phone's and the door's filters (the phone's 350-3200 Hz band cuts 80 Hz by more than 20 dB and
9 kHz by more than 15), that the reverb at 100 % is about as loud as the voice in any room, the
echo's repeats (where and how loud), that the reverb stays bounded, that what rings on dies away
and that a reset silences it.

## How it plugs in

The engine reads a filter's `interface` as a pointer to its `iTVPBasicWaveFilter` and puts the
filter between the decoder and the buffer (`Recreate`), as it does its own phase vocoder. The
plugin declares the two interfaces as the engine does (`common/sound/WaveIntf.h`,
`WaveLoopManager.h`) and never touches the engine's segment queue, which it passes on as it
comes: that's what lets it work without the engine's headers. After the decoder has run out, it
keeps the buffer filled with the effect's tail and gives fewer samples than asked only once that
has died away, which is how the engine knows a sound has ended.

## Licence

0BSD (`LICENSE`).
