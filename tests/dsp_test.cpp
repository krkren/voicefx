// The effect with no engine: none changes nothing, the phone and the door cut what they should,
// the echo repeats where it should, the reverb stays bounded, and the tails die away
//
// SPDX-License-Identifier: 0BSD
#include "dsp.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace voicefx;

static int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static const double RATE = 48000;
static const double PI = 3.14159265358979323846;

// A stereo sine through the effect, in blocks; the RMS of its last half second, left channel
static double sineLevel(const char *preset, double freq, bool noDrive = false) {
	Settings s;
	s.preset(preset);
	if (noDrive) s.drive = 0;
	Effect e;
	e.configure(s);
	e.start(RATE, 2);
	const int block = 1024, total = int(RATE * 1.5);
	std::vector<float> buf(size_t(block) * 2);
	double sum = 0;
	int n = 0;
	for (int f0 = 0; f0 < total; f0 += block) {
		for (int f = 0; f < block; f++) {
			float v = float(0.5 * std::sin(2 * PI * freq * (f0 + f) / RATE));
			buf[size_t(f) * 2] = v;
			buf[size_t(f) * 2 + 1] = v;
		}
		e.process(buf.data(), block);
		if (f0 >= total - int(RATE / 2))
			for (int f = 0; f < block; f++) {
				sum += double(buf[size_t(f) * 2]) * buf[size_t(f) * 2];
				n++;
			}
	}
	return std::sqrt(sum / n);
}

static double db(double level) { return 20 * std::log10(level / (0.5 / std::sqrt(2.0))); }

int main() {
	// none: nothing changes
	{
		Settings s;
		CHECK(s.preset("none"));
		Effect e;
		e.configure(s);
		e.start(RATE, 2);
		std::vector<float> in(2000), out;
		for (size_t k = 0; k < in.size(); k++) in[k] = float(std::sin(k * 0.01));
		out = in;
		e.process(out.data(), 1000);
		CHECK(out == in);
		CHECK(!e.ringing());
	}
	CHECK(!Settings().preset("nonsense"));

	// the phone: about as loud in its band; its filters (without the crackle, whose harmonics of a
	// low hum fall in the band, as on a real line) cut the lows and highs hard
	double phoneMid = db(sineLevel("phone", 1000));
	double bandMid = db(sineLevel("phone", 1000, true)), bandLow = db(sineLevel("phone", 80, true)),
	       bandHigh = db(sineLevel("phone", 9000, true));
	std::printf("phone: 1 kHz %.1f dB; its band: 1 kHz %.1f dB, 80 Hz %.1f dB, 9 kHz %.1f dB\n", phoneMid, bandMid,
	            bandLow, bandHigh);
	CHECK(phoneMid > -3 && phoneMid < 3);
	CHECK(bandLow < bandMid - 30);
	CHECK(bandHigh < bandMid - 30);

	// through a door: the voice's body through, its highs gone
	double doorLow = db(sineLevel("muffled", 200)), doorHigh = db(sineLevel("muffled", 4000));
	std::printf("muffled: 200 Hz %.1f dB, 4 kHz %.1f dB\n", doorLow, doorHigh);
	CHECK(doorLow > -6);
	CHECK(doorHigh < doorLow - 40);

	// the echo: an impulse comes back after the delay, at the echo's level, then quieter
	{
		Settings s;
		s.delay = 100;
		s.echo = 50;
		s.feedback = 50;
		s.damping = 0;
		Effect e;
		e.configure(s);
		e.start(RATE, 1);
		int n = int(RATE * 0.5);
		std::vector<float> buf(size_t(n), 0.0f);
		buf[0] = 1.0f;
		e.process(buf.data(), n);
		int d = int(RATE * 0.1);
		CHECK(std::fabs(buf[0] - 1.0f) < 1e-6);
		CHECK(std::fabs(buf[size_t(d)] - 0.5f) < 1e-3);           // the first repeat
		CHECK(std::fabs(buf[size_t(2 * d)] - 0.25f) < 1e-3);      // the second, fed back
		CHECK(std::fabs(buf[size_t(d / 2)]) < 1e-6);              // nothing between
		CHECK(e.ringing());
		// silence until it has died away: it must, and it rings no longer than the tail
		std::vector<float> zeros(4800, 0.0f);
		int blocks = 0;
		while (e.ringing() && blocks < 1000) {
			std::fill(zeros.begin(), zeros.end(), 0.0f);
			e.process(zeros.data(), 4800);
			blocks++;
		}
		std::printf("echo: rang on for %d ms after the input\n", blocks * 100 + 500);
		CHECK(!e.ringing());
		CHECK(blocks < 100);
		// a reset clears what rings
		buf.assign(size_t(n), 0.0f);
		buf[0] = 1.0f;
		e.process(buf.data(), n);
		e.reset();
		std::fill(zeros.begin(), zeros.end(), 0.0f);
		e.process(zeros.data(), 4800);
		double peak = 0;
		for (float v : zeros) peak = std::max(peak, double(std::fabs(v)));
		CHECK(peak < 1e-6);
	}

	// the reverb: bounded, and it dies away
	{
		Settings s;
		CHECK(s.preset("cave"));
		Effect e;
		e.configure(s);
		e.start(RATE, 2);
		int n = int(RATE);
		std::vector<float> buf(size_t(n) * 2, 0.0f);
		for (int f = 0; f < 4800; f++) buf[size_t(f) * 2] = buf[size_t(f) * 2 + 1] = float(0.5 * std::sin(f * 0.07));
		e.process(buf.data(), n);
		double peak = 0;
		for (float v : buf) peak = std::max(peak, double(std::fabs(v)));
		std::printf("cave: peak %.2f for a 0.5 input\n", peak);
		CHECK(peak < 1.0);
		// the tail after the input is there, and goes
		double after = 0;
		for (int f = 9600; f < 14400; f++) after = std::max(after, double(std::fabs(buf[size_t(f) * 2])));
		CHECK(after > 0.01);
		int blocks = 0;
		std::vector<float> zeros(9600, 0.0f);
		while (e.ringing() && blocks < 1000) {
			std::fill(zeros.begin(), zeros.end(), 0.0f);
			e.process(zeros.data(), 4800);
			blocks++;
		}
		std::printf("cave: rang on for %d ms after its second\n", blocks * 100);
		CHECK(!e.ringing());
	}

	// the settings by name
	{
		Settings s;
		CHECK(s.set("highcut", 3000));
		double v = 0;
		CHECK(s.get("highcut", v) && v == 3000);
		CHECK(s.set("feedback", 200) && s.get("feedback", v) && v == 95); // clamped
		CHECK(!s.set("nonsense", 1));
		for (const std::string &n : settingNames()) CHECK(s.get(n, v));
		Settings p;
		p.preset("phone");
		CHECK(p.mono && p.lowcut > 0 && p.highcut > 0 && !p.hasTail());
		p.preset("echo");
		CHECK(p.hasTail());
	}

	if (failures) {
		std::printf("%d failed\n", failures);
		return 1;
	}
	std::printf("dsp_test: all passed\n");
	return 0;
}
