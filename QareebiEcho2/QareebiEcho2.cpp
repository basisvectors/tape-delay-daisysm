/**
 * Digital Tape Delay for Electro-Smith Daisy Patch Submodule
 * 
 * HARDWARE CONNECTIONS:
 * ---------------------
 * Knobs (left to right):
 * 1. Time     -> ADC 9  (delay time 10ms-10000ms)
 * 2. Feedback -> CV 7   (feedback amount 0-95%)
 * 3. Mix      -> CV 8   (dry/wet mix)
 * 4. Filter   -> ADC 10 (lowpass filter cutoff)
 * 5. Flutter  -> ADC 11 (tape wow/flutter amount)
 * 
 * CV Input Jacks:
 * - CV 1-5    -> Modulate the parameters above
 * - CV 6      -> Filter Cutoff CV
 * 
 * Control:
 * - Gate In 1 -> Clock/Tap Tempo input
 * - Button D1 -> Manual tap tempo
 * - Button D2 -> Ping-pong mode toggle
 * - LED B8    -> Tempo indicator (PWM)
 * 
 * Audio:
 * - Audio In  -> L/R stereo input
 * - Audio Out -> L/R stereo output
 */

#include "daisy_patch_sm.h"
#include "daisysp.h"
#include <cmath>

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;

// Hardware object
DaisyPatchSM patch;

// Delay time configuration - adjust these to change delay range
#define MAX_DELAY_TIME_SEC 2.0f  // Maximum delay time in seconds
#define MIN_DELAY_TIME_MS 10.0f   // Minimum delay time in milliseconds
#define DELAY_TIME_CURVE 3.0f     // Curve exponent (higher = more control at short times)

// Maximum delay buffer size
#define MAX_DELAY static_cast<size_t>(48000 * MAX_DELAY_TIME_SEC)

// DSP modules - delay lines in SDRAM
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delMems[2];

// Delay structure (adapted from MultiDelay)
struct delay
{
    DelayLine<float, MAX_DELAY> *del;
    float                        currentDelay;
    float                        delayTarget;
    float                        dc_block_x1;  // DC blocker state
    float                        dc_block_y1;

    void Init()
    {
        dc_block_x1 = 0.0f;
        dc_block_y1 = 0.0f;
    }

    float Process(float feedback, float in)
    {
        //set delay times with smoothing
        fonepole(currentDelay, delayTarget, .0002f);
        del->SetDelay(currentDelay);

        float read = del->Read();
        
        // DC blocking filter to prevent DC offset accumulation in feedback
        // y[n] = x[n] - x[n-1] + 0.995 * y[n-1]
        float dc_blocked = read - dc_block_x1 + 0.995f * dc_block_y1;
        dc_block_x1 = read;
        dc_block_y1 = dc_blocked;
        
        // Soft limit feedback to prevent runaway (tanh saturation)
        float feedback_signal = feedback * dc_blocked;
        feedback_signal = tanhf(feedback_signal * 1.2f) * 0.95f;  // Gentler limiting
        
        del->Write(feedback_signal + in);

        return dc_blocked;
    }
};

delay delays[2];  // Left and Right channels

// Filters and effects
Svf lowpass_left, lowpass_right;
Oscillator lfo_flutter;
GPIO led_pin;

// Control variables
float delay_time_samples = 24000.0f;  // Default ~500ms at 48kHz
float feedback_amount = 0.5f;
float filter_cutoff = 8000.0f;
float flutter_amount = 0.0f;
float dry_wet_mix = 0.5f;

// CV modulation values
float cv_mod[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// Button and tempo control
Switch tap_button, mode_button;
bool ping_pong_mode = false;
uint32_t last_tap_time = 0;
uint32_t tap_count = 0;
float tap_tempo_ms = 500.0f;
bool use_tap_tempo = false;

// LED brightness for tempo indication
float led_brightness = 0.0f;
uint32_t last_led_toggle = 0;

// Helper function to map CV input to parameter range
inline float MapCvInput(float cv_value, float min_val, float max_val) {
    float normalized = fclamp(cv_value, 0.0f, 1.0f);
    return min_val + normalized * (max_val - min_val);
}

void ProcessControls() {
    // Read all analog controls
    patch.ProcessAnalogControls();
    
    // Process gate inputs for clock sync
    if (patch.gate_in_1.Trig()) {
        uint32_t current_time = System::GetNow();
        if (tap_count > 0 && (current_time - last_tap_time) < 10000) {
            tap_tempo_ms = current_time - last_tap_time;
            tap_tempo_ms = fclamp(tap_tempo_ms, 10.0f, 10000.0f);
            delay_time_samples = (tap_tempo_ms / 1000.0f) * patch.AudioSampleRate();
            use_tap_tempo = true;
        }
        last_tap_time = current_time;
        tap_count++;
    }
    
    // Read knob values
    float knob_time = patch.GetAdcValue(ADC_9);
    float knob_feedback = patch.GetAdcValue(CV_7);
    float knob_mix = patch.GetAdcValue(CV_8);
    float knob_filter = patch.GetAdcValue(ADC_10);
    float knob_flutter = patch.GetAdcValue(ADC_11);
    
    // Read CV inputs for modulation
    cv_mod[0] = patch.GetAdcValue(CV_1);  // Time CV
    cv_mod[1] = patch.GetAdcValue(CV_2);  // Feedback CV
    cv_mod[2] = patch.GetAdcValue(CV_3);  // Mix CV
    cv_mod[3] = patch.GetAdcValue(CV_4);  // Filter CV
    cv_mod[4] = patch.GetAdcValue(CV_5);  // Flutter CV
    
    // Filter cutoff from CV6 jack
    float cv_filter = patch.GetAdcValue(CV_6);
    
    // Calculate final parameter values with CV modulation
    float final_time = fclamp(knob_time + cv_mod[0] * 0.5f, 0.0f, 1.0f);
    // Scale feedback to be between 0 and 2
    float final_feedback = fclamp((knob_feedback + cv_mod[1] * 0.3f) * 1.2f, 0.0f, 2.0f);
    float final_mix = fclamp(knob_mix + cv_mod[2] * 0.3f, 0.0f, 1.0f);
    float final_filter = fclamp(knob_filter + cv_mod[3] * 0.3f + cv_filter * 0.3f, 0.0f, 1.0f);
    float final_flutter = fclamp(knob_flutter + cv_mod[4] * 0.2f, 0.0f, 1.0f);
    
    // Map parameters to useful ranges
    if (!use_tap_tempo) {
        // Apply curve for more control at shorter delay times
        // Using cubic curve (or configurable exponent) for finer control at low values
        float time_curve = powf(final_time, DELAY_TIME_CURVE);
        
        // Map from minimum to maximum delay time
        float min_delay_samples = patch.AudioSampleRate() * (MIN_DELAY_TIME_MS / 1000.0f);
        float max_delay_samples = MAX_DELAY - 1;
        delay_time_samples = MapCvInput(time_curve, min_delay_samples, max_delay_samples);
    }
    
    feedback_amount = final_feedback;
    filter_cutoff = MapCvInput(final_filter, 80.0f, 12000.0f);
    flutter_amount = final_flutter * 0.05f;
    dry_wet_mix = final_mix;
    
    // Update filter cutoffs
    lowpass_left.SetFreq(filter_cutoff);
    lowpass_right.SetFreq(filter_cutoff);
    
    // Update delay targets
    delays[0].delayTarget = delay_time_samples;
    delays[1].delayTarget = delay_time_samples;
    
    // Process buttons
    tap_button.Debounce();
    mode_button.Debounce();
    
    // Handle tap tempo
    if (tap_button.RisingEdge()) {
        uint32_t current_time = System::GetNow();
        if (tap_count > 0 && (current_time - last_tap_time) < 10000) {
            tap_tempo_ms = current_time - last_tap_time;
            tap_tempo_ms = fclamp(tap_tempo_ms, 10.0f, 10000.0f);
            delay_time_samples = (tap_tempo_ms / 1000.0f) * patch.AudioSampleRate();
            use_tap_tempo = true;
            tap_count++;
        } else {
            tap_count = 1;
            use_tap_tempo = false;
        }
        last_tap_time = current_time;
    }
    
    // Handle mode toggle
    if (mode_button.RisingEdge()) {
        ping_pong_mode = !ping_pong_mode;
    }
    
    // Reset tap tempo if time knob is moved significantly
    static float last_knob_time = 0.0f;
    if (fabs(knob_time - last_knob_time) > 0.05f) {
        use_tap_tempo = false;
    }
    last_knob_time = knob_time;
}

void ProcessLED() {
    // Calculate LED blink rate based on delay time
    float delay_time_ms = (delay_time_samples / patch.AudioSampleRate()) * 1000.0f;
    uint32_t current_time = System::GetNow();
    
    // Toggle LED at delay tempo
    if (current_time - last_led_toggle > delay_time_ms) {
        led_brightness = led_brightness > 0.5f ? 0.0f : 1.0f;
        last_led_toggle = current_time;
    }
    
    led_pin.Write(led_brightness > 0.5f);
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    ProcessControls();
    ProcessLED();
    
    for (size_t i = 0; i < size; i++) {
        float input_left = in[0][i];
        float input_right = in[1][i];
        
        // Apply fixed soft clipping to inputs (tanh saturation)
        float clipped_left = tanhf(input_left * 1.5f);
        float clipped_right = tanhf(input_right * 1.5f);
        
        // Pitch flutter: modulate delay time for tape-style wow/flutter
        float flutter_mod = lfo_flutter.Process() * flutter_amount;
        float modulated_delay_time = delay_time_samples * (1.0f + flutter_mod);
        modulated_delay_time = fclamp(modulated_delay_time, 1.0f, MAX_DELAY - 1);
        
        // Update delay targets with flutter modulation
        delays[0].delayTarget = modulated_delay_time;
        delays[1].delayTarget = modulated_delay_time;
        
        // Process delays with feedback (using MultiDelay structure)
        float delayed_left = delays[0].Process(feedback_amount, clipped_left);
        float delayed_right = delays[1].Process(feedback_amount, clipped_right);
        
        // Apply lowpass filtering to delayed signals
        lowpass_left.Process(delayed_left);
        delayed_left = lowpass_left.Low();
        lowpass_right.Process(delayed_right);
        delayed_right = lowpass_right.Low();
        
        // Ping-pong mode: cross-feed the delays
        if (ping_pong_mode) {
            float temp = delayed_left;
            delayed_left = (delayed_left + delayed_right) * 0.707f;  // -3dB
            delayed_right = (temp + delayed_right) * 0.707f;
        }
        
        // Mix dry and wet signals
        out[0][i] = input_left * (1.0f - dry_wet_mix) + delayed_left * dry_wet_mix;
        out[1][i] = input_right * (1.0f - dry_wet_mix) + delayed_right * dry_wet_mix;
    }
}

int main(void) {
    // Initialize hardware
    patch.Init();
    
    float sample_rate = patch.AudioSampleRate();
    
    // Initialize delay lines (MultiDelay style)
    for(int i = 0; i < 2; i++)
    {
        delMems[i].Init();
        delays[i].del = &delMems[i];
        delays[i].currentDelay = delay_time_samples;
        delays[i].delayTarget = delay_time_samples;
        delays[i].Init();  // Initialize DC blocker
    }
    
    // Initialize filters
    lowpass_left.Init(sample_rate);
    lowpass_right.Init(sample_rate);
    lowpass_left.SetFreq(filter_cutoff);
    lowpass_right.SetFreq(filter_cutoff);
    lowpass_left.SetRes(0.1f);
    lowpass_right.SetRes(0.1f);
    
    // Initialize flutter LFO
    lfo_flutter.Init(sample_rate);
    lfo_flutter.SetWaveform(Oscillator::WAVE_SIN);
    lfo_flutter.SetFreq(3.2f);
    lfo_flutter.SetAmp(1.0f);
    
    // Initialize LED pin (B8)
    led_pin.Init(DaisyPatchSM::B8, GPIO::Mode::OUTPUT);
    
    // Initialize buttons
    tap_button.Init(DaisyPatchSM::D1, patch.AudioCallbackRate());
    mode_button.Init(DaisyPatchSM::D2, patch.AudioCallbackRate());
    
    // Start audio processing
    patch.StartAudio(AudioCallback);
    
    // Main loop
    while(1) {
        System::Delay(1);
    }
}
