// dsp.h - the effect itself, with no engine in it: what the plugin runs on a voice as it plays,
// and what the tests check
//
// One effect is a chain of stages, each off until its setting is given, so a preset is just a
// set of settings:
//
//   lowcut, highcut   Butterworth high- and low-pass filters (Hz; 0 off), of order 2 or 4
//   drive             soft clipping, 0-100 (a phone's or a radio's crunch), before the filters:
//                     quiet parts as they are, loud ones flattened
//   mono              both channels the same
//   delay, feedback,  an echo: delay ms, feedback 0-95 %, the echo's level 0-100 %, its
//   echo, damping     repeats darker by damping 0-100 %
//   room, reverb      a reverb (a hall, a cave): room size 0-100, its level 0-100 % (100 about
//                     as loud as the voice in any room; damping too)
//   dry               the processed voice's own level, 0-100 % (100)
//   gain              the whole effect's level, % (100)
//
// When the voice ends, the echo and the reverb play on until they have died away (at most
// `tail` ms).
//
// SPDX-License-Identifier: 0BSD
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voicefx {

struct Settings {
	double lowcut = 0, highcut = 0;
	int order = 2;
	double drive = 0;
	bool mono = false;
	double delay = 0, feedback = 0, echo = 0, damping = 30;
	double room = 0, reverb = 0;
	double dry = 100, gain = 100;
	double tail = 8000;

	/// A preset by name: "echo", "phone", "muffled", "hall", "radio", "cave", "none"; false (and
	/// the settings unchanged) for another name
	bool preset(const std::string &name);

	/// Sets one setting by name (as the plugin's properties are named); false for another name
	bool set(const std::string &name, double value);
	/// A setting by name; false for another name
	bool get(const std::string &name, double &value) const;

	/// Whether the effect has anything that rings on after the voice (an echo or a reverb)
	bool hasTail() const { return (delay > 0 && echo > 0) || (room > 0 && reverb > 0); }
};

/// The setting names, as the plugin's properties are named
const std::vector<std::string> &settingNames();

// A biquad (direct form I)
struct Biquad {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	void lowpass(double rate, double freq, double q);
	void highpass(double rate, double freq, double q);
	void clear() { x1 = x2 = y1 = y2 = 0; }
	double run(double x) {
		double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		return y;
	}
};

// A comb filter with a damped feedback, and an all-pass, as a Schroeder reverb is built
struct Comb {
	std::vector<float> buf;
	size_t pos = 0;
	double store = 0;
	double run(double x, double feedback, double damp) {
		double out = buf[pos];
		store = out * (1 - damp) + store * damp;
		buf[pos] = float(x + store * feedback);
		if (++pos >= buf.size()) pos = 0;
		return out;
	}
};
struct AllPass {
	std::vector<float> buf;
	size_t pos = 0;
	double run(double x) {
		double b = buf[pos];
		double out = b - x;
		buf[pos] = float(x + b * 0.5);
		if (++pos >= buf.size()) pos = 0;
		return out;
	}
};

class Effect {
public:
	/// Prepares for a voice: its rate and channels. Clears what's ringing
	void start(double rate, int channels);
	/// Clears what's ringing (a seek, a stop)
	void reset();
	/// New settings: from the next block on (the filters keep their state)
	void configure(const Settings &s);
	const Settings &settings() const { return settings_; }

	/// Runs a block in place: `frames` frames of interleaved float samples
	void process(float *samples, int frames);

	/// Whether anything rings on (the echo's line or the reverb isn't silent yet)
	bool ringing() const { return ringing_; }

private:
	Settings settings_;
	double rate_ = 48000;
	int channels_ = 2;
	bool ready_ = false;
	bool ringing_ = false;

	struct Channel {
		Biquad hp[2], lp[2];
		std::vector<float> line; // the echo
		size_t linePos = 0;
		double lineDamp = 0;
		Comb combs[8];
		AllPass allpasses[4];
	};
	std::vector<Channel> ch_;
	void build();
	void filters();
};

} // namespace voicefx
