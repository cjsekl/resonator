# Resonator

A sympathetic resonator workshop card inspired by the Mutable Instruments Rings module and the tanpura.
It has four resonating Karplus-Strong strings that, when excited, create rich, harmonic textures.

## Description

The sympathetic resonator simulates the behavior of strings that vibrate in response to external excitation. When you feed an audio signal into the module, it excites four virtual strings tuned to different harmonic relationships. This creates a shimmering, resonant effect similar to the sound of sympathetic strings on instruments like the sitar or tanpura.

## How It Works

- Each string is implemented as a delay line with feedback
- A lowpass filter in the feedback path simulates damping (energy loss)
- The delay time determines the pitch of each string
- Input signals excite the strings, which then resonate at their tuned frequencies

## Controls

### Knobs
- **X Knob**: Base frequency (fundamental pitch) of the resonator
- **Y Knob**: Damping amount (higher = more resonance/longer decay)
- **Main Knob**: Dry/wet mix (0 = dry signal, full = wet resonator output)

### CV Inputs
- **CV1**: 1V/octave pitch control (X knob acts as fine tune when CV connected)
- **CV2**: Damping modulation (adds to Y knob)

### Switch
- **Up**: Tuning mode - only the fundamental string sounds
- **Middle**: Normal operation
- **Down (momentary)**: Advance to next chord in progression
- **Down (hold 3 seconds)**: Reset to factory defaults (all 18 chords)

### Pulse Inputs
- **Pulse In 1**: Trigger string excitation (pluck with noise burst)
- **Pulse In 2**: Advance to next chord in progression

## Chord Modes

18 chord voicings are available:

- **HARMONIC**: Harmonic series - 1:1, 2:1, 3:1, 4:1
- **FIFTH**: Stacked fifths - 1:1, 3:2, 2:1, 3:1
- **MAJOR**: Major triad - 1:1, 5:4, 3:2, 2:1
- **MAJOR6**: Major 6th - 1:1, 5:4, 3:2, 5:3
- **MAJOR7**: Major 7th - 1:1, 5:4, 3:2, 15:8
- **DOM7**: Dominant 7th - 1:1, 5:4, 3:2, 9:5
- **ADD9**: Major add 9 - 1:1, 5:4, 3:2, 9:4
- **MAJOR10**: Major 10th - 1:1, 5:4, 3:2, 5:2
- **MINOR**: Minor triad - 1:1, 6:5, 3:2, 2:1
- **MINOR7**: Minor 7th - 1:1, 6:5, 3:2, 9:5
- **MIN9**: Minor add 9 - 1:1, 6:5, 3:2, 9:4
- **DIM**: Diminished - 1:1, 6:5, 36:25, 3:2
- **SUS2**: Suspended 2nd - 1:1, 9:8, 3:2, 2:1
- **SUS4**: Suspended 4th - 1:1, 4:3, 3:2, 2:1

### Tanpura Tunings
- **TANPURA PA**: Sa, Pa, Sa', Sa'' - 1:1, 3:2, 2:1, 4:1
- **TANPURA MA**: Sa, Ma, Sa', Sa'' - 1:1, 4:3, 2:1, 4:1
- **TANPURA NI**: Sa, Ni, Sa', Sa'' - 1:1, 15:8, 2:1, 4:1
- **TANPURA NI KOMAL**: Sa, ni, Sa', Sa'' - 1:1, 9:5, 2:1, 4:1

## Web Editor

A browser-based chord editor lets you customize which chords are in your progression and their order.

### Using the Editor
1. Connect your Resonator via USB
2. Open the editor at [johaneklund.io/resonator](https://johaneklund.io/resonator)
3. Click **Connect USB** and select `Pico` (may appear as `ttyACM0` on Linux)
4. Drag chords from the palette to build your progression
5. Click **Send to Device** to save

Changes persist on the module even after power off.

### Requirements
- Chrome or Edge browser (Web Serial API required)
- USB connection to Resonator

## LED Indicators

The 6 LEDs show the current position in the chord progression (0-17). Single LEDs indicate positions 0-5, LED pairs indicate positions 6-17.

## Building

```bash
mkdir build && cd build
cmake ..
make
```

This generates `resonator.uf2` in the `build/` directory.

## Flashing

1. Hold BOOTSEL on the Pico while connecting USB
2. Copy `resonator.uf2` to the mounted drive
3. The Pico will reboot automatically

## References

- [Mutable Instruments Rings Source Code](https://github.com/pichenettes/eurorack/tree/master/rings)
- [Tanpura](https://en.wikipedia.org/wiki/Tanpura)
- [Karplus-Strong Synthesis](https://en.wikipedia.org/wiki/Karplus%E2%80%93Strong_string_synthesis)
