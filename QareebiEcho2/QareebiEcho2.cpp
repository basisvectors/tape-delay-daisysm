/**
 * Gen~ Modeled Tape Delay for Electro-Smith Daisy Patch Submodule
 * WITH CLOCK SYNC & LED
 * * HARDWARE CONNECTIONS:
 * ---------------------
 * Knobs:
 * 1. Time     -> ADC 9  (Delay Time / Clock Divider if synced)
 * 2. Feedback -> CV 7   (Intensity)
 * 3. Mix      -> CV 8   (Dry/Wet)
 * 4. Filter   -> ADC 10 (Tone)
 * 5. Flutter  -> ADC 11 (Wow/Flutter Amount)
 * * Inputs:
 * - Gate In 1 -> CLOCK INPUT (Syncs delay time)
 * - Audio In  -> L/R
 * - Audio Out -> L/R
 * * Indicator:
 * - LED (B8)  -> Blinks at delay tempo
 */

#include "daisy_patch_sm.h"
#include "daisysp.h"
#include <cmath>

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;

DaisyPatchSM patch;

// Configuration
#define MAX_DELAY_TIME_SEC 3.0f // Increased slightly to allow slow clocking
#define MAX_DELAY static_cast<size_t>(48000 * MAX_DELAY_TIME_SEC)

// Buffers
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delMems[2];

// Globals for Sync & LED
GPIO led;
uint32_t last_clock_tick = 0;
float current_delay_ms = 500.0f;
bool is_clocked = false;
float led_phase = 0.0f; // For tracking blink timing

// --------------------------------------------------------------------------
// DSP FUNCTIONS (Ported from gen~)
// --------------------------------------------------------------------------

float tnhLam(float x) {
    float x2 = x * x;
    float a = (((x2 + 378.0f) * x2 + 17325.0f) * x2 + 135135.0f) * x;
    float b = ((28.0f * x2 + 3150.0f) * x2 + 62370.0f) * x2 + 135135.0f;
    return fclamp(a / b, -1.0f, 1.0f);
}

float softStatic(float x) {
    if (x > 1.0f) return (1.0f - 4.0f / (x + 3.0f)) * 4.0f + 1.0f;
    else if (x < -1.0f) return (1.0f + 4.0f / (x - 3.0f)) * -4.0f - 1.0f;
    else return x;
}

struct OnePole6dB {
    float y0 = 0.0f;
    float sample_rate;
    void Init(float sr) { sample_rate = sr; }
    float Process(float x, float cutoff, int type) {
        float f = fclamp(sinf(cutoff * TWOPI_F / sample_rate), 0.00001f, 0.99999f);
        float lp = y0 + f * (x - y0);
        y0 = lp;
        return (type == 1) ? lp - x : lp;
    }
};

struct TapeHead {
    DelayLine<float, MAX_DELAY> *del;
    OnePole6dB lpFilter, hpFilter;
    float currentDelay = 24000.0f;
    float dc_x = 0.0f, dc_y = 0.0f;

    void Init(float sr) { lpFilter.Init(sr); hpFilter.Init(sr); }

    float Process(float in, float feedback_signal, float delay_samps, float tone_freq) {
        // 1. Saturation (Pre-Tape)
        float saturated_signal = tnhLam((in + feedback_signal) * 1.3f);
        
        // 2. Write
        del->Write(saturated_signal);

        // 3. Read (Variable Speed / "Slew")
        fonepole(currentDelay, delay_samps, 0.0005f); // Tape inertia
        float tape_out = del->ReadHermite(currentDelay);

        // 4. Filters (201 Topology)
        float lp_out = lpFilter.Process(tape_out, tone_freq, 0); 
        float hp_out = hpFilter.Process(lp_out, 147.0f, 1);
        
        // 5. DC Block & Soft Limit
        float clean_out = hp_out - dc_x + 0.995f * dc_y;
        dc_x = hp_out; dc_y = clean_out;
        return softStatic(clean_out);
    }
};

TapeHead heads[2];
Oscillator flutterLfo, flutterLfo2;
float sample_rate;

float MapLog(float input, float min_freq, float max_freq) {
    input = fclamp(input, 0.0f, 1.0f);
    return min_freq * powf(max_freq / min_freq, input);
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    patch.ProcessAnalogControls(); // Processes ADC and Gates

    // ----------------------
    // 1. CLOCK / SYNC LOGIC
    // ----------------------
    uint32_t now = System::GetNow();

    // Check for Gate Trigger
    if(patch.gate_in_1.Trig()) {
        float interval = (float)(now - last_clock_tick);
        
        // Debounce & Range check (40ms to 3000ms)
        if (interval > 40.0f && interval < 3000.0f) {
            current_delay_ms = interval;
            is_clocked = true;
            led_phase = 0.0f; // Reset LED blink phase on clock
        }
        last_clock_tick = now;
    }

    // Timeout: If no clock for 3.5 seconds, revert to knob
    if (now - last_clock_tick > 3500) {
        is_clocked = false;
    }

    // ----------------------
    // 2. PARAMETER CALCULATIONS
    // ----------------------
    
    // Time Calculation
    float target_delay_samps;
    float raw_time = fclamp(patch.GetAdcValue(ADC_9) + patch.GetAdcValue(CV_1), 0.0f, 1.0f);

    if (is_clocked) {
        // If Clocked: Use Clock Time
        // (Optional: You could use raw_time here as a clock divider/multiplier)
        target_delay_samps = (current_delay_ms / 1000.0f) * sample_rate;
    } else {
        // If Not Clocked: Use Knob
        float knob_delay_ms = 10.0f + (powf(raw_time, 2.5f) * 1500.0f);
        target_delay_samps = (knob_delay_ms / 1000.0f) * sample_rate;
        current_delay_ms = knob_delay_ms; // Update global for LED sync
    }

    // Feedback
    float fb_val = fclamp((patch.GetAdcValue(CV_7) + patch.GetAdcValue(CV_2)) * 1.1f, 0.0f, 1.2f);
    
    // Tone & Flutter
    float tone_freq = MapLog(patch.GetAdcValue(ADC_10) + patch.GetAdcValue(CV_4), 400.0f, 18000.0f);
    float flutter_depth = fclamp(patch.GetAdcValue(ADC_11) + patch.GetAdcValue(CV_5), 0.0f, 1.0f) * 60.0f;
    float dry_wet = fclamp(patch.GetAdcValue(CV_8) + patch.GetAdcValue(CV_3), 0.0f, 1.0f);

    // ----------------------
    // 3. AUDIO LOOP
    // ----------------------
    static float feedL = 0.0f;
    static float feedR = 0.0f;

    for (size_t i = 0; i < size; i++) {
        // Flutter Modulation
        float wobble = (flutterLfo.Process() + (flutterLfo2.Process() * 0.5f)) * flutter_depth;
        
        float dL = fclamp(target_delay_samps + wobble, 10.0f, (float)MAX_DELAY - 100.0f);
        float dR = fclamp(target_delay_samps + wobble + 50.0f, 10.0f, (float)MAX_DELAY - 100.0f); // Stereo spread

        // Tape Process
        float outL = heads[0].Process(in[0][i], feedL * fb_val, dL, tone_freq);
        float outR = heads[1].Process(in[1][i], feedR * fb_val, dR, tone_freq);

        feedL = outL;
        feedR = outR;

        out[0][i] = (in[0][i] * (1.0f - dry_wet)) + (outL * dry_wet);
        out[1][i] = (in[1][i] * (1.0f - dry_wet)) + (outR * dry_wet);

        // 4. LED PHASE CALCULATION (Per sample)
        // Increment phase based on current delay time
        // 1.0 / (delay_in_seconds * sample_rate)
        float phase_inc = 1.0f / ( (current_delay_ms/1000.0f) * sample_rate );
        led_phase += phase_inc;
        if(led_phase >= 1.0f) led_phase -= 1.0f;
    }
}

int main(void) {
    patch.Init();
    sample_rate = patch.AudioSampleRate();

    // Init LED (B8 is the standard user LED on Patch SM)
    led.Init(DaisyPatchSM::B8, GPIO::Mode::OUTPUT);

    // Init DSP
    for(int i=0; i<2; i++) {
        delMems[i].Init();
        heads[i].del = &delMems[i];
        heads[i].Init(sample_rate);
    }

    // Init Flutter LFOs
    flutterLfo.Init(sample_rate);
    flutterLfo.SetFreq(0.4f); flutterLfo.SetAmp(1.0f);
    flutterLfo2.Init(sample_rate);
    flutterLfo2.SetFreq(3.5f); flutterLfo2.SetAmp(0.3f);
    flutterLfo2.SetWaveform(Oscillator::WAVE_TRI);

    patch.StartAudio(AudioCallback);

    while(1) {
        // Update LED in main loop to save Interrupt cycles
        // Blink on for the first 10% of the delay cycle
        led.Write(led_phase < 0.1f);
        
        // Wait 1ms
        System::Delay(1);
    }
}