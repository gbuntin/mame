// license:BSD-3-Clause
// copyright-holders:m1macrophage

/*
The Moog Source is a CPU-controlled analog monosynth. It lacks knobs and
sliders. Sound parameters are modified by pressing a button for a specific
parameter and using the encoder wheel to modify it.

The architecture of this synthesizer is typical of digitally-controlled analog
synthesizers. The firmware is responsible for:
* Scanning and reacting to membrane button presses. This is a typical key
  matrix setup (see buttons_latch_w(), buttons_a_r(), buttons_b_r()).
* Detecting which key is pressed on the keyboard (see get_keyboard_v()).
* Setting Control Voltages (aka CVs). Detailed info in cv_w().
* Configuring audio and modulation routing through 4016 switches. See
  output_latch_a_w(), output_latch_b_w().
* Controlling the Loudness and Filter envelope generators (EGs). Starts the
  Attack phase when a key is pressed, detects when an EG peaks and transitions
  it to the Decay phase, and transitions EGs to the Release phase when a key is
  released.
* Controlling the LED displays and cassette I/O.

The 16 sound programs are stored in battery-backed NVRAM, and can also be stored
to (and loaded from) a cassette.

This driver is based on the Source's schematics. Most of the circuitry
relevant to this driver is on Board 3 (digital board). Component designations in
comments refer to Board 3, unless otherwise noted.

This driver attempts to accurately emulate the digital and digital-analog
interface of the synthesizer, including all analog behavior that is relevant to
the firmware. The analog audio circuit is not emulated. The driver includes an
interactive layout, and is intended as an educational tool.
*/

#include "emu.h"

#include "nl_source.h"

#include "cpu/z80/z80.h"
#include "machine/netlist.h"
#include "machine/nvram.h"
#include "machine/rescap.h"
#include "machine/timer.h"
#include "sound/va_eg.h"

#include "moog_source.lh"

#define LOG_CV                  (1U << 1)
#define LOG_BUTTONS             (1U << 2)
#define LOG_ENCODER             (1U << 3)
#define LOG_KEYBOARD            (1U << 4)
#define LOG_CV_KEYBOARD_APPROX  (1U << 5)
#define LOG_LFO                 (1U << 6)
#define LOG_LFO_TIMER           (1U << 7)
#define LOG_CONTOUR             (1U << 8)

#define VERBOSE (LOG_GENERAL | LOG_CV)
//#define LOG_OUTPUT_FUNC osd_printf_info

#include "logmacro.h"

namespace {

constexpr const char MAINCPU_TAG[] = "z80";
constexpr const char NVRAM_TAG[] = "nvram";

class source_state : public driver_device
{
public:
	static constexpr feature_type unemulated_features() { return feature::TAPE; }

	source_state(const machine_config &mconfig, device_type type, const char *tag) ATTR_COLD
		: driver_device(mconfig, type, tag)
		, m_maincpu(*this, MAINCPU_TAG)
		, m_contour(*this, "contour_%d", 0)
		, m_contour_rate(*this, "source_nl:cntr_rate_%d", 0)
		, m_contour_range(*this, "contour_range_%d",0)
		, m_lfo_timer(*this, "lfo_timer")
		, m_lfo_rate(*this, "source_nl:lfo_rate")
		, m_lfo_range(*this, "lfo_range")
		, m_octave_io(*this, "octave_buttons")
		, m_button_a_io(*this, "button_group_a_%d", 0U)
		, m_button_b_io(*this, "button_group_b_%d", 0U)
		, m_keyboard_io(*this, "keyboard_oct_%d", 1U)
		, m_encoder(*this, "incremental_controller")
		, m_trigger_io(*this, "trigger_in")
		, m_octave_led(*this, "octave_led_%d")
		, m_lfo_rate_led(*this, "mod_rate_led")
		, m_program_display(*this, "program_digit_%d")
		, m_edit_display(*this, "edit_digit_%d")
		, m_edit_led(*this, "edit_led")
		, m_kb_track(*this, "kb_track")
		, m_osc_waveform(*this, "osc_%d_waveform", 1U)
		, m_sync(*this, "sync")
		, m_lfo_to_filter(*this, "lfo_to_filter")
		, m_lfo_to_osc(*this, "lfo_to_osc")
		, m_lfo_shape(*this, "lfo_shape")
		, m_trigger_out(*this, "trigger_out")
		, m_cv(int(CV::SIZE), 0)
	{}

	void source(machine_config &config) ATTR_COLD;

	DECLARE_INPUT_CHANGED_MEMBER(octave_button_pressed);
	DECLARE_INPUT_CHANGED_MEMBER(encoder_moved);

protected:
	void machine_start() override ATTR_COLD;
	void machine_reset() override ATTR_COLD;

private:
	void update_octave_leds();

	void edit_latch_w(u8 data);
	void output_latch_a_w(u8 data);
	void output_latch_b_w(u8 data);
	void buttons_latch_w(u8 data);
	void program_latch_w(u8 data);
	void cassette_w(u8 data);
	void cv_w(offs_t offset, u8 data);

	bool contour_peaked(const va_rc_eg_device &eg) const;
	float get_keyboard_v() const;
	u8 keyboard_r();
	u8 buttons_r(const required_ioport_array<6> &button_io, const char *name) const;
	u8 buttons_a_r();
	u8 buttons_b_r();
	u8 encoder_r();

	template<int Which> NETDEV_ANALOG_CALLBACK_MEMBER(contour_cv_changed);
	template<int Which> TIMER_CALLBACK_MEMBER(update_contour);

	NETDEV_ANALOG_CALLBACK_MEMBER(lfo_cv_changed);
	TIMER_CALLBACK_MEMBER(update_lfo_timer);
	TIMER_DEVICE_CALLBACK_MEMBER(lfo_timer_tick);

	void memory_map(address_map &map) ATTR_COLD;
	void io_map(address_map &map) ATTR_COLD;

	required_device<z80_device> m_maincpu;

	required_device_array<va_rc_eg_device, 2> m_contour;
	required_device_array<netlist_mame_analog_input_device, 2> m_contour_rate;
	required_ioport_array<2> m_contour_range;

	required_device<timer_device> m_lfo_timer;
	required_device<netlist_mame_analog_input_device> m_lfo_rate;
	required_ioport m_lfo_range;

	required_ioport m_octave_io;
	required_ioport_array<6> m_button_a_io;
	required_ioport_array<6> m_button_b_io;
	required_ioport_array<4> m_keyboard_io;
	required_ioport m_encoder;
	required_ioport m_trigger_io;

	output_finder<2> m_octave_led;
	output_finder<> m_lfo_rate_led;
	output_finder<2> m_program_display;
	output_finder<2> m_edit_display;
	output_finder<> m_edit_led;
	output_finder<> m_kb_track;
	output_finder<2> m_osc_waveform;
	output_finder<> m_sync;
	output_finder<> m_lfo_to_filter;
	output_finder<> m_lfo_to_osc;
	output_finder<> m_lfo_shape;
	output_finder<> m_trigger_out;

	bool m_octave_hi = true;  // `true` due to internal pullups of 74LS367 and 7404.
	u8 m_button_row_latch = 0xff;
	bool m_encoder_incr = false;
	bool m_lfo_state = false;  // Square output of the LFO. -14V (false) to 14V (true).

	float m_lfo_cc = 0;  // Control current into the LFO OTA.
	std::array<float, 2> m_contour_cc = { 0, 0 };  // Control currents into the EG OTAs.
	std::vector<float> m_cv;

	enum contour_type
	{
		FILTER_CONTOUR = 0,
		LOUDNESS_CONTOUR
	};

	// All MUXes are CD4051B.
	// Component designations refer to board 2 (synthesizer board).
	// The enum names match the CV labels in the schematic, but some
	// abbreviations are expanded.
	enum class CV : int
	{
		// U2
		CUTOFF_COARSE = 0,
		AUTO_TUNE_2,
		INT_COARSE,
		CUTOFF_FINE,
		PW_1,
		PW_2,
		INT_FINE,
		OCT_2,

		// U4
		FILTER_CONTOUR_LEVEL,
		OCT_1,
		GLIDE,
		LOUDNESS_CONTOUR_LEVEL,
		OSC_2,
		NOISE,
		UNUSED,  // Sampled in (C22, U10A), but not used.
		OSC_1,

		// U5
		EMPHASIS,
		NOT_CONNECTED,  // U5, Y1 (pin 14) is not connected.
		AMT,  // Filter contout amount.
		MOD_RATE,  // Modulation (LFO) rate.
		KEYBOARD_APPROX,
		KEYBOARD_CV,
		FILTER_CONTOUR_RATE,
		LOUDNESS_CONTOUR_RATE,

		SIZE
	};

	static constexpr const float VMINUS = -15;  // In Volts.
	static constexpr const float MAX_CV = 10;  // In Volts.
	static constexpr const float CA3080_VABC = VMINUS + 0.7;  // 1 diode drop above -15.
	static constexpr const float CONTOUR_C = CAP_U(0.047);  // C57 (filter), C56 (loudness).
	static constexpr const u8 PATTERNS_7447[16] =
	{
		0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7c, 0x07,
		0x7f, 0x67, 0x58, 0x4c, 0x62, 0x69, 0x78, 0x00,
	};
};

void source_state::update_octave_leds()
{
	m_octave_led[0] = m_octave_hi ? 0 : 1;
	m_octave_led[1] = m_octave_hi ? 1 : 0;
}

void source_state::edit_latch_w(u8 data)
{
	// U3 (74LS378) 0-4 (D0-D4) -> U3 (7447, Board 4) A-D -> U4 (MAN 3610A).
	// U3 4-5 not connected.
	m_edit_display[0] = PATTERNS_7447[data & 0x0f];

	// U4 (74LS378) 0-4 (D4-D7)-> U5 (7447, Board 4) A-D -> U6 (MAN 3610A).
	m_edit_display[1] = PATTERNS_7447[(data >> 4) & 0x0f];

	// U4 (74LS378) 5, 6 (D0, D7) -> J1-5 (cassette interface, "cassette out").
	// TODO: Add cassette support.
}

void source_state::output_latch_a_w(u8 data)
{
	// Latch is U11, 74LS378 (Board 3). 6-bit latch, top 2 bits ignored.
	// All component designations are for Board 2.

	// Keyboard tracking for filter, bits D0 and D1.
	const u8 kb_track = data & 0x03;
	if (kb_track == 1)
	{
		// 1/2 tracking. U36C on and U36B off.
		// Keyboard CV mixed in via 342Kohm resistance (2 x 121K: R142, R143).
		m_kb_track = 1;
	}
	else if (kb_track == 2 || kb_track == 3)
	{
		// Full tracking.
		// Only U36B on (kb_track == 2), or both U36B and U36C on
		// (kb_track == 3).
		// In both cases, keyboard CV is mixed in via a 121KOhm resistor (R142).
		m_kb_track = 2;
	}
	else
	{
		// Both U36B and U36B are off. Keyboard CV does not make it through.
		m_kb_track = 0;
	}

	// Osc 2 waveform, bits D2 and D3.
	// 0 - Sawtooth (U32A closed, U32B open, U32C closed, U32D closed).
	// 1 - Triangle (U32A closed, U32B closed, U32D open, U32C open).
	// 2 - Square/pulse (U32A open, U32B closed, U32C open, U32D closed).
	// 3 - Mix of Triange and Square/Pulse (probably unused)
	//     (U32A open, U32B closed, U32C open, U32D open).
	m_osc_waveform[1] = (data >> 2) & 0x03;

	// Osc 1 waveform, bits D4 and D5.
	// Same swithc configuration as above, but replace U32 with U23.
	m_osc_waveform[0] = (data >> 4) & 0x03;
}

void source_state::output_latch_b_w(u8 data)
{
	// Latch is U12, 74LS378 (Board 3). 6-bit latch, top 2 bits ignored.

	// D0 -> S21-16 -> Sync. Synchronizes osc 2 to osc 1.
	// When sync is on, the pitch wheel is only routed to Osc 2.
	// When 1, Q2 is "off", U18B and C are off, U18A on, pitch wheel routed
	// to osc 2 only.
	// When 0, Q2 is "on", U18B is on (routes pitch wheel to central pitch),
	// U18C is "on", which turns off U18A (disables direct route to osc 2).
	m_sync = BIT(data, 0);

	// D1 -> inverted by U2A (Board 3), and connected to cathode of led.
	// The led is active low, but the inverter presents it as active high.
	m_edit_led = BIT(data, 1);

	// D2 -> J2 S-TRIG OUT, inverted via Q1, R21, R20 (Board 3).
	m_trigger_out = BIT(data, 2) ? 0 : 1;

	// Component designations below refer to Board 2.

	// D3 -> S22-4 -> U46A (inverted and level-shifted to -5-5V through 4007B
	// and R213): mod to filter.
	m_lfo_to_filter = BIT(data, 3) ? 0 : 1;

	// D4 -> S22-3 -> U18D (inverted and level-shifted to -5-5V through 4007B
	// and R61): mod to osc.
	m_lfo_to_osc = BIT(data, 4) ? 0 : 1;

	// D5 -> S22-2 (level shifted & inverted to -5-5V through 4007B and R214)
	//       0 - Triange (U46C on, U46B on, turns off U46D). -1.5V - 1.5V.
	//       1 - Square (U46C off, U46B off, U46D on/off controlled by square
	//           wave. Translates -14V - 14V wave to 0-5V.
	m_lfo_shape = BIT(data, 5);
}

void source_state::buttons_latch_w(u8 data)
{
	// U5, 74LS378. All output connected to diode cathodes.
	// Connected to "Membrane switch interface", P1, "top left".
	// (D0, D1, D2, D3, D4, D5) -> (P1-1, P1-5, P1-6, P1-2, P1-4, P1-3)
	m_button_row_latch = data & 0x3f;  // Only D0-D5 connected.
}

void source_state::program_latch_w(u8 data)
{
	// U1, 74LS378
	// D0-D3 -> U1 (7447, Board 4) A-D -> right digit of MAN6630.
	// D4 -> inverted (U2E, U2D, 7404) -> left 1/2 digit of MAN6630
	//       (inputs a and b).
	// D5 -> inverted (U2A, 7404) -> HOLD ->"plus" sign of MAN6630.
	// Note that MAN6630 has 3 "digits". From right to left:
	// - 7-segment digit.
	// - 2-segment digit (can represent a "1").
	// - "+" sign.
	// Here, we simulate this with two 7-segment digits.

	m_program_display[0] = PATTERNS_7447[data & 0x0f];

	u8 digit1 = PATTERNS_7447[15];  // All segments off.
	if (BIT(data, 4))
	{
		digit1 |= PATTERNS_7447[1];  // Turn on segments for "1".
	}
	if (BIT(data, 5))
	{
		// This enables two segments on the MAN6630 that display a "+" symbol.
		// Since 7-segment displays don't support that, enable the segment for
		// "-" instead.
		digit1 |= 0x40;
	}
	m_program_display[1] = digit1;
}

void source_state::cassette_w(u8 data)
{
	// Z80 D4 controlls a normally-open relay (K1) through U22A (74LS74).
	// A low D4 powers the relay, which connects cassette jack J1-1 to J1-3.
	// TODO: Add cassette support.
}

void source_state::cv_w(offs_t offset, u8 data)
{
	// CVs are generated by writing to an AM6012 12-bit DAC, but only 8 bits are
	// used: the 8 MSBs are connected to the data bus, and the 4 LSBs are
	// grounded. The DAC is mapped to the Z80's port IO space.
	// The DAC and support circuitry convert the 8 bit data (0-255) to a voltage
	// (0-10V). That voltage is routed to the Sample & Hold circuit (a
	// capacitor and a buffer) of a specific CV, controlled by A0-A4.

	// Interesting tidbit: In most designs, the DAC inputs are
	// latched. In the Source, the DAC inputs are directly connected to the data
	// bus. This means the DAC output voltage is constantly changing, in an
	// attempt to track the data bus.

	if (!machine().side_effects_disabled())
	{
		// U14, U15B, U16D,E and U17D generate WAIT states whenever there is
		// an IO write. This lasts 32 cycles.
		// For the first 8 cycles, no MUX is selected, to allow the DAC to
		// settle. Then a specific channel in a specific MUX is enabled (based
		// on the port address), and there's a WAIT for another 24 cycles, to
		// allow the selected Sample & Hold capacitor to (dis)charge.
		m_maincpu->adjust_icount(-(8 + 24));
	}

	// Z80 A0,A1,A2 connected to the A,B,C inputs (respectively) of all MUXes.
	// Z80 A3,A4 select which MUX to enable via decoder 74LS155.
	// The fourth output of the decoder is not connected. There are 3 muxes.

	if (offset >= offs_t(CV::SIZE))
		return;

	const float cv = MAX_CV * data / 255.0F;
	if (cv == m_cv[offset])
		return;
	m_cv[offset] = cv;

	switch (offset)
	{
		case offs_t(CV::MOD_RATE):
			m_lfo_rate->write(cv);
			break;
		case offs_t(CV::LOUDNESS_CONTOUR_RATE):
			m_contour_rate[LOUDNESS_CONTOUR]->write(cv);
			break;
		case offs_t(CV::LOUDNESS_CONTOUR_LEVEL):
			machine().scheduler().synchronize(timer_expired_delegate(FUNC(source_state::update_contour<LOUDNESS_CONTOUR>), this));
			break;
		case offs_t(CV::FILTER_CONTOUR_RATE):
			m_contour_rate[FILTER_CONTOUR]->write(cv);
			break;
		case offs_t(CV::FILTER_CONTOUR_LEVEL):
			machine().scheduler().synchronize(timer_expired_delegate(FUNC(source_state::update_contour<FILTER_CONTOUR>), this));
			break;
	}

	if (offset == offs_t(CV::KEYBOARD_APPROX))
		LOGMASKED(LOG_CV_KEYBOARD_APPROX, "CV %d: 0x%02x, %f\n", offset, data, cv);
	else
		LOGMASKED(LOG_CV, "CV %d: 0x%02x, %f\n", offset, data, cv);
}

bool source_state::contour_peaked(const va_rc_eg_device &eg) const
{
	// The peak detector circuits for the filter and loudness contour generators
	// are identical. They are located on board 2, and based on an LM393 comparator
	// (U41B and U41A respectively). The threshold is set to ~9.984V, with a
	// +-0.005V hysteresis. The EG output is connected to the inverting input.
	// The comparator output is 0V when the EG output is above the threshold, and
	// 5V otherwise (open collector output pulled to 5V via 10K resistors, R193
	// and R190 respectively).

	static constexpr const float R192 = RES_M(4.7);  // R188 for loudness EG.
	static constexpr const float R191 = RES_K(10);   // R189 for loudness EG.
	static constexpr const float RISING_THRESHOLD = 5 * RES_VOLTAGE_DIVIDER(R191, R192) + 5;
	static constexpr const float FALLING_THRESHOLD = 10 * RES_VOLTAGE_DIVIDER(R191, R192);
	static_assert(RISING_THRESHOLD > FALLING_THRESHOLD);

	const float eg_v = eg.get_v();
	if (eg_v > RISING_THRESHOLD)
	{
		return true;
	}
	else if (eg_v < FALLING_THRESHOLD)
	{
		return false;
	}
	else  // eg_v is within the hysteresis range.
	{
		// Proper emulation of hysteresis would require a streaming (or otherwise
		// stateful) comparator. But a heuristic tailored to this use case works
		// fine: if the EG voltage is falling, assume we hit the 'rising' threshold
		// in the past, so the 'falling' threshold is the active one.

		// This heuristic will pick the wrong threshold if a key is released while
		// the EG is within the hysteresis range. But that should be rare (the
		// hysteresis range is ~0.01V), and inconsequential. The difference in
		// thresholds is very small, and the firmware is the one that initiates
		// the EG release and is probably ignoring this input until the next
		// attack.

		const float future_eg_v = eg.get_v(machine().time() + attotime::from_msec(1));
		return future_eg_v < eg_v;
	}
}

float source_state::get_keyboard_v() const
{
	// *** Detect which key is pressed.

	static constexpr const int OCTAVES = 4;
	static constexpr const int KEYS_PER_OCTAVE = 12;
	static constexpr const int KEYS = 3 * KEYS_PER_OCTAVE + 1;
	static constexpr const int OCTAVE_KEYS[4] =
	{
		KEYS_PER_OCTAVE, KEYS_PER_OCTAVE, KEYS_PER_OCTAVE, 1
	};

	// The circuit is structure such that the lowest note has priority.
	// Scan from lowest, and exit the loop once a pressed key is found.
	int pressed_key = -1;
	for (int octave = 0; octave < OCTAVES; ++octave)
	{
		const u32 keys = m_keyboard_io[octave]->read();
		for (int key = 0; key < OCTAVE_KEYS[octave]; ++key)
		{
			if (BIT(keys, key))
			{
				pressed_key = octave * KEYS_PER_OCTAVE + key;
				break;
			}
		}
		if (pressed_key >= 0)
			break;
	}

	// *** Convert pressed key to a voltage.

	static constexpr const float KEYBOARD_VREF = 8.24F;  // From schematic.
	static constexpr const float RKEY = RES_R(100);
	static constexpr const float R74 = RES_R(150);
	static constexpr const float R76 = RES_K(220);
	static constexpr const float R77 = RES_K(2.2);

	float kb_voltage = 0;
	if (pressed_key >= 0)
	{
		// Pressing a key forms a voltage devider consisting of the lower and
		// upper resistances as shown below. The resulting voltage is further
		// reduced by another voltage divider (R77-R76) before being fed to
		// comparator U31A.
		const float lower_r = R74 + pressed_key * RKEY;
		const float upper_r = (KEYS - pressed_key - 1) * RKEY;
		const float v = KEYBOARD_VREF * RES_VOLTAGE_DIVIDER(upper_r, lower_r);
		kb_voltage = v * RES_VOLTAGE_DIVIDER(R77, R76);
		LOGMASKED(LOG_KEYBOARD, "Key %d - %f - %f\n", pressed_key, v,
				  kb_voltage);
	}
	return kb_voltage;
}

u8 source_state::keyboard_r()
{
	// U32: 74LS367
	// U18: 74LS125

	// D0 <- U32, KEYBD.
	// Output of comparator U31A. Compares the "KYBD APPROX" CV with the voltage
	// generated by the keyboad (see get_keyboard_r()). This bit is used by a
	// successive approximation algorithm to detect the keyboard voltage. The
	// firmware does a binary search by checking the result of the comparison
	// and updating the "KYBD APPROX" CV accordingly.
	// TODO: Compute keyboard voltage in an input callback.
	const u8 d0 = (get_keyboard_v() >= m_cv[int(CV::KEYBOARD_APPROX)]) ? 1 : 0;

	// D1 - Filter contour peak reached (active low).
	// D1 <- U32, FILT CNTR <- S22-11 <- Comparator (U41B, LM393).
	const u8 d1 = contour_peaked(*m_contour[FILTER_CONTOUR]) ? 0 : 1;

	// D2 - Loudness contour peak reached (active low).
	// D2 <- U32, LOUD CNTR <- S22-10 <- Comparator (U41A, LM393).
	const u8 d2 = contour_peaked(*m_contour[LOUDNESS_CONTOUR]) ? 0 : 1;

	// D3: Octave. <- U32, OCT (P34-2 (octave 0 button) and P34-1 (octave +1
	//                button) via U2B and U2C).
	const u8 d3 = m_octave_hi ? 1 : 0;

	// D4 <- J1-4, CASSETTE IN (through "cassette return" circuit and U18D).
	const u8 d4 = 1;  // TODO: Implement.

	// D5 <- U32 D5 <- MOD.
	// The square wave output of the LFO (~ -14V - 14V, connection S33-5) is
	// inverted and level-shifted by Q5, R85, R86. The emitter of Q4 (signal
	// name "MOD") is connected to U32's D5, and to the cathode of the "MOD
	// RATE" LED (connection P34-5).
	const u8 d5 = m_lfo_state ? 0 : 1;

	// D6 <- J2-5, S-TRIG IN, through U18C, pulled up by R23 and protected by
	//       R22.
	const u8 d6 = BIT(m_trigger_io->read(), 0);

	// D7 <- U32, N.C. <- 1 (data bus is pulled high).
	const u8 d7 = 1;

	return (d7 << 7) | (d6 << 6) | (d5 << 5) | (d4 << 4) |
		   (d3 << 3) | (d2 << 2) | (d1 << 1) | d0;
}

u8 source_state::buttons_r(
	const required_ioport_array<6> &button_io, const char *name) const
{
	// Button presses are active low, but the result is inverted by a CD4502.
	// So they look active high to the firmware.
	u8 pressed = 0x00;
	for (int i = 0; i < 6; ++i)
	{
		if (!BIT(m_button_row_latch, i))
			pressed |= u8(~button_io[i]->read() & 0xff);
	}
	// Bits 6 and 7 are not connected to the button input and pulled high.
	pressed |= 0xc0;
	if (pressed & 0x3f)
	{
		LOGMASKED(LOG_BUTTONS, "Button read %s - %02X: %02X\n",
				  name, m_button_row_latch, pressed);
	}
	return pressed;
}

u8 source_state::buttons_a_r()
{
	// U8, CD4502B (connceted to "Membrane switch interface", P2, "Bottom left")
	// (D0, D1, D2, D3, D4, D5) <- (P2-1, P2-3, P2-2, P2-6, P2-5, P2-4)
	return buttons_r(m_button_a_io, "A");
}

u8 source_state::buttons_b_r()
{
	// U9, CD4502B (connected to "Membrane switch interface", P3, "Bottom right")
	// (D0, D1, D2, D3, D4, D5, D6, D7) <- (P3-2, P3-1, P3-3, P3-6, P3-5, P3-4)
	return buttons_r(m_button_b_io, "B");
}

u8 source_state::encoder_r()
{
	// D0 contains whether the encoder was last incremented or decremented.
	LOGMASKED(LOG_ENCODER,
			  "Encoder read: %d - %d\n", m_encoder->read(), m_encoder_incr);
	// Reading the encoder's state also clears /INT (via U21B, U7A and U15A).
	if (!machine().side_effects_disabled())
		m_maincpu->set_input_line(INPUT_LINE_IRQ0, CLEAR_LINE);
	return m_encoder_incr ? 1 : 0;
}

template<int Which> NETDEV_ANALOG_CALLBACK_MEMBER(source_state::contour_cv_changed)
{
	// The control current (Iabc) into each envelope generator ("contour") CA3080
	// is determined by a voltage-to-exponential-current converter (see relevant
	// netlist).

	// This callback is invoked by the netlist simulation when the control current
	// changes. This happens when the firmware sets a new (dis)charge rate, or if
	// the "range" trimmer is adjusted.

	static_assert(Which == FILTER_CONTOUR || Which == LOUDNESS_CONTOUR);
	static constexpr const char *CONTOUR_NAME = (Which == FILTER_CONTOUR) ? "Filter" : "Loudness";
	static constexpr const int RATE_CV_INDEX =
		(Which == FILTER_CONTOUR) ? int(CV::FILTER_CONTOUR_RATE) : int(CV::LOUDNESS_CONTOUR_RATE);

	// The netlist outputs a voltage. Convert it to a current.
	m_contour_cc[Which] = (data - CA3080_VABC) / RES_K(10);  // R198 for filter, R186 for loudness.
	machine().scheduler().synchronize(timer_expired_delegate(FUNC(source_state::update_contour<Which>), this));

	LOGMASKED(LOG_CONTOUR, "%s contour CC: %f uA, rate CV: %f, range trimmer: %d\n",
			  CONTOUR_NAME, m_contour_cc[Which] * 1e6F, m_cv[RATE_CV_INDEX], m_contour_range[Which]->read());
}

// Must be called with machine().scheduler().synchronize(...), to ensure the EG
// updates use the global time.
template<int Which> TIMER_CALLBACK_MEMBER(source_state::update_contour)
{
	// Each of the voltage-controlled envelope generators (called "contours" on
	// the schematic) are based on a CA3080 OTA configured as a current-controlled
	// resistor. This configuration is explained in the first 10 minutes of
	// https://www.youtube.com/watch?v=pTHHzFsa4Ss

	// The OTA (dis)charges a capacitor to the level set by the firmware.
	// Charge rate is controlled by the Iabc current into the OTA, which is
	// also contrlled by the firmware (see contour_cv_changed()).

	static_assert(Which == FILTER_CONTOUR || Which == LOUDNESS_CONTOUR);
	static constexpr const char *CONTOUR_NAME = (Which == FILTER_CONTOUR) ? "Filter" : "Loudness";
	static constexpr const int LEVEL_CV_INDEX =
		(Which == FILTER_CONTOUR) ? int(CV::FILTER_CONTOUR_LEVEL) : int(CV::LOUDNESS_CONTOUR_LEVEL);

	// All componets are on board 2. All resistors have 1% tolerance.
	//                           Filter contour          Loudness contour
	static constexpr const float R196 = RES_K(18.2);  // R183
	static constexpr const float R197 = RES_R(100);   // R184
	static constexpr const float R195 = RES_K(20);    // R187
	static constexpr const float R194 = RES_R(100);   // R182

	// Voltage dividers at the OTA's + and - inputs.
	static constexpr const float OTA_DIVIDER_PLUS = RES_VOLTAGE_DIVIDER(R196, R197);
	static constexpr const float OTA_DIVIDER_MINUS = RES_VOLTAGE_DIVIDER(R195, R194);

	if (m_contour_cc[Which] <= 0)
	{
		// The netlist solver might transiently send negative values.
		LOG("%s EG received a non-positive control current. Skipping update.\n", CONTOUR_NAME);
		return;
	}

	// Ideal OTA transconductance at room temparature.
	const float g = 19.2F * m_contour_cc[Which];
	// Note the sligh difference in the calculations below, compared to the video
	// linked above, due to the resistive dividers at the two OTA inputs not
	// being identical.
	const float effective_r = 1.0F / (g * OTA_DIVIDER_MINUS);
	m_contour[Which]->set_r(effective_r);

	const float level_cv = m_cv[LEVEL_CV_INDEX];  // 0V - 10V.
	const float target_v = level_cv * OTA_DIVIDER_PLUS / OTA_DIVIDER_MINUS;  // 0V - ~10.98V.
	m_contour[Which]->set_target_v(target_v);

	LOGMASKED(LOG_CONTOUR, "%s EG update - Level CV: %f, target_v: %f, R: %f, tau: %f\n",
			  CONTOUR_NAME, level_cv, target_v, effective_r, effective_r * CONTOUR_C);
}

NETDEV_ANALOG_CALLBACK_MEMBER(source_state::lfo_cv_changed)
{
	// The calculation of the LFO control current is very similar to that of the
	// EGs. See contour_cv_changed().
	m_lfo_cc = (data - CA3080_VABC) / RES_K(10);  // R227
	machine().scheduler().synchronize(timer_expired_delegate(FUNC(source_state::update_lfo_timer), this));
	LOGMASKED(LOG_LFO, "LFO CC: %f uA, rate CV: %f, range trimmer: %d\n",
			  m_lfo_cc * 1e6F, m_cv[int(CV::MOD_RATE)], m_lfo_range->read());
}

TIMER_CALLBACK_MEMBER(source_state::update_lfo_timer)
{
	// The LFO ("MOD OSC" in the schematic) is a triangle core oscillator based
	// on a CA3080 OTA (U49). The OTA's Iabc is determined by a voltage-to-exponential-current
	// converter, whose voltage is set by the firmware.
	// The OTA is configured to (dis)charges the capacitor (C58) with a constant
	// current, resulting in a triangle wave (-/+ ~1.5V). The oscillator also
	// produces a square wave (-/+ ~14V) as part of its operation.

	// All components on board 2.
	static constexpr const float R219 = RES_K(100);
	static constexpr const float R220 = RES_K(12);
	static constexpr const float C58 = CAP_U(0.33);

	// Approximate max magnitude of opamp output, according to schematic (supply voltage is 15V).
	static constexpr const float V_PEAK_SQUARE = 14;
	static constexpr const float V_PEAK_TRIANGLE = V_PEAK_SQUARE * RES_VOLTAGE_DIVIDER(R219, R220);  // ~1.5V

	// The differential input at the OTA will be +/- V_PEAK_TRIANGLE. This is well
	// beyond the "linear" range (-/+ ~10-20mV), so the output current will be
	// saturated to (almost) +/- Iabc (m_lfo_cc).
	const float i_out = m_lfo_cc;

	// Time it takes to charge the capacitor from -V_PEAK_TRIANGLE to +V_PEAK_TRIANGLE
	// with a constant current. This is the half-period of the LFO, which is what
	// we need for our timer.
	const float t_half = 2 * V_PEAK_TRIANGLE * C58 / i_out;

	// Continue from the current position in the cycle.
	const double t_remaining = t_half * m_lfo_timer->remaining().as_double() / m_lfo_timer->period().as_double();

	if (i_out > 0)
		m_lfo_timer->adjust(attotime::from_double(t_remaining), 0, attotime::from_double(t_half));
	else
		m_lfo_timer->reset();

	LOGMASKED(LOG_LFO, "LFO frequency updated - Icharge: %f uA, t_remaining: %f, t_half: %f, f: %f\n",
			  i_out * 1e6F, t_remaining, t_half, 1.0F / (2.0F * t_half));
}

TIMER_DEVICE_CALLBACK_MEMBER(source_state::lfo_timer_tick)
{
	m_lfo_state = !m_lfo_state;
	m_lfo_rate_led = !m_lfo_state;  // LED (LED 3, board 5) is active low.
	LOGMASKED(LOG_LFO_TIMER, "LFO Timer ticked: %d\n", m_lfo_state);
}

void source_state::memory_map(address_map &map)
{
	// Address decoding done through U26, 74LS138, E1=E2=0, E3=1,
	// A0-A2 = Z80 A13-A15.
	// Z80 A12 is not connected.
	// The signal names below (e.g. "ROM /EN", "RAM /EN") match those in the
	// schematics.

	// ROM /EN: 0x0000-0x1fff.
	// 1 x 2532 (4K, 8bit) ROM, U23.
	map(0x0000, 0x0fff).mirror(0x1000).rom();
	// 2 x 74LS378. Z80 and latch data lines are not connected in order.
	map(0x0000, 0x0000).mirror(0x1fff).w(FUNC(source_state::edit_latch_w));

	// RAM /EN: 0x2000-0x3fff.
	// 2 x 6514 (1K, 4bit) NVRAMs. U27: D0-D3, U28: D4-D7.
	// Z80 A0-A1 -> RAM A0-A1. Z80 A2-A8 -> RAM A3-A9. Z80 A9 -> RAM A2.
	map(0x2000, 0x23ff).mirror(0x1c00).ram().share(NVRAM_TAG);

	// OUTPUT /EN: 0x4000-0x5fff.
	// 2 output latches (74LS378, U11 and U12) enabled by 74LS155 (U13B),
	// with Z80 A3-A4 as inputs to A0-A1. O2 and O3 are not connected, so
	// A4=1 does not enable anything.
	map(0x4000, 0x4000).mirror(0x1fe7).w(FUNC(source_state::output_latch_a_w));
	map(0x4008, 0x4008).mirror(0x1fe7).w(FUNC(source_state::output_latch_b_w));

	// KYBD /EN: 0x6000-0x7fff.
	// 74LS367, U32
	map(0x6000, 0x6000).mirror(0x1fff).r(FUNC(source_state::keyboard_r));
	// 74LS74, U22A. D <- Z80 D4.
	map(0x6000, 0x6000).mirror(0x1fff).w(FUNC(source_state::cassette_w));

	// FRONT PANEL /EN1: 0x8000-0x9fff.
	// CD4502, U8.
	map(0x8000, 0x8000).mirror(0x1fff).r(FUNC(source_state::buttons_a_r));
	// 74LS378, U5.
	map(0x8000, 0x8000).mirror(0x1fff).w(FUNC(source_state::buttons_latch_w));

	// FRONT PANEL /EN2: 0xa000-0xbfff.
	// CD4502, U9
	map(0xa000, 0xa000).mirror(0x1fff).r(FUNC(source_state::buttons_b_r));

	// DISPLAY /EN: 0xc000-0xdfff.
	// 74LS378, U1.
	map(0xc000, 0xc000).mirror(0x1fff).w(FUNC(source_state::program_latch_w));

	// CNTRL /EN: 0xe000-0xffff. (typo in schematic: 0xefff-0xffff).
	map(0xe000, 0xe000).mirror(0x1fff).r(FUNC(source_state::encoder_r));
}

void source_state::io_map(address_map &map)
{
	map.global_mask(0xff);
	map(0x00, 0x1f).mirror(0xe0).w(FUNC(source_state::cv_w));
}

void source_state::machine_start()
{
	m_octave_led.resolve();
	m_lfo_rate_led.resolve();
	m_program_display.resolve();
	m_edit_display.resolve();
	m_edit_led.resolve();
	m_kb_track.resolve();
	m_osc_waveform.resolve();
	m_sync.resolve();
	m_lfo_to_filter.resolve();
	m_lfo_to_osc.resolve();
	m_lfo_shape.resolve();
	m_trigger_out.resolve();

	save_item(NAME(m_octave_hi));
	save_item(NAME(m_button_row_latch));
	save_item(NAME(m_encoder_incr));
	save_item(NAME(m_lfo_state));
	save_item(NAME(m_lfo_cc));
	save_item(NAME(m_contour_cc));
	save_item(NAME(m_cv));
}

void source_state::machine_reset()
{
	update_octave_leds();
	update_contour<FILTER_CONTOUR>(0);
	update_contour<LOUDNESS_CONTOUR>(0);
	update_lfo_timer(0);

	// If an input port has its default value at startup, its write callback will
	// not be invoked. Ensure the netlist inputs are initialized even in that
	// scenario.
	subdevice<netlist_mame_analog_input_device>("source_nl:cntr_range_0")->write(m_contour_range[0]->read());
	subdevice<netlist_mame_analog_input_device>("source_nl:cntr_range_1")->write(m_contour_range[1]->read());
	subdevice<netlist_mame_analog_input_device>("source_nl:lfo_range")->write(m_lfo_range->read());
}

void source_state::source(machine_config &config)
{
	// /M1, /RFSH not Connected.
	// /HALT, /NMI pulled up to 5V, with no other connection.
	Z80(config, m_maincpu, 4_MHz_XTAL / 2);  // Divided by 2 through U22B.
	m_maincpu->set_addrmap(AS_PROGRAM, &source_state::memory_map);
	m_maincpu->set_addrmap(AS_IO, &source_state::io_map);

	NVRAM(config, NVRAM_TAG, nvram_device::DEFAULT_ALL_0);  // 2x6514: U27, U28.

	VA_RC_EG(config, m_contour[FILTER_CONTOUR]).set_c(CONTOUR_C);  // C57 (Board 2).
	VA_RC_EG(config, m_contour[LOUDNESS_CONTOUR]).set_c(CONTOUR_C);  // C56 (Board 2).
	TIMER(config, m_lfo_timer).configure_generic(FUNC(source_state::lfo_timer_tick));

	config.set_default_layout(layout_moog_source);


	NETLIST_CPU(config, "source_nl", netlist::config::DEFAULT_CLOCK()).set_source(NETLIST_NAME(moogsource));

	NETLIST_ANALOG_INPUT(config, "source_nl:cntr_range_0", "R201.DIAL");
	NETLIST_ANALOG_INPUT(config, m_contour_rate[FILTER_CONTOUR], "FLT_CNTR_RATE.IN");
	NETLIST_ANALOG_OUTPUT(config, "source_nl:cntr_cv_0")
		.set_params("FLT_CNTR_CV", FUNC(source_state::contour_cv_changed<FILTER_CONTOUR>));

	NETLIST_ANALOG_INPUT(config, "source_nl:cntr_range_1", "R179.DIAL");
	NETLIST_ANALOG_INPUT(config,  m_contour_rate[LOUDNESS_CONTOUR], "LOUD_CNTR_RATE.IN");
	NETLIST_ANALOG_OUTPUT(config, "source_nl:cntr_cv_1")
		.set_params("LOUD_CNTR_CV", FUNC(source_state::contour_cv_changed<LOUDNESS_CONTOUR>));

	NETLIST_ANALOG_INPUT(config, "source_nl:lfo_range", "R223.DIAL");
	NETLIST_ANALOG_INPUT(config, m_lfo_rate, "MOD_RATE.IN");
	NETLIST_ANALOG_OUTPUT(config, "source_nl:lfo_cv")
		.set_params("MOD_CV", FUNC(source_state::lfo_cv_changed));
}

DECLARE_INPUT_CHANGED_MEMBER(source_state::octave_button_pressed)
{
	// Inverters U2B and U2C (Board 3) are configured as an SR flip-flop, with
	// SW1 and SW2 (Board 5) as Reset and Set respectively.

	// Inputs are active low.
	const u8 input = m_octave_io->read();
	const bool octave_0 = (input & 0x01) == 0;  // "0", SW1 (Board 5).
	const bool octave_p1 = (input & 0x02) == 0;  // "+1", SW2 (Board 5).
	if (!octave_0 && octave_p1)
	{
		m_octave_hi = true;
	}
	else if (octave_0 && !octave_p1)
	{
		m_octave_hi = false;
	}
	else if (octave_0 && octave_p1)
	{
		// The selected octave is undefined in this case, so it is not updated.
		// An octave will be selected when one of the two buttons is released.
	}
	else
	{
		// No buttons pressed. No change in selected octave.
	}
	update_octave_leds();
}

DECLARE_INPUT_CHANGED_MEMBER(source_state::encoder_moved)
{
	static constexpr const int WRAP_BUFFER = 10;
	const bool overflowed = newval <= WRAP_BUFFER &&
							oldval >= 240 - WRAP_BUFFER;
	const bool underflowed = newval >= 240 - WRAP_BUFFER &&
							 oldval <= WRAP_BUFFER;
	m_encoder_incr = ((newval > oldval) || overflowed) && !underflowed;
	m_maincpu->set_input_line(INPUT_LINE_IRQ0, ASSERT_LINE);
	LOGMASKED(LOG_ENCODER, "Encoder changed: %d %d\n", newval, m_encoder_incr);
}

INPUT_PORTS_START(source)
	PORT_START("button_group_a_0")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Memory STORE") PORT_CODE(KEYCODE_S)  // r2p6
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 13")  // r2p4
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 12")  // r2p5
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 16")  // r2p1
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 15")  // r2p2
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 14")  // r2p3

	PORT_START("button_group_b_0")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Level 2") // r1p5
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Level 1")  //r1p6
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter Contour Decay")  //r1p4
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter Contour Amount")  //r1p1
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter Contour Release")  //r1p2
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter Contour Sustain")  // r1p3

	PORT_START("button_group_a_1")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) //NC
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 3") PORT_CODE(KEYCODE_3)
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 2") PORT_CODE(KEYCODE_2)
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 6") PORT_CODE(KEYCODE_6)
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 5") PORT_CODE(KEYCODE_5)
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 4") PORT_CODE(KEYCODE_4)

	PORT_START("button_group_b_1")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter Attack")
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Mixer: OSC 2")
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 2 Shape: Pulse")
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter KB Track: OFF")
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter Emphasis")
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter Cutoff")

	PORT_START("button_group_a_2")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) // NC
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 10") PORT_CODE(KEYCODE_0)
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 11")
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 7") PORT_CODE(KEYCODE_7)
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 8") PORT_CODE(KEYCODE_8)
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 9") PORT_CODE(KEYCODE_9)

	PORT_START("button_group_b_2")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Loudness Decay")
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Loudness Attack")
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("MOD Rate")
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) // NC / PROGRAM 1
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Loudness Release")
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Loudness Sustain")

	PORT_START("button_group_a_3")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) // NC
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("MOD To Filter: OFF")
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Program 1") PORT_CODE(KEYCODE_1)
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 2 Footage: 32'")
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) // NC / PROGRAM 1
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("MOD To Filter: ON")

	PORT_START("button_group_b_3")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Mixer: NOISE")
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 2 Interval")
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 2 Shape: Sawtooth")
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 1 Shape: Pulse")
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter KB Track: 1/2")
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Filter KB Track: FULL")

	PORT_START("button_group_a_4")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) // NC
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("MOD to Osc: OFF")
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Memory HOLD") PORT_CODE(KEYCODE_H)
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("KB Glide")
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("MOD Shape: Triangle")
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("MOD to Osc: ON")

	PORT_START("button_group_b_4")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 2 Shape: Triange")
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 2 Footage: 16'")
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("SYNC: ON")
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 1 Footage: 32'")
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 1 Footage: 16'")
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Mixer: OSC 1")

	PORT_START("button_group_a_5")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) // NC
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Trigger MULTI")
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Trigger SINGLE")
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) // NC / PROGRAM 1
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("MOD Shape: Square")
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) // NC / PROGRAM 1

	PORT_START("button_group_b_5")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 2 Footage: 8'")
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) // NC / PROGRAM 1
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("SYNC: OFF")
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 1 Footage: 8'")
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 1 Shape: Triange")
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("OSC 1 Shape: Sawtooth")

	PORT_START("octave_buttons")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Octave 0")  // SW1 (Board 5).
		PORT_CHANGED_MEMBER(DEVICE_SELF, FUNC(source_state::octave_button_pressed), 0x01)
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("Octave +1")  // SW2 (Board 5).
		PORT_CHANGED_MEMBER(DEVICE_SELF, FUNC(source_state::octave_button_pressed), 0x02)

	PORT_START("incremental_controller")
	PORT_BIT(0xff, 0x00, IPT_POSITIONAL) PORT_POSITIONS(240) PORT_WRAPS
		PORT_SENSITIVITY(25) PORT_KEYDELTA(3)
		PORT_CODE_DEC(KEYCODE_LEFT) PORT_CODE_INC(KEYCODE_RIGHT) PORT_FULL_TURN_COUNT(240)
		PORT_CHANGED_MEMBER(DEVICE_SELF, FUNC(source_state::encoder_moved), 1)

	PORT_START("keyboard_oct_1")
	PORT_BIT(0x001, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_C2
	PORT_BIT(0x002, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_CS2
	PORT_BIT(0x004, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_D2
	PORT_BIT(0x008, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_DS2
	PORT_BIT(0x010, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_E2
	PORT_BIT(0x020, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_F2
	PORT_BIT(0x040, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_FS2
	PORT_BIT(0x080, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_G2
	PORT_BIT(0x100, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_GS2
	PORT_BIT(0x200, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_A2
	PORT_BIT(0x400, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_AS2
	PORT_BIT(0x800, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_B2

	PORT_START("keyboard_oct_2")
	PORT_BIT(0x001, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_C3
	PORT_BIT(0x002, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_CS3
	PORT_BIT(0x004, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_D3
	PORT_BIT(0x008, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_DS3
	PORT_BIT(0x010, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_E3
	PORT_BIT(0x020, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_F3
	PORT_BIT(0x040, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_FS3
	PORT_BIT(0x080, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_G3
	PORT_BIT(0x100, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_GS3
	PORT_BIT(0x200, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_A3
	PORT_BIT(0x400, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_AS3
	PORT_BIT(0x800, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_B3

	PORT_START("keyboard_oct_3")
	PORT_BIT(0x001, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_C4
	PORT_BIT(0x002, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_CS4
	PORT_BIT(0x004, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_D4
	PORT_BIT(0x008, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_DS4
	PORT_BIT(0x010, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_E4
	PORT_BIT(0x020, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_F4
	PORT_BIT(0x040, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_FS4
	PORT_BIT(0x080, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_G4
	PORT_BIT(0x100, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_GS4
	PORT_BIT(0x200, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_A4
	PORT_BIT(0x400, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_AS4
	PORT_BIT(0x800, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_B4

	PORT_START("keyboard_oct_4")
	PORT_BIT(0x001, IP_ACTIVE_HIGH, IPT_OTHER) PORT_GM_C5

	PORT_START("trigger_in")  // External trigger input (see keyboard_r()).
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_OTHER) PORT_NAME("S TRIG IN") PORT_CODE(KEYCODE_T)

	PORT_START("contour_range_0")  // R201 (Board 2), 100K trimpot.
	PORT_ADJUSTER(50, "FILTER_CONTOUR_RANGE") NETLIST_ANALOG_PORT_CHANGED("source_nl", "cntr_range_0")

	PORT_START("contour_range_1")  // R179 (Board 2), 100K trimpot.
	PORT_ADJUSTER(50, "LOUDNESS_CONTOUR_RANGE") NETLIST_ANALOG_PORT_CHANGED("source_nl", "cntr_range_1")

	PORT_START("lfo_range")  // R223 (Board 2), 100K trimpot.
	// A default of 0 takes us the closest to the advertised highest LFO frequency of 30 Hz.
	PORT_ADJUSTER(0, "LFO RANGE") NETLIST_ANALOG_PORT_CHANGED("source_nl", "lfo_range")
INPUT_PORTS_END

// It seems like the Source was launched with firmware Revision 2.2.
// There was also a Revision 3.2, and the last official firmware release was
// Revision 3.3.
ROM_START(moogsource)
	ROM_REGION(0x1000, MAINCPU_TAG, 0)
	ROM_DEFAULT_BIOS("r3.3")

	ROM_SYSTEM_BIOS(0, "r3.3", "Rev 3.3")
	ROMX_LOAD("3p3.u23", 0x000000, 0x001000, CRC(4211331f) SHA1(8767ef6b1cbb032a89a78bdb77bb7dbc1c187974), ROM_BIOS(0))
ROM_END

}  // anonymous namespace.

SYST(1981, moogsource, 0, 0, source, source, source_state, empty_init, "Moog Music", "Moog Source", MACHINE_NOT_WORKING | MACHINE_SUPPORTS_SAVE | MACHINE_NO_SOUND)
