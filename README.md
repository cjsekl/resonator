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
- **Down**: Cycles through eleven chord modes

#### Chord Modes
- **HARMONIC**: Harmonic series - 1:1, 2:1, 3:1, 4:1
- **FIFTH**: Stacked fifths - 1:1, 3:2, 2:1, 3:1
- **MAJOR7**: Major 7th chord - 1:1, 5:4, 3:2, 15:8
- **MINOR7**: Minor 7th chord - 1:1, 6:5, 3:2, 9:5
- **DIM**: Diminished - 1:1, 6:5, 36:25, 3:2
- **SUS4**: Suspended 4th - 1:1, 4:3, 3:2, 2:1
- **ADD9**: Major add 9 - 1:1, 5:4, 3:2, 9:4
- **TANPURA_PA**: Tanpura Pa drone - 1:1, 3:2, 2:1, 4:1 (Sa, Pa, Sa', Sa'')
- **TANPURA_MA**: Tanpura Ma drone - 1:1, 4:3, 2:1, 4:1 (Sa, Ma, Sa', Sa'')
- **TANPURA_NI**: Tanpura Ni drone - 1:1, 15:8, 2:1, 4:1 (Sa, Ni, Sa', Sa'')
- **TANPURA_NI_KOMAL**: Tanpura ni drone - 1:1, 9:5, 2:1, 4:1 (Sa, ni, Sa', Sa'')

### Pulse Inputs
- **Pulse In 1**: Trigger string excitation (pluck with noise burst)
- **Pulse In 2**: Reserved for future use

## LED Indicators

All 6 LEDs indicate the current chord mode:

| Mode | LEDs |
|------|------|
| HARMONIC | 0 |
| FIFTH | 1 |
| MAJOR7 | 2 |
| MINOR7 | 3 |
| DIM | 4 |
| SUS4 | 5 |
| ADD9 | 0 + 5 |
| TANPURA_PA | 1 + 4 |
| TANPURA_MA | 2 + 3 |
| TANPURA_NI | 0 + 3 |
| TANPURA_NI_KOMAL | 2 + 5 |

## Building

Use the standard Workshop System build process:
```bash
./build.sh
```

This will generate a `resonator.uf2` file in the `build/` directory.

## References

- [Mutable Instruments Rings Source Code](https://github.com/pichenettes/eurorack/tree/master/rings)
- [Tanpura](https://en.wikipedia.org/wiki/Tanpura)
- [Karplus-Strong Synthesis](https://en.wikipedia.org/wiki/Karplus%E2%80%93Strong_string_synthesis)
