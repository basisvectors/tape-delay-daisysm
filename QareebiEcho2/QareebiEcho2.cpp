/**
 * Gen~ Modeled Tape Delay for Electro-Smith Daisy Patch Submodule
 * * Modeled after 'gentildacode.cpp'
 * * HARDWARE CONNECTIONS:
 * ---------------------
 * Knobs (left to right):
 * 1. Time     -> ADC 9  (Delay Time)
 * 2. Feedback -> CV 7   (Intensity)
 * 3. Mix      -> CV 8   (Dry/Wet)
 * 4. Filter   -> ADC 10 (Tone/Bandwidth)
 * 5. Flutter  -> ADC 11 (Wow/Flutter Amount)
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

// Configuration
#define MAX_DELAY_TIME_SEC 2.0f
#define MAX_DELAY static_cast<size_t>(48000 * MAX_DELAY_TIME_SEC)

// Buffers
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delMems[2];

// --------------------------------------------------------------------------
// GEN~ PORTED FUNCTIONS
// --------------------------------------------------------------------------

// Tanh Lambert Approximation (from gentildacode.cpp 'tnhLam')
// This provides the specific non-linear saturation curve of the tape.
float tnhLam(float x) {
    float x2 = x * x;
    float a = (((x2 + 378.0f) * x2 + 17325.0f) * x2 + 135135.0f) * x;
    float b = ((28.0f * x2 + 3150.0f) * x2 + 62370.0f) * x2 + 135135.0f;
    float res = a / b;
    return fclamp(res, -1.0f, 1.0f);
}

// Static limit, processes at 1, limits at 4 (from gentildacode.cpp 'softStatic')
// Used at the output stage to round off peaks.
float softStatic(float x) {
    if (x > 1.0f)
        return (1.0f - 4.0f / (x + 3.0f)) * 4.0f + 1.0f;
    else if (x < -1.0f)
        return (1.0f + 4.0f / (x - 3.0f)) * -4.0f - 1.0f;
    else
        return x;
}

// 6dB One Pole Filter (from gentildacode.cpp 'eAllPoleLPHP6')
// Type 0 = LP, Type 1 = HP (Imperfect)
struct OnePole6dB {
    float y0 = 0.0f;
    float sample_rate;

    void Init(float sr) { sample_rate = sr; }

    float Process(float x, float cutoff, int type) {
        // approx sin(cutoff * 2pi / sr)
        float f = fclamp(sinf(cutoff * TWOPI_F / sample_rate), 0.00001f, 0.99999f);
        
        // lp = mix(y0, x, f);
        float lp = y0 + f * (x - y0);
        y0 = lp;

        if (type == 1) {
            return lp - x; // Intentionally "wrong" HP from gen~ script
        } else {
            return lp;     // Lowpass
        }
    }
};

// --------------------------------------------------------------------------
// TAPE SYSTEM STRUCT
// --------------------------------------------------------------------------

struct TapeHead {
    DelayLine<float, MAX_DELAY> *del;
    OnePole6dB lpFilter;
    OnePole6dB hpFilter;
    
    // States
    float currentDelay = 24000.0f;
    float targetDelay = 24000.0f;
    float dc_x = 0.0f; // For final DC blocking
    float dc_y = 0.0f;

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

        // 3. READ FROM TAPE (Capstan Stage)
        // Smooth delay time changes
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
Oscillator flutterLfo;
Oscillator flutterLfo2; // Secondary LFO for complex wobble
float sample_rate;

// Control Vars
float cv_time = 0.5f;
float cv_feedback = 0.0f;
float cv_mix = 0.5f;
float cv_tone = 2000.0f;
float cv_flutter = 0.0f;

// Helper to map 0-1 to Frequency Logarithmically
float MapLog(float input, float min_freq, float max_freq) {
    input = fclamp(input, 0.0f, 1.0f);
    return min_freq * powf(max_freq / min_freq, input);
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    patch.ProcessAnalogControls();

    // ----------------------
    // CONTROL PROCESSING
    // ----------------------
    
    // 1. Time (Tape Speed)
    // Inverse curve: Lower values = Longer delay (Slower tape)
    float raw_time = patch.GetAdcValue(ADC_9) + patch.GetAdcValue(CV_1);
    raw_time = fclamp(raw_time, 0.0f, 1.0f);
    float target_delay_ms = 10.0f + (powf(raw_time, 2.5f) * 1500.0f); // Up to 1.5s
    float target_delay_samps = (target_delay_ms / 1000.0f) * sample_rate;

    // 2. Feedback (Intensity)
    float raw_fb = patch.GetAdcValue(CV_7) + patch.GetAdcValue(CV_2);
    // Gen~ script allows feedback > 1.0 for self oscillation
    float feedback_amt = fclamp(raw_fb * 1.1f, 0.0f, 1.2f); 

    // 3. Filter (Tone)
    float raw_filter = patch.GetAdcValue(ADC_10) + patch.GetAdcValue(CV_4) + patch.GetAdcValue(CV_6);
    // Maps roughly 400Hz to 18kHz
    float tone_freq = MapLog(raw_filter, 400.0f, 18000.0f);

    // 4. Flutter (Wow)
    float raw_flutter = patch.GetAdcValue(ADC_11) + patch.GetAdcValue(CV_5);
    float flutter_depth = fclamp(raw_flutter, 0.0f, 1.0f) * 60.0f; // Depth in samples

    // 5. Mix
    float raw_mix = patch.GetAdcValue(CV_8) + patch.GetAdcValue(CV_3);
    float dry_wet = fclamp(raw_mix, 0.0f, 1.0f);

    // ----------------------
    // AUDIO LOOP
    // ----------------------
    
    // Static state for ping-pong-ish feedback cross
    static float feedL = 0.0f;
    static float feedR = 0.0f;

    for (size_t i = 0; i < size; i++) {
        // Calculate Flutter
        // Gen~ uses complex "Capstan" logic. We approx this by summing two LFOs
        // to create a non-cyclic feeling wobble.
        float wob = flutterLfo.Process();
        float wob2 = flutterLfo2.Process(); 
        float total_flutter = (wob + (wob2 * 0.5f)) * flutter_depth;

        float delay_L_samps = fclamp(target_delay_samps + total_flutter, 10.0f, (float)MAX_DELAY - 100.0f);
        // Slight offset for Right channel for stereo width
        float delay_R_samps = fclamp(target_delay_samps + total_flutter + 50.0f, 10.0f, (float)MAX_DELAY - 100.0f);

        // Process Tape Heads
        // Note: We feed the *previous* output back into the input (Feedback)
        float outL = heads[0].Process(in[0][i], feedL * feedback_amt, delay_L_samps, tone_freq);
        float outR = heads[1].Process(in[1][i], feedR * feedback_amt, delay_R_samps, tone_freq);

        // Store Feedback for next cycle (Simple Cross Feed for stereo swirl)
        // Gen~ logic for feedback routing is complex, but often simple stereo is best for hardware
        feedL = outL; 
        feedR = outR;

        // Final Mix
        out[0][i] = (in[0][i] * (1.0f - dry_wet)) + (outL * dry_wet);
        out[1][i] = (in[1][i] * (1.0f - dry_wet)) + (outR * dry_wet);
    }
}

int main(void) {
    patch.Init();
    sample_rate = patch.AudioSampleRate();

    // Init Delays
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

    patch.StartAudio(AudioCallback);
    while(1) { }
}