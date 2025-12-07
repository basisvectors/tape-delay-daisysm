/**
 * Hardware Test for Daisy Patch SM Custom Module
 * 
 * TEST FUNCTIONS:
 * ---------------
 * BUTTONS:
 * - D1 Button   -> Turns LED ON when pressed
 * - D2 Button   -> Turns LED OFF when pressed
 * 
 * KNOBS (LED Brightness Controls):
 * - ADC 9  (Knob 1) -> LED brightness control
 * - CV 7   (Knob 2) -> LED brightness control
 * - CV 8   (Knob 3) -> LED brightness control
 * - ADC 10 (Knob 4) -> LED brightness control
 * - ADC 11 (Knob 5) -> LED brightness control
 * 
 * CV INPUTS (LED Brightness Control):
 * - CV 1-5 -> Control LED brightness via PWM (averaged)
 * - CV 6   -> Additional LED brightness control
 * 
 * GATE/CLOCK INPUT:
 * - Gate In 1 -> Makes LED blink on each trigger
 * 
 * AUDIO:
 * - Audio In L/R -> Passed through to Audio Out L/R (unity gain)
 */

#include "daisy_patch_sm.h"
#include "daisysp.h"

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;

DaisyPatchSM hw;

// Buttons
Switch button_d1, button_d2;

// LED control (B8 pin)
GPIO led_pin;
bool led_state = false;
bool gate_blink = false;
uint32_t gate_blink_timer = 0;
const uint32_t BLINK_DURATION = 100; // ms

// Knob values
float knob_values[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

// CV values for LED brightness
float cv_brightness[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
	// Process all analog and digital controls
	hw.ProcessAllControls();
	
	// Read all 5 knobs
	knob_values[0] = hw.GetAdcValue(ADC_9);   // Knob 1
	knob_values[1] = hw.GetAdcValue(CV_7);    // Knob 2
	knob_values[2] = hw.GetAdcValue(CV_8);    // Knob 3
	knob_values[3] = hw.GetAdcValue(ADC_10);  // Knob 4
	knob_values[4] = hw.GetAdcValue(ADC_11);  // Knob 5
	
	// Read all 6 CV inputs for LED brightness control
	cv_brightness[0] = hw.GetAdcValue(CV_1);
	cv_brightness[1] = hw.GetAdcValue(CV_2);
	cv_brightness[2] = hw.GetAdcValue(CV_3);
	cv_brightness[3] = hw.GetAdcValue(CV_4);
	cv_brightness[4] = hw.GetAdcValue(CV_5);
	cv_brightness[5] = hw.GetAdcValue(CV_6);
	
	// Process buttons
	button_d1.Debounce();
	button_d2.Debounce();
	
	// Button D1 turns LED ON
	if (button_d1.Pressed()) {
		led_state = true;
	}
	
	// Button D2 turns LED OFF
	if (button_d2.Pressed()) {
		led_state = false;
	}
	
	// Gate input blinks LED
	if (hw.gate_in_1.Trig()) {
		gate_blink = true;
		gate_blink_timer = System::GetNow();
	}
	
	// Turn off gate blink after duration
	if (gate_blink && (System::GetNow() - gate_blink_timer > BLINK_DURATION)) {
		gate_blink = false;
	}
	
	// Calculate combined LED brightness from CV inputs (average them)
	float cv_led_brightness = 0.0f;
	for (int i = 0; i < 6; i++) {
		cv_led_brightness += cv_brightness[i];
	}
	cv_led_brightness /= 6.0f; // Average
	
	// Add knob values to LED brightness (average all inputs)
	float knob_brightness = 0.0f;
	for (int i = 0; i < 5; i++) {
		knob_brightness += knob_values[i];
	}
	knob_brightness /= 5.0f; // Average
	
	// Combine CV and knob brightness (average them)
	float total_brightness = (cv_led_brightness + knob_brightness) * 0.5f;
	
	// Set LED state - gate blink overrides, then button state, then brightness
	if (gate_blink) {
		led_pin.Write(true);
	} else if (led_state) {
		led_pin.Write(true);
	} else {
		// Use combined brightness for PWM-like control
		// Since GPIO is binary, we'll blink rapidly based on brightness
		static uint32_t pwm_counter = 0;
		pwm_counter++;
		led_pin.Write((pwm_counter % 100) < (total_brightness * 100));
	}
	
	// Process audio - simple passthrough
	for (size_t i = 0; i < size; i++)
	{
		OUT_L[i] = IN_L[i];
		OUT_R[i] = IN_R[i];
	}
}

int main(void)
{
	// Initialize hardware
	hw.Init();
	hw.SetAudioBlockSize(4);
	hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
	
	// Initialize LED pin (B8)
	led_pin.Init(DaisyPatchSM::B8, GPIO::Mode::OUTPUT);
	
	// Initialize buttons
	button_d1.Init(DaisyPatchSM::D1, hw.AudioCallbackRate());
	button_d2.Init(DaisyPatchSM::D2, hw.AudioCallbackRate());
	
	// Start audio processing
	hw.StartAudio(AudioCallback);
	
	// Main loop - just keep running
	while(1) {
		System::Delay(1);
	}
}
