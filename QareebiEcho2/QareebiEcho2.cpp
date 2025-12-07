/**
 * Gen~ Modeled Tape Delay for Electro-Smith Daisy Patch Submodule
 * * HARDWARE CONNECTIONS:
 * ---------------------
 * Knobs (left to right):
 * 1. Time     -> ADC 9  (Delay Time / Tap Override)
 * 2. Feedback -> CV 7   (Intensity)
 * 3. Mix      -> CV 8   (Dry/Wet)
 * 4. Filter   -> ADC 10 (Tone/Bandwidth)
 * 5. Flutter  -> ADC 11 (Wow/Flutter Amount)
 * * CV Input Jacks:
 * - CV 1-5    -> Modulate the parameters above
 * - CV 6      -> Extra Filter Cutoff CV
 * * Control:
 * - Gate In 1 -> Tap Tempo Input
 * - Button D1 -> Manual Tap Tempo
 * - LED B8    -> Tempo Indicator
 * * Audio:
 * - Audio In  -> L/R stereo input
 * - Audio Out -> L/R stereo output
 */

#include "daisy_patch_sm.h"
#include "daisysp.h"
#include <cmath>

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;

DaisyPatchSM patch;

// --------------------------------------------------------------------------
// CONFIGURATION
// --------------------------------------------------------------------------
#define MAX_DELAY_TIME_SEC 2.0f
#define MAX_DELAY static_cast<size_t>(48000 * MAX_DELAY_TIME_SEC)
#define MIN_DELAY_MS 10.0f

DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delMems[2];

// --------------------------------------------------------------------------
// DSP HELPER FUNCTIONS (From Gen~ Script)
// --------------------------------------------------------------------------

// Tanh Lambert Approximation - The "Tape Saturation" Curve
float tnhLam(float x) {
    float x2 = x * x;
    float a = (((x2 + 378.0f) * x2 + 17325.0f) * x2 + 135135.0f) * x;
    float b = ((28.0f * x2 + 3150.0f) * x2 + 62370.0f) * x2 + 135135.0f;
    return fclamp(a / b, -1.0f, 1.0f);
}

// Soft Static Limiter - Output Stage Protection
float softStatic(float x) {
    if (x > 1.0f) return (1.0f - 4.0f / (x + 3.0f)) * 4.0f + 1.0f;
    else if (x < -1.0f) return (1.0f + 4.0f / (x - 3.0f)) * -4.0f - 1.0f;
    else return x;
}

// 6dB One Pole Filter
struct OnePole6dB {
    float y0 = 0.0f;
    float sample_rate;
    void Init(float sr) { sample_rate = sr; }
    float Process(float x, float cutoff, int type) {
        float f = fclamp(sinf(cutoff * TWOPI_F / sample_rate), 0.00001f, 0.99999f);
        float lp = y0 + f * (x - y0);
        y0 = lp;
        return (type == 1) ? (lp - x) : lp; // Type 1 = "Wrong" HP, Type 0 = LP
    }
};

// --------------------------------------------------------------------------
// TAPE HEAD STRUCT
// --------------------------------------------------------------------------
struct TapeHead {
    DelayLine<float, MAX_DELAY> *del;
    OnePole6dB lpFilter;
    OnePole6dB hpFilter;
    
    // States
    float currentDelay = 24000.0f;
    float dc_x = 0.0f, dc_y = 0.0f;

    void Init(float sr) {
        lpFilter.Init(sr);
        hpFilter.Init(sr);
    }

    // Main Signal Path
    // Flow: Input+Fb -> Saturation -> Write -> Read -> Filter -> Output
    float Process(float in, float feedback_signal, float delay_samps, float tone_freq) {
        
        // 1. INPUT MIX & SATURATION (Amp Simulation Stage)
        // In the gen~ script, saturation happens *before* writing to tape
        float dry_signal = in + feedback_signal;
        
        // Apply 'tnhLam' Saturation (Tape Saturation)
        // We boost slightly into it to drive the saturation, simulating input gain
        float saturated_signal = tnhLam(dry_signal * 1.3f);

        // 2. WRITE TO TAPE
        del->Write(saturated_signal);

        // 3. Read (with slew limiting on time changes)
        fonepole(currentDelay, delay_samps, 0.0005f);
        
        // Use Hermite Interpolation (Cubic) as requested by gen~ code (interp="cubic")
        float tape_out = del->ReadHermite(currentDelay);

        // 4. FILTERING (Trap Filters Stage - Topology 0 "201")
        // The gen~ script uses a chain of OnePole LP -> OnePole HP
        
        // LP: Tone control
        float lp_out = lpFilter.Process(tape_out, tone_freq, 0); 
        
        // HP: Fixed rumble filter (approx 147Hz in gen~)
        float hp_out = hpFilter.Process(lp_out, 147.0f, 1);

        // 5. OUTPUT STAGE
        // DC Block (standard)
        float clean_out = hp_out - dc_x + 0.995f * dc_y;
        dc_x = hp_out;
        dc_y = clean_out;

        return softStatic(clean_out);
    }
};

TapeHead heads[2];
Oscillator flutterLfo, flutterLfo2;
GPIO led_pin;
Switch tap_button;

// --------------------------------------------------------------------------
// GLOBAL CONTROL VARIABLES
// --------------------------------------------------------------------------
float sample_rate;
float global_delay_samples = 24000.0f;
float global_feedback = 0.0f;
float global_tone = 2000.0f;
float global_flutter = 0.0f;
float global_mix = 0.5f;

// Tap Tempo Globals
uint32_t last_tap_time = 0;
uint32_t tap_count = 0;
bool use_tap_tempo = false;
float last_knob_time_val = -1.0f;

// LED Globals
float led_brightness = 0.0f;
uint32_t last_led_toggle = 0;

// Helper: Logarithmic Map
float MapLog(float input, float min_freq, float max_freq) {
    input = fclamp(input, 0.0f, 1.0f);
    return min_freq * powf(max_freq / min_freq, input);
}

// --------------------------------------------------------------------------
// CONTROL PROCESSING
// --------------------------------------------------------------------------
void ProcessControls() {
    patch.ProcessAnalogControls();
    tap_button.Debounce();

    // 1. TIME & TAP TEMPO LOGIC
    float knob_time = patch.GetAdcValue(ADC_9);
    float cv_time   = patch.GetAdcValue(CV_1);
    
    // Check if knob moved significantly to override tap tempo
    if (fabs(knob_time - last_knob_time_val) > 0.02f) {
        use_tap_tempo = false;
        last_knob_time_val = knob_time;
    }

    // Handle Tap Button (D1) or Gate In (Gate 1)
    if (tap_button.RisingEdge() || patch.gate_in_1.Trig()) {
        uint32_t now = System::GetNow();
        if (tap_count > 0 && (now - last_tap_time) < 5000) { // 5 sec timeout
            float tap_ms = (float)(now - last_tap_time);
            tap_ms = fclamp(tap_ms, MIN_DELAY_MS, MAX_DELAY_TIME_SEC * 1000.0f);
            
            global_delay_samples = (tap_ms / 1000.0f) * sample_rate;
            use_tap_tempo = true;
        } else {
            tap_count = 0; // Reset logic if timed out
        }
        last_tap_time = now;
        tap_count++;
    }

    // Calculate Final Time
    if (!use_tap_tempo) {
        float raw_time = fclamp(knob_time + cv_time, 0.0f, 1.0f);
        // Exponential curve for time (more resolution at short delays)
        float delay_ms = MIN_DELAY_MS + (powf(raw_time, 2.5f) * (MAX_DELAY_TIME_SEC * 1000.0f)); 
        global_delay_samples = (delay_ms / 1000.0f) * sample_rate;
    }

    // 2. FEEDBACK
    float knob_fb = patch.GetAdcValue(CV_7);
    float cv_fb   = patch.GetAdcValue(CV_2);
    // Allow going > 1.0 for self-oscillation
    global_feedback = fclamp((knob_fb + cv_fb) * 1.1f, 0.0f, 1.2f);

    // 3. MIX
    float knob_mix = patch.GetAdcValue(CV_8);
    float cv_mix   = patch.GetAdcValue(CV_3);
    global_mix = fclamp(knob_mix + cv_mix, 0.0f, 1.0f);

    // 4. FILTER (TONE)
    float knob_filt = patch.GetAdcValue(ADC_10);
    float cv_filt   = patch.GetAdcValue(CV_4);
    float cv_filt2  = patch.GetAdcValue(CV_6);
    float raw_filt = knob_filt + cv_filt + cv_filt2;
    global_tone = MapLog(raw_filt, 400.0f, 18000.0f);

    // 5. FLUTTER
    float knob_flut = patch.GetAdcValue(ADC_11);
    float cv_flut   = patch.GetAdcValue(CV_5);
    // Max depth approx 60 samples
    global_flutter = fclamp(knob_flut + cv_flut, 0.0f, 1.0f) * 60.0f; 
}

void ProcessLED() {
    // Blink LED at delay rate
    float current_ms = (global_delay_samples / sample_rate) * 1000.0f;
    uint32_t now = System::GetNow();

    if (now - last_led_toggle > current_ms) {
        led_brightness = (led_brightness > 0.5f) ? 0.0f : 1.0f;
        last_led_toggle = now;
    }
    led_pin.Write(led_brightness > 0.5f);
}

// --------------------------------------------------------------------------
// AUDIO CALLBACK
// --------------------------------------------------------------------------
void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    ProcessControls();
    ProcessLED();

    static float feedL = 0.0f;
    static float feedR = 0.0f;

    for (size_t i = 0; i < size; i++) {
        // Generate complex wow/flutter
        float wob1 = flutterLfo.Process();
        float wob2 = flutterLfo2.Process();
        float total_flutter = (wob1 + (wob2 * 0.5f)) * global_flutter;

        // Calculate stereo delay times (Right channel slightly offset for width)
        float dL = fclamp(global_delay_samples + total_flutter, 10.0f, (float)MAX_DELAY - 100.0f);
        float dR = fclamp(global_delay_samples + total_flutter + 50.0f, 10.0f, (float)MAX_DELAY - 100.0f);

        // Process DSP
        // Note: Using 'feedL' in Process() creates the feedback loop
        float outL = heads[0].Process(in[0][i], feedL * global_feedback, dL, global_tone);
        float outR = heads[1].Process(in[1][i], feedR * global_feedback, dR, global_tone);

        // Simple Cross-Feedback for stereo swirl
        feedL = outL;
        feedR = outR;

        // Output Mix
        out[0][i] = (in[0][i] * (1.0f - global_mix)) + (outL * global_mix);
        out[1][i] = (in[1][i] * (1.0f - global_mix)) + (outR * global_mix);
    }
}

// --------------------------------------------------------------------------
// MAIN
// --------------------------------------------------------------------------
int main(void) {
    patch.Init();
    sample_rate = patch.AudioSampleRate();

    // Initialize Delay Memory & Heads
    for(int i = 0; i < 2; i++) {
        delMems[i].Init();
        heads[i].del = &delMems[i];
        heads[i].Init(sample_rate);
    }

    // Init LFOs for Flutter
    flutterLfo.Init(sample_rate);
    flutterLfo.SetWaveform(Oscillator::WAVE_SIN);
    flutterLfo.SetFreq(0.4f); // Slow capstan eccentricity
    flutterLfo.SetAmp(1.0f);

    flutterLfo2.Init(sample_rate);
    flutterLfo2.SetWaveform(Oscillator::WAVE_TRI);
    flutterLfo2.SetFreq(3.5f); // Faster motor flutter
    flutterLfo2.SetAmp(0.3f);

    // Initialize Hardware Peripherals
    led_pin.Init(DaisyPatchSM::B8, GPIO::Mode::OUTPUT);
    tap_button.Init(DaisyPatchSM::D1, patch.AudioCallbackRate());

    patch.StartAudio(AudioCallback);
    while(1) { }
}