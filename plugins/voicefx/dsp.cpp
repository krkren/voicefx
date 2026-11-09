// dsp.cpp - see dsp.h
//
// SPDX-License-Identifier: 0BSD
#include "dsp.h"

#include <algorithm>
#include <cmath>

namespace voicefx {

namespace {

const double PI = 3.14159265358979323846;

double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// The reverb's comb and all-pass lengths at 44.1 kHz (Schroeder and Moorer's mutually prime
// sort of numbers), scaled to the voice's rate; the right channel's a little longer
const int kCombs[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
const int kAllPasses[4] = { 556, 441, 341, 225 };
const int kSpread = 23;

} // namespace

const std::vector<std::string> &settingNames() {
	static const std::vector<std::string> names = { "lowcut", "highcut", "order", "drive", "mono", "delay",
	                                                "feedback", "echo", "damping", "room", "reverb", "dry",
	                                                "gain", "tail" };
	return names;
}

bool Settings::preset(const std::string &name) {
	Settings s;
	if (name == "none" || name.empty()) {
		// as made
	} else if (name == "echo") {
		// a memory, an inner voice: a few repeats well under the voice, darker each time, in a
		// small room
		s.delay = 240;
		s.feedback = 25;
		s.echo = 26;
		s.damping = 55;
		s.room = 35;
		s.reverb = 18;
	} else if (name == "phone") {
		// a call: the voice's band without its lows and highs, a touch of crunch
		s.lowcut = 350;
		s.highcut = 3200;
		s.order = 2;
		s.drive = 8;
		s.mono = true;
		s.gain = 125;
	} else if (name == "muffled") {
		// through a door or a wall: the highs softened, a little quieter
		s.highcut = 1100;
		s.order = 2;
		s.room = 15;
		s.reverb = 10;
		s.gain = 90;
	} else if (name == "hall") {
		// a large room, behind the voice
		s.room = 70;
		s.reverb = 30;
		s.damping = 40;
	} else if (name == "cave") {
		s.room = 85;
		s.reverb = 42;
		s.damping = 20;
		s.delay = 160;
		s.feedback = 22;
		s.echo = 22;
	} else if (name == "radio") {
		// a transceiver: a narrower band and more crunch than a phone
		s.lowcut = 450;
		s.highcut = 2800;
		s.order = 4;
		s.drive = 18;
		s.mono = true;
		s.gain = 145;
	} else {
		return false;
	}
	*this = s;
	return true;
}

bool Settings::set(const std::string &name, double v) {
	if (name == "lowcut") lowcut = clampd(v, 0, 20000);
	else if (name == "highcut") highcut = clampd(v, 0, 20000);
	else if (name == "order") order = v >= 3 ? 4 : 2;
	else if (name == "drive") drive = clampd(v, 0, 100);
	else if (name == "mono") mono = v != 0;
	else if (name == "delay") delay = clampd(v, 0, 2000);
	else if (name == "feedback") feedback = clampd(v, 0, 95);
	else if (name == "echo") echo = clampd(v, 0, 100);
	else if (name == "damping") damping = clampd(v, 0, 100);
	else if (name == "room") room = clampd(v, 0, 100);
	else if (name == "reverb") reverb = clampd(v, 0, 100);
	else if (name == "dry") dry = clampd(v, 0, 100);
	else if (name == "gain") gain = clampd(v, 0, 400);
	else if (name == "tail") tail = clampd(v, 0, 30000);
	else return false;
	return true;
}

bool Settings::get(const std::string &name, double &v) const {
	if (name == "lowcut") v = lowcut;
	else if (name == "highcut") v = highcut;
	else if (name == "order") v = order;
	else if (name == "drive") v = drive;
	else if (name == "mono") v = mono ? 1 : 0;
	else if (name == "delay") v = delay;
	else if (name == "feedback") v = feedback;
	else if (name == "echo") v = echo;
	else if (name == "damping") v = damping;
	else if (name == "room") v = room;
	else if (name == "reverb") v = reverb;
	else if (name == "dry") v = dry;
	else if (name == "gain") v = gain;
	else if (name == "tail") v = tail;
	else return false;
	return true;
}

// RBJ's cookbook
void Biquad::lowpass(double rate, double freq, double q) {
	double w = 2 * PI * clampd(freq, 10, rate * 0.49) / rate, cw = std::cos(w), sw = std::sin(w);
	double alpha = sw / (2 * q), a0 = 1 + alpha;
	b0 = (1 - cw) / 2 / a0;
	b1 = (1 - cw) / a0;
	b2 = (1 - cw) / 2 / a0;
	a1 = -2 * cw / a0;
	a2 = (1 - alpha) / a0;
}

void Biquad::highpass(double rate, double freq, double q) {
	double w = 2 * PI * clampd(freq, 10, rate * 0.49) / rate, cw = std::cos(w), sw = std::sin(w);
	double alpha = sw / (2 * q), a0 = 1 + alpha;
	b0 = (1 + cw) / 2 / a0;
	b1 = -(1 + cw) / a0;
	b2 = (1 + cw) / 2 / a0;
	a1 = -2 * cw / a0;
	a2 = (1 - alpha) / a0;
}

void Effect::start(double rate, int channels) {
	rate_ = rate > 0 ? rate : 48000;
	channels_ = channels > 0 ? channels : 1;
	ch_.assign(size_t(channels_), Channel());
	ready_ = true;
	build();
	filters();
}

void Effect::reset() {
	for (Channel &c : ch_) {
		for (Biquad &b : c.hp) b.clear();
		for (Biquad &b : c.lp) b.clear();
		std::fill(c.line.begin(), c.line.end(), 0.0f);
		c.lineDamp = 0;
		for (Comb &m : c.combs) {
			std::fill(m.buf.begin(), m.buf.end(), 0.0f);
			m.store = 0;
		}
		for (AllPass &a : c.allpasses) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
	}
	ringing_ = false;
}

void Effect::configure(const Settings &s) {
	bool rebuild = s.delay != settings_.delay;
	settings_ = s;
	if (!ready_) return;
	if (rebuild) build();
	filters();
}

// The delay lines, sized for the rate and the echo's delay
void Effect::build() {
	double scale = rate_ / 44100.0;
	for (size_t k = 0; k < ch_.size(); k++) {
		Channel &c = ch_[k];
		size_t len = size_t(std::max(1.0, settings_.delay * rate_ / 1000.0));
		c.line.assign(len, 0.0f);
		c.linePos = 0;
		int spread = k % 2 ? kSpread : 0;
		for (int m = 0; m < 8; m++)
			if (c.combs[m].buf.empty()) c.combs[m].buf.assign(size_t((kCombs[m] + spread) * scale) + 1, 0.0f);
		for (int a = 0; a < 4; a++)
			if (c.allpasses[a].buf.empty()) c.allpasses[a].buf.assign(size_t((kAllPasses[a] + spread) * scale) + 1, 0.0f);
	}
}

// The filters' coefficients: a Butterworth of order 2 is one biquad at Q 1/sqrt 2, of order 4
// two at 0.541 and 1.307
void Effect::filters() {
	const double q2 = 0.70710678118654752;
	const double q4[2] = { 0.54119610014619698, 1.3065629648763766 };
	for (Channel &c : ch_) {
		for (int k = 0; k < 2; k++) {
			double q = settings_.order >= 4 ? q4[k] : q2;
			if (settings_.lowcut > 0) c.hp[k].highpass(rate_, settings_.lowcut, q);
			if (settings_.highcut > 0) c.lp[k].lowpass(rate_, settings_.highcut, q);
		}
	}
}

void Effect::process(float *samples, int frames) {
	if (!ready_ || frames <= 0) return;
	const Settings &s = settings_;
	const int n = channels_;
	const bool hp = s.lowcut > 0, lp = s.highcut > 0, four = s.order >= 4;
	// soft clipping that leaves quiet parts as they are and flattens loud ones
	const double driveK = 1 + s.drive / 8.0, driveNorm = 1.0 / driveK;
	const bool echo = s.delay > 0 && s.echo > 0;
	const double fb = s.feedback / 100.0, echoLevel = s.echo / 100.0, damp = s.damping / 100.0 * 0.85;
	const bool verb = s.room > 0 && s.reverb > 0;
	// reverb 100 % is about as loud as the voice whatever the room's size: the combs ring longer
	// and louder as their feedback rises, so their sum is scaled down by sqrt(1 - feedback^2)
	const double roomFb = 0.70 + s.room / 100.0 * 0.28, verbDamp = s.damping / 100.0 * 0.5,
	             verbLevel = s.reverb / 100.0 * 0.07 * std::sqrt(1 - roomFb * roomFb);
	const double dry = s.dry / 100.0, gain = s.gain / 100.0;
	double loud = 0;
	for (int f = 0; f < frames; f++) {
		float *frame = samples + size_t(f) * size_t(n);
		double monoSum = 0;
		for (int k = 0; k < n; k++) {
			Channel &c = ch_[size_t(k)];
			double x = frame[k];
			// the crackle first, then the band: a line limits what the clipping adds
			if (s.drive > 0) x = std::tanh(x * driveK) * driveNorm;
			if (hp) {
				x = c.hp[0].run(x);
				if (four) x = c.hp[1].run(x);
			}
			if (lp) {
				x = c.lp[0].run(x);
				if (four) x = c.lp[1].run(x);
			}
			frame[k] = float(x);
			monoSum += x;
		}
		if (s.mono && n > 1)
			for (int k = 0; k < n; k++) frame[k] = float(monoSum / n);
		for (int k = 0; k < n; k++) {
			Channel &c = ch_[size_t(k)];
			double x = frame[k];
			double out = x * dry;
			if (echo) {
				double d = c.line[c.linePos];
				c.lineDamp = d * (1 - damp) + c.lineDamp * damp;
				c.line[c.linePos] = float(x + c.lineDamp * fb);
				if (++c.linePos >= c.line.size()) c.linePos = 0;
				out += d * echoLevel;
				x = x + d * echoLevel; // the echo is in the hall too
			}
			if (verb) {
				double acc = 0;
				for (Comb &m : c.combs) acc += m.run(x, roomFb, verbDamp);
				for (AllPass &a : c.allpasses) acc = a.run(acc);
				out += acc * verbLevel;
			}
			out *= gain;
			frame[k] = float(out);
			double a = std::fabs(out);
			if (a > loud) loud = a;
		}
	}
	// what rings on: anything in the echo's line or the reverb's combs
	if (echo || verb) {
		double left = 0;
		for (Channel &c : ch_) {
			if (echo)
				for (float v : c.line) left = std::max(left, double(std::fabs(v)));
			if (verb)
				for (Comb &m : c.combs)
					for (size_t k = 0; k < m.buf.size(); k += 7) left = std::max(left, double(std::fabs(m.buf[k])));
		}
		ringing_ = left > 1e-4 || loud > 1e-4;
	} else {
		ringing_ = false;
	}
}

} // namespace voicefx
