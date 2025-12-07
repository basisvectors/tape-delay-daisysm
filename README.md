# TapeDelay: Modeled Tape Delay for Daisy Patch SM

## Overview

**TapeDelay** is a stereo tape delay module for the Electro-Smith Daisy Patch Submodule, inspired by Gen~ tape delay algorithms. It features:

- Multi-mode tape delay with analog-style feedback, wow/flutter, and tone shaping
- Freeze/Blur (infinite hold) and Reverse Feedback modes
- Clock sync via external gate input
- Visual tempo indication and state via LED
- Gate output for tempo clock

## Hardware Construction

This module is designed for the Daisy Patch SM platform. It uses the following hardware connections:

### Knobs (left to right)
1. **Time**     → ADC 9  (Delay time, or clock divider if synced)
2. **Feedback** → CV 7   (Intensity)
3. **Mix**      → CV 8   (Dry/Wet)
4. **Filter**   → ADC 10 (Tone)
5. **Flutter**  → ADC 11 (Wow/Flutter amount)

### Inputs
- **Gate In 1** → Clock input (syncs delay time)
- **Audio In**  → Stereo L/R

### Controls
- **Button D1** → Freeze/Blur toggle (stops input, infinite feedback, stable)
- **Button D2** → Reverse feedback toggle

### Outputs
- **Gate Out 2** → Tempo clock output (pulses at delay time)
- **Audio Out**  → Stereo L/R

### Indicator
- **LED (B8)**   → Blinks at tempo, solid when Freeze or Reverse is active

## Features

- **Analog Tape Delay**: Modeled feedback, soft saturation, and DC blocking for authentic tape sound.
- **Freeze/Blur**: Press D1 to hold the current buffer and set feedback to infinite (safe, no runaway gain).
- **Reverse Feedback**: Press D2 to enable reverse playback in the feedback path for evolving, reversed echoes.
- **Clock Sync**: Send a clock to Gate In 1 to sync delay time to external tempo. Delay time knob acts as a divider.
- **Wow/Flutter**: LFO-based modulation for tape-style pitch movement.
- **Tone Control**: Lowpass and highpass filtering in the feedback path for classic tape coloration.
- **Gate Out**: Outputs a clock pulse at the current delay time for syncing other gear.
- **LED Feedback**: LED blinks at tempo, stays solid when Freeze or Reverse is active.

## Usage

1. **Power on the Daisy Patch SM with the module loaded.**
2. **Connect stereo audio to Audio In and Out.**
3. **Adjust the five knobs to set delay time, feedback, mix, tone, and flutter.**
4. **To sync to an external clock, send a gate to Gate In 1.**
5. **Press D1 to freeze/blur the buffer (infinite hold, input muted, feedback safe).**
6. **Press D2 to enable reverse feedback (buffer plays backward in feedback path).**
7. **Gate Out 2 will output a clock pulse at the current delay time.**
8. **LED (B8) blinks at tempo, solid when Freeze or Reverse is active.**

## File Structure

- `TapeDelay.cpp` — Main firmware source implementing the tape delay logic and controls
- `README.md`     — This documentation

## Credits

Algorithm and code inspired by Magnetic M4L device Gen~ tape delay techniques. Hardware and DSP by Electro-Smith Daisy Patch SM and DaisySP.

## Building and Installation

*You might have to adapt the hardware connections to suit for example if you are using a daisy patch init or something else.*

Look at hwtest/hwtest.cpp to see how exactly my hardware was wired up.

1. Clone this repository into the `./DaisyExamples` directory to get `./DaisyExamples/tape-delay-daisysm/`.
2. Make sure the `libDaisy` submodule is correctly initialized in `./DaisyExamples/libDaisy/`.
3. Open the project in your preferred IDE and build it for the Daisy Patch SM.
    
- run `make` to compile the project.
 
- plug in the Daisy Patch SM via USB and run `make program-dfu` to upload the firmware.

4. Pray that it works on the first try!