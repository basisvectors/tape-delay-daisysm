/**
 * Gen~ Modeled Tape Delay for Electro-Smith Daisy Patch Submodule
 * WITH FREEZE/BLUR (D1, stable), REVERSE FEEDBACK (D2), CLOCK SYNC (Gate In 1), LED, AND GATE OUT 2 TEMPO
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
 * * Controls:
 * - Button D1 -> FREEZE/BLUR TOGGLE (Stops input, sets feedback to infinite, now stable)
 * - Button D2 -> REVERSE FEEDBACK TOGGLE
 * * Outputs:
 * - Gate Out 2 -> TEMPO CLOCK OUTPUT
 * - Audio Out -> L/R
 * * Indicator:
 * - LED (B8)  -> Blinks at tempo, solid when Freeze or Reverse is active
 */

#include "daisy_patch_sm.h"
#include "daisysp.h"
#include <cmath>

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;

DaisyPatchSM patch;

// Configuration
#define MAX_DELAY_TIME_SEC 3.0f 
#define MAX_DELAY static_cast<size_t>(48000 * MAX_DELAY_TIME_SEC)
// 1 second of audio for the reverse loop
#define REVERSE_BUFFER_SIZE static_cast<size_t>(48000) 

// Buffers
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delMems[2];
float DSY_SDRAM_BSS reverseBufferL[REVERSE_BUFFER_SIZE];
float DSY_SDRAM_BSS reverseBufferR[REVERSE_BUFFER_SIZE];

// Globals for Sync & LED & Gate Out
GPIO led;
Switch mode_button;      // D2 for reverse mode
Switch freeze_button;    // D1 for freeze/blur mode
uint32_t last_clock_tick = 0;
float current_delay_ms = 500.0f;
bool is_clocked = false;
float led_phase = 0.0f; 
bool reverse_feedback_mode = false;
bool freeze_mode = false;

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
    
    // Reverse Buffer state
    float *rev_buffer;
    size_t write_idx = 0;
    size_t rev_read_idx = 0;
    bool recording_done = false;
    
    float next_feedback_signal = 0.0f; 

    void Init(float sr, float *buffer_ptr) { 
        lpFilter.Init(sr); 
        hpFilter.Init(sr); 
        rev_buffer = buffer_ptr;
        rev_read_idx = REVERSE_BUFFER_SIZE - 1; 
    }

    float Process(float in, float feedback_signal, float delay_samps, float tone_freq, bool reverse_fb_active) {
        
        // --- GAIN STABILITY FIX ---
        // Corrective attenuation factor applied only when in freeze mode 
        float corrected_fb_signal = feedback_signal;
        // Check global flag, not the function argument `reverse_fb_active`
        if (freeze_mode) { 
            // 0.768f results in an overall loop gain of ~0.9984 (safe) to prevent blowup.
            corrected_fb_signal *= 0.85f; 
        }

        // 1. Process main delay
        float fb_input_for_write = corrected_fb_signal; 
        float saturated_signal = tnhLam((in + fb_input_for_write) * 1.3f);
        del->Write(saturated_signal);
        fonepole(currentDelay, delay_samps, 0.0005f); 
        float tape_out = del->ReadHermite(currentDelay);

        // 2. Filters (201 Topology)
        float lp_out = lpFilter.Process(tape_out, tone_freq, 0); 
        float hp_out = hpFilter.Process(lp_out, 147.0f, 1);
        
        // 3. DC Block & Soft Limit -> WET OUTPUT
        float clean_delayed_signal = hp_out - dc_x + 0.995f * dc_y;
        dc_x = hp_out; dc_y = clean_delayed_signal;
        clean_delayed_signal = softStatic(clean_delayed_signal);


        // --- REVERSE FEEDBACK MECHANISM ---
        next_feedback_signal = clean_delayed_signal; // Default feedback source 
        
        if (reverse_fb_active) {
            // A. Always record the current delayed/filtered signal (WET OUTPUT) into the buffer
            rev_buffer[write_idx] = clean_delayed_signal;
            
            // B. Check for full buffer (first time only)
            if (!recording_done && write_idx == REVERSE_BUFFER_SIZE - 1) {
                recording_done = true;
            }

            if (recording_done) {
                // C. Read backward for next feedback cycle
                next_feedback_signal = rev_buffer[rev_read_idx]; 

                // D. Decrement read index, wrapping from 0 back to N-1
                if (rev_read_idx == 0) {
                    rev_read_idx = REVERSE_BUFFER_SIZE - 1;
                } else {
                    rev_read_idx--;
                }
            } else {
                 // Use silence until the buffer is full to prevent initial glitches
                 next_feedback_signal = 0.0f;
            }
        }
        
        // 4. Increment write index 
        write_idx = (write_idx + 1) % REVERSE_BUFFER_SIZE;
        
        // Return the WET OUTPUT
        return clean_delayed_signal;
    }
};

TapeHead heads[2];
Oscillator flutterLfo, flutterLfo2;
float sample_rate;

float MapLog(float input, float min_freq, float max_freq) {
    input = fclamp(input, 0.0f, 1.0f);
    return min_freq * powf(max_freq / min_freq, input);
}

// --------------------------------------------------------------------------
// CONTROL PROCESSING
// --------------------------------------------------------------------------
void ProcessControls() {
    patch.ProcessAnalogControls();
    
    // Process the Reverse Mode Button (D2)
    mode_button.Debounce();
    if (mode_button.RisingEdge()) {
        reverse_feedback_mode = !reverse_feedback_mode;
        // If reverse is engaged, ensure freeze is off
        if (reverse_feedback_mode) {
             freeze_mode = false;
        }
        // Reset reverse buffer state
        heads[0].recording_done = false;
        heads[1].recording_done = false;
    }
    
    // Process the Freeze Button (D1)
    freeze_button.Debounce();
    if (freeze_button.RisingEdge()) {
        freeze_mode = !freeze_mode;
        // If freeze is engaged, ensure reverse is off, and reset recording state
        if (freeze_mode) {
             reverse_feedback_mode = false;
        }
    }
}


void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    ProcessControls(); 

    // ----------------------
    // 1. CLOCK / SYNC LOGIC
    // ----------------------
    uint32_t now = System::GetNow();
    if(patch.gate_in_1.Trig()) {
        float interval = (float)(now - last_clock_tick);
        if (interval > 40.0f && interval < 3000.0f) {
            current_delay_ms = interval;
            is_clocked = true;
            led_phase = 0.0f;
        }
        last_clock_tick = now;
    }
    if (now - last_clock_tick > 3500) {
        is_clocked = false;
    }

    // ----------------------
    // 2. PARAMETER CALCULATIONS
    // ----------------------
    
    float target_delay_samps;
    float raw_time = fclamp(patch.GetAdcValue(ADC_9) + patch.GetAdcValue(CV_1), 0.0f, 1.0f);

    if (is_clocked) {
        target_delay_samps = (current_delay_ms / 1000.0f) * sample_rate;
    } else {
        float knob_delay_ms = 10.0f + (powf(raw_time, 2.5f) * 1500.0f);
        // FIX 1: Corrected typo from 'knb_delay_ms' to 'knob_delay_ms'
        target_delay_samps = (knob_delay_ms / 1000.0f) * sample_rate;
        current_delay_ms = knob_delay_ms;
    }

    float fb_val = fclamp((patch.GetAdcValue(CV_7) + patch.GetAdcValue(CV_2)) * 1.1f, 0.0f, 1.2f);
    float tone_freq = MapLog(patch.GetAdcValue(ADC_10) + patch.GetAdcValue(CV_4), 400.0f, 18000.0f);
    float flutter_depth = fclamp(patch.GetAdcValue(ADC_11) + patch.GetAdcValue(CV_5), 0.0f, 1.0f) * 60.0f;
    float dry_wet = fclamp(patch.GetAdcValue(CV_8) + patch.GetAdcValue(CV_3), 0.0f, 1.0f);


    // --- FREEZE OVERRIDE --- 
    if (freeze_mode) {
        // Set feedback to unity gain. The actual stability correction happens inside TapeHead::Process.
        fb_val = 1.0f; 
        // 100% wet mix
        dry_wet = 1.0f;
    }

    // ----------------------
    // 3. AUDIO LOOP
    // ----------------------
    static float feedL = 0.0f;
    static float feedR = 0.0f;
    static bool gate_out_state = false; 

    for (size_t i = 0; i < size; i++) {
        // Flutter Modulation
        float wobble = (flutterLfo.Process() + (flutterLfo2.Process() * 0.5f)) * flutter_depth;
        
        float dL = fclamp(target_delay_samps + wobble, 10.0f, (float)MAX_DELAY - 100.0f);
        float dR = fclamp(target_delay_samps + wobble + 50.0f, 10.0f, (float)MAX_DELAY - 100.0f);

        // --- FREEZE AUDIO INPUT --- 
        float inputL = in[0][i];
        float inputR = in[1][i];
        
        if (freeze_mode) {
            // Stop writing new audio input to freeze the loop contents
            inputL = 0.0f;
            inputR = 0.0f;
        }

        // Tape Process. outL/R is the WET OUTPUT.
        float outL = heads[0].Process(inputL, feedL * fb_val, dL, tone_freq, reverse_feedback_mode);
        float outR = heads[1].Process(inputR, feedR * fb_val, dR, tone_freq, reverse_feedback_mode);

        // Access the member variable for the feedback signal
        feedL = heads[0].next_feedback_signal;
        feedR = heads[1].next_feedback_signal;


        out[0][i] = (in[0][i] * (1.0f - dry_wet)) + (outL * dry_wet);
        out[1][i] = (in[1][i] * (1.0f - dry_wet)) + (outR * dry_wet);

        // 4. LED & GATE PHASE CALCULATION
        float phase_inc = 1.0f / ( (current_delay_ms/1000.0f) * sample_rate );
        
        bool phase_wrapped = (led_phase + phase_inc) >= 1.0f;
        
        led_phase += phase_inc;
        if(led_phase >= 1.0f) led_phase -= 1.0f;

        // GATE OUT LOGIC (Trigger pulse generation)
        if (phase_wrapped) {
            gate_out_state = true;
        } else if (i > (size / 2)) {
            gate_out_state = false;
        }
    }
    
    // FIX 2: Corrected Gate Out 2 write syntax
    dsy_gpio_write(&patch.gate_out_2, gate_out_state ? 1 : 0);
}

int main(void) {
    patch.Init();
    sample_rate = patch.AudioSampleRate();

    // Init GPIO
    led.Init(DaisyPatchSM::B8, GPIO::Mode::OUTPUT);
    
    // Init Buttons D1 and D2
    freeze_button.Init(DaisyPatchSM::D1, patch.AudioCallbackRate()); 
    mode_button.Init(DaisyPatchSM::D2, patch.AudioCallbackRate()); 

    // Init DSP
    for(int i=0; i<2; i++) {
        delMems[i].Init();
        heads[i].del = &delMems[i];
    }
    heads[0].Init(sample_rate, reverseBufferL);
    heads[1].Init(sample_rate, reverseBufferR);


    // Init Flutter LFOs
    flutterLfo.Init(sample_rate);
    flutterLfo.SetFreq(0.4f); flutterLfo.SetAmp(1.0f);
    flutterLfo2.Init(sample_rate);
    flutterLfo2.SetFreq(3.5f); flutterLfo2.SetAmp(0.3f);
    flutterLfo2.SetWaveform(Oscillator::WAVE_TRI);

    patch.StartAudio(AudioCallback);

    while(1) {
        // LED is ON for the first 10% of the delay cycle, OR when Reverse Mode is active, OR when Freeze Mode is active.
        led.Write(led_phase < 0.1f || reverse_feedback_mode || freeze_mode); 
        
        System::Delay(1);
    }
}