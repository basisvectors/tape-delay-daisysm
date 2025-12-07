/**
 * Digital Tape Delay for Electro-Smith Daisy Patch Submodule
 * 
 * HARDWARE CONNECTIONS:
 * ---------------------
 * Knobs (left to right):
 * 1. Time     -> ADC 9  (delay time 10ms-10000ms)
 * 2. Feedback -> CV 7   (feedback amount 0-95%)
 * 3. Mix      -> CV 8   (dry/wet mix)
 * 4. Drive    -> ADC 10 (input drive/saturation)
 * 5. Flutter  -> ADC 11 (tape wow/flutter amount)
 * 
 * CV Input Jacks:
 * - CV 1-5    -> Modulate the parameters above
 * - CV 6      -> Filter Cutoff (lowpass filter control)
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

// Maximum delay time: 10 seconds at 48kHz to utilize 64MB flash
#define MAX_DELAY static_cast<size_t>(48000 * 10.0f)

// DSP modules
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_left;
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_right;
Svf lowpass_left, lowpass_right;
Overdrive drive_left, drive_right;
Oscillator lfo_flutter;
Metro tempo_clock;

// Control variables
float delay_time_ms = 500.0f;
float feedback_amount = 0.5f;
float filter_cutoff = 8000.0f;
float drive_gain = 0.0f;
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
float led_brightness = 0.0f;

// Parameter smoothing
float smooth_delay_time = 500.0f;
float smooth_feedback = 0.5f;
float smooth_cutoff = 8000.0f;
float smooth_drive = 0.0f;
float smooth_flutter = 0.0f;
float smooth_dry_wet = 0.5f;

// Helper function for parameter smoothing
inline void SmoothParameter(float &smooth_val, float target, float slew_rate) {
    smooth_val += (target - smooth_val) * slew_rate;
}

// Helper function to map CV input (-5V to +5V) to parameter range
inline float MapCvInput(float cv_value, float min_val, float max_val) {
    // CV input is normalized to -1.0 to 1.0, map to 0.0 to 1.0
    float normalized = (cv_value + 1.0f) * 0.5f;
    normalized = fclamp(normalized, 0.0f, 1.0f);
    return min_val + normalized * (max_val - min_val);
}

void ProcessControls() {
    // Read all analog controls
    patch.ProcessAnalogControls();
    
    // Process gate inputs for clock sync
    if (patch.gate_in_1.Trig()) {
        uint32_t current_time = System::GetNow();
        if (tap_count > 0 && (current_time - last_tap_time) < 10000) {  // Within 10 seconds
            tap_tempo_ms = current_time - last_tap_time;
            tap_tempo_ms = fclamp(tap_tempo_ms, 10.0f, 10000.0f);  // Extended range
            delay_time_ms = tap_tempo_ms;
            use_tap_tempo = true;
        }
        last_tap_time = current_time;
        tap_count++;
    }
    
    // Read knob values
    float knob_time = patch.GetAdcValue(ADC_9);       // Time knob
    float knob_feedback = patch.GetAdcValue(CV_7);    // Feedback knob
    float knob_mix = patch.GetAdcValue(CV_8);         // Mix knob (dry/wet)
    float knob_drive = patch.GetAdcValue(ADC_10);     // Drive knob
    float knob_flutter = patch.GetAdcValue(ADC_11);   // Flutter knob
    
    // Read CV inputs for modulation
    cv_mod[0] = patch.GetAdcValue(CV_1);  // Time CV
    cv_mod[1] = patch.GetAdcValue(CV_2);  // Feedback CV
    cv_mod[2] = patch.GetAdcValue(CV_3);  // Mix CV
    cv_mod[3] = patch.GetAdcValue(CV_4);  // Drive CV
    cv_mod[4] = patch.GetAdcValue(CV_5);  // Flutter CV
    
    // Filter cutoff from CV6 jack
    float cv_filter = patch.GetAdcValue(CV_6);
    
    // Calculate final parameter values with CV modulation
    float final_time = knob_time + cv_mod[0] * 0.5f;  // CV adds ±50% modulation
    final_time = fclamp(final_time, 0.0f, 1.0f);
    
    float final_feedback = knob_feedback + cv_mod[1] * 0.3f;  // CV adds ±30% modulation
    final_feedback = fclamp(final_feedback, 0.0f, 0.95f);  // Limit to prevent runaway feedback
    
    float final_mix = knob_mix + cv_mod[2] * 0.3f;  // Mix with CV modulation
    final_mix = fclamp(final_mix, 0.0f, 1.0f);
    
    float final_drive = knob_drive + cv_mod[3] * 0.3f;
    final_drive = fclamp(final_drive, 0.0f, 1.0f);
    
    float final_flutter = knob_flutter + cv_mod[4] * 0.2f;
    final_flutter = fclamp(final_flutter, 0.0f, 1.0f);
    
    // Filter controlled by CV6 with exponential response
    float final_filter = 0.5f + cv_filter * 0.5f;  // Normalize to 0.0 to 1.0
    final_filter = fclamp(final_filter, 0.0f, 1.0f);
    
    // Map parameters to useful ranges
    if (!use_tap_tempo) {
        delay_time_ms = MapCvInput(final_time * 2.0f - 1.0f, 10.0f, 10000.0f);  // 10ms to 10000ms (10 seconds)
    }
    feedback_amount = final_feedback;
    filter_cutoff = MapCvInput(final_filter * 2.0f - 1.0f, 200.0f, 12000.0f);  // 200Hz to 12kHz
    drive_gain = final_drive * 3.0f;  // 0 to 3x gain
    flutter_amount = final_flutter * 0.05f;  // 0 to 5% flutter
    
    // Dry/wet mix from knob 3
    dry_wet_mix = final_mix;
    
    // Process buttons
    tap_button.Debounce();
    mode_button.Debounce();
    
    // Handle tap tempo
    if (tap_button.RisingEdge()) {
        uint32_t current_time = System::GetNow();
        if (tap_count > 0 && (current_time - last_tap_time) < 10000) {  // Within 10 seconds
            tap_tempo_ms = current_time - last_tap_time;
            tap_tempo_ms = fclamp(tap_tempo_ms, 10.0f, 10000.0f);  // Extended range
            delay_time_ms = tap_tempo_ms;
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
    
    // Smooth parameter changes
    SmoothParameter(smooth_delay_time, delay_time_ms, 0.001f);
    SmoothParameter(smooth_feedback, feedback_amount, 0.01f);
    SmoothParameter(smooth_cutoff, filter_cutoff, 0.005f);
    SmoothParameter(smooth_drive, drive_gain, 0.01f);
    SmoothParameter(smooth_flutter, flutter_amount, 0.01f);
    SmoothParameter(smooth_dry_wet, dry_wet_mix, 0.01f);
}

void ProcessLED() {
    // Blink LED with tempo
    bool led_state = tempo_clock.Process();
    
    if (led_state) {
        led_brightness = 1.0f;
    } else {
        led_brightness *= 0.95f;  // Fade out
    }
    
    // Update LED brightness on B8 - using user LED for simplicity
    patch.SetLed(led_brightness > 0.1f);
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    // Process controls at audio rate
    ProcessControls();
    ProcessLED();
    
    for (size_t i = 0; i < size; i++) {
        float input_left = in[0][i];
        float input_right = in[1][i];
        
        // Apply input drive/saturation
        float driven_left = drive_left.Process(input_left * (1.0f + smooth_drive));
        float driven_right = drive_right.Process(input_right * (1.0f + smooth_drive));
        
        // Calculate delay time in samples with flutter modulation
        float flutter_mod = lfo_flutter.Process() * smooth_flutter;
        float delay_samples = (smooth_delay_time / 1000.0f) * patch.AudioSampleRate();
        delay_samples = delay_samples * (1.0f + flutter_mod);
        delay_samples = fclamp(delay_samples, 1.0f, MAX_DELAY - 1);
        
        // Set delay times
        delay_left.SetDelay(delay_samples);
        delay_right.SetDelay(delay_samples);
        
        // Read delayed signals
        float delayed_left = delay_left.Read();
        float delayed_right = delay_right.Read();
        
        // Apply lowpass filtering to delayed signals
        lowpass_left.Process(delayed_left);
        delayed_left = lowpass_left.Low();
        lowpass_right.Process(delayed_right);
        delayed_right = lowpass_right.Low();
        
        // Ping-pong mode: cross-feed the delays
        if (ping_pong_mode) {
            float temp = delayed_left;
            delayed_left = (delayed_left + delayed_right) * 0.5f;
            delayed_right = (temp + delayed_right) * 0.5f;
        }
        
        // Write new samples to delay lines (input + feedback)
        delay_left.Write(driven_left + delayed_left * smooth_feedback);
        delay_right.Write(driven_right + delayed_right * smooth_feedback);
        
        // Mix dry and wet signals
        out[0][i] = input_left * (1.0f - smooth_dry_wet) + delayed_left * smooth_dry_wet;
        out[1][i] = input_right * (1.0f - smooth_dry_wet) + delayed_right * smooth_dry_wet;
    }
}

int main(void) {
    // Initialize hardware
    patch.Init();
    
    float sample_rate = patch.AudioSampleRate();
    
    // Initialize DSP modules
    delay_left.Init();
    delay_right.Init();
    
    // Initialize filters  
    lowpass_left.Init(sample_rate);
    lowpass_right.Init(sample_rate);
    lowpass_left.SetFreq(smooth_cutoff);
    lowpass_right.SetFreq(smooth_cutoff);
    lowpass_left.SetRes(0.1f);  // Low resonance for smooth lowpass
    lowpass_right.SetRes(0.1f);
    
    // Initialize overdrive
    drive_left.Init();
    drive_right.Init();
    drive_left.SetDrive(0.5f);
    drive_right.SetDrive(0.5f);
    
    // Initialize flutter LFO
    lfo_flutter.Init(sample_rate);
    lfo_flutter.SetWaveform(Oscillator::WAVE_SIN);
    lfo_flutter.SetFreq(3.2f);  // 3.2 Hz flutter
    lfo_flutter.SetAmp(1.0f);
    
    // Initialize tempo clock
    tempo_clock.Init(2.0f, sample_rate);  // 2 Hz default
    
    // Initialize buttons using pin definitions
    tap_button.Init(DaisyPatchSM::D1, patch.AudioCallbackRate());
    mode_button.Init(DaisyPatchSM::D2, patch.AudioCallbackRate());
    
    // Start audio processing
    patch.StartAudio(AudioCallback);
    
    // Main loop
    while(1) {
        // Update filter cutoff
        lowpass_left.SetFreq(smooth_cutoff);
        lowpass_right.SetFreq(smooth_cutoff);
        
        // Update tempo clock frequency
        float tempo_hz = 1000.0f / smooth_delay_time;
        tempo_clock.SetFreq(tempo_hz);
        
        // Brief delay to prevent excessive CPU usage
        System::Delay(1);
    }
}
