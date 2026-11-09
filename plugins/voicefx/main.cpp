// main.cpp - the voicefx plugin: VoiceFX, a sound filter for WaveSoundBuffer.filters
//
//   var fx = new VoiceFX();
//   fx.preset = "phone";               // or set the settings one by one: fx.highcut = 3000
//   buffer.filters.add(fx);            // before the sound is opened: the engine builds its
//   buffer.open("ai0001");             // filter chain as it opens one
//
// A VoiceFX serves one buffer at a time. The settings may change while it plays (from the next
// block on). It may be invalidated while a buffer has it: the buffer lets go of it later (as it
// opens another sound, or goes), and it goes then. With an echo or a reverb, the sound plays on after its end until they have died
// away (the buffer's position stays at the end meanwhile).
//
// Members: preset (a name; setting it sets every setting), the settings by name (lowcut,
// highcut, order, drive, mono, delay, feedback, echo, damping, room, reverb, dry, gain, tail:
// see dsp.h), set(name, value), get(name), load(dictionary: "preset" first, then settings),
// interface (for the engine), connected, and VoiceFX.presets() (the names).
//
// SPDX-License-Identifier: 0BSD

#include "ncbind.hpp"

#include "dsp.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ------------------------------------------------------------------------
// The engine's sound filter interfaces, as the engine declares them (common/sound/WaveIntf.h,
// WaveLoopManager.h); the filter never touches the segment queue, so it stays opaque. In a
// namespace of their own: where plugins are linked into the engine (the web, Android, iOS) the
// engine's classes of these names are there too, and the engine reaches these only through
// their pointers, so only their layout matters
// ------------------------------------------------------------------------

namespace voicefx_engine {

class tTVPWaveSegmentQueue;

class tTVPSampleAndLabelSource {
public:
	virtual void Decode(void *dest, tjs_uint samples, tjs_uint &written, tTVPWaveSegmentQueue &segments) = 0;
	virtual const tTVPWaveFormat &GetFormat() const = 0;
	virtual ~tTVPSampleAndLabelSource() {}
};

class iTVPBasicWaveFilter {
public:
	virtual tTVPSampleAndLabelSource *Recreate(tTVPSampleAndLabelSource *source) = 0;
	virtual void Clear(void) = 0;
	virtual void Update(void) = 0;
	virtual void Reset(void) = 0;
};

} // namespace voicefx_engine

using voicefx_engine::iTVPBasicWaveFilter;
using voicefx_engine::tTVPSampleAndLabelSource;
using voicefx_engine::tTVPWaveSegmentQueue;

namespace {

std::string toAscii(const ttstr &s) {
	std::string out;
	for (const tjs_char *p = s.c_str(); *p; p++) out.push_back(*p < 128 ? char(*p) : '?');
	return out;
}

} // namespace

// What the engine holds: the buffer keeps its pointer from open() until it opens another sound
// or goes (Clear), which may be after the script's VoiceFX is gone (invalidated, or collected),
// so the script's object only lets it go (release), and it deletes itself once no buffer has it,
// as the engine's PhaseVocoder outlives its script object's invalidation
class VoiceFXCore : public iTVPBasicWaveFilter, public tTVPSampleAndLabelSource {
public:
	std::mutex lock;
	voicefx::Settings settings;   // what scripts set
	bool dirty = true;
	std::string presetName = "none";

	// the decoding thread's side
	tTVPSampleAndLabelSource *source = nullptr;
	tTVPWaveFormat inFormat, outFormat;
	voicefx::Effect effect;
	std::vector<uint8_t> convert;
	bool sourceEnded = false;
	uint64_t tailFrames = 0;
	bool released = false;   // the script's object is gone

	VoiceFXCore() {
		std::memset(&inFormat, 0, sizeof inFormat);
		std::memset(&outFormat, 0, sizeof outFormat);
	}
	virtual ~VoiceFXCore() {}

	// the script's object is gone: delete now, or once the buffer that has it lets it go
	void release() {
		std::unique_lock<std::mutex> hold(lock);
		if (source) {
			released = true;
			return;
		}
		hold.unlock();
		delete this;
	}

	// --- the engine's side ---

	tTVPSampleAndLabelSource *Recreate(tTVPSampleAndLabelSource *src) override {
		std::lock_guard<std::mutex> hold(lock);
		if (source && source != src) {
			// one buffer at a time: the second plays without the effect
			TVPAddLog(TJS_W("voicefx: a VoiceFX serves one buffer at a time; this one plays without it"));
			return src;
		}
		source = src;
		inFormat = src->GetFormat();
		outFormat = inFormat;
		outFormat.IsFloat = true;
		outFormat.BitsPerSample = 32;
		outFormat.BytesPerSample = 4;
		effect.configure(settings);
		effect.start(double(inFormat.SamplesPerSec), int(inFormat.Channels));
		dirty = false;
		sourceEnded = false;
		tailFrames = 0;
		return this;
	}

	void Clear() override {
		std::unique_lock<std::mutex> hold(lock);
		source = nullptr;
		convert.clear();
		convert.shrink_to_fit();
		if (!released) return;
		hold.unlock();
		delete this;
	}

	void Update() override {}

	void Reset() override {
		std::lock_guard<std::mutex> hold(lock);
		effect.reset();
		sourceEnded = false;
		tailFrames = 0;
	}

	const tTVPWaveFormat &GetFormat() const override { return outFormat; }

	void Decode(void *dest, tjs_uint samples, tjs_uint &written, tTVPWaveSegmentQueue &segments) override {
		std::lock_guard<std::mutex> hold(lock);
		written = 0;
		if (!source) return;
		if (dirty) {
			effect.configure(settings);
			dirty = false;
		}
		const int channels = int(inFormat.Channels);
		float *out = static_cast<float *>(dest);
		tjs_uint got = 0;
		if (!sourceEnded) {
			if (inFormat.IsFloat && inFormat.BitsPerSample == 32 && inFormat.BytesPerSample == 4) {
				source->Decode(dest, samples, got, segments);
			} else {
				size_t bytes = size_t(samples) * inFormat.BytesPerSample * size_t(channels);
				if (convert.size() < bytes) convert.resize(bytes);
				source->Decode(convert.data(), samples, got, segments);
				if (got) TVPConvertPCMToFloat(out, convert.data(), inFormat, tjs_int(got));
			}
			if (got < samples) sourceEnded = true;
		}
		if (got < samples) std::memset(out + size_t(got) * size_t(channels), 0, size_t(samples - got) * sizeof(float) * size_t(channels));
		effect.process(out, int(samples));
		if (!sourceEnded) {
			written = got;
			return;
		}
		// the voice is over: the echo and the reverb ring on while they're heard
		uint64_t maxTail = uint64_t(effect.settings().tail * inFormat.SamplesPerSec / 1000.0);
		if (effect.settings().hasTail() && effect.ringing() && tailFrames < maxTail) {
			tailFrames += samples - got;
			written = samples;
		} else {
			written = got;
		}
	}

};

// The script's object
class VoiceFX {
	VoiceFXCore *core;
	std::mutex &lock;
	voicefx::Settings &settings;
	bool &dirty;
	std::string &presetName;

public:
	VoiceFX()
	    : core(new VoiceFXCore()), lock(core->lock), settings(core->settings), dirty(core->dirty),
	      presetName(core->presetName) {}
	~VoiceFX() { core->release(); }

	tjs_int64 getInterface() { return tjs_int64(reinterpret_cast<intptr_t>(static_cast<iTVPBasicWaveFilter *>(core))); }
	bool getConnected() {
		std::lock_guard<std::mutex> hold(lock);
		return core->source != nullptr;
	}

	ttstr getPreset() { return ttstr(presetName.c_str()); }
	void setPreset(ttstr name) {
		std::lock_guard<std::mutex> hold(lock);
		std::string n = toAscii(name);
		voicefx::Settings s;
		if (!s.preset(n)) {
			TVPAddLog(ttstr(TJS_W("voicefx: no preset ")) + name);
			return;
		}
		settings = s;
		presetName = n.empty() ? "none" : n;
		dirty = true;
	}

	bool set(ttstr name, tjs_real value) {
		std::lock_guard<std::mutex> hold(lock);
		bool ok = settings.set(toAscii(name), double(value));
		if (ok) dirty = true;
		return ok;
	}

	tTJSVariant get(ttstr name) {
		std::lock_guard<std::mutex> hold(lock);
		double v = 0;
		if (!settings.get(toAscii(name), v)) return tTJSVariant();
		return tTJSVariant(tjs_real(v));
	}

	// "preset" first, then each setting the dictionary has
	void load(tTJSVariant d) {
		if (d.Type() == tvtString) {
			setPreset(ttstr(d));
			return;
		}
		if (d.Type() != tvtObject || !d.AsObjectNoAddRef()) return;
		iTJSDispatch2 *dict = d.AsObjectNoAddRef();
		tTJSVariant p;
		if (TJS_SUCCEEDED(dict->PropGet(0, TJS_W("preset"), nullptr, &p, dict)) && p.Type() != tvtVoid)
			setPreset(ttstr(p));
		for (const std::string &n : voicefx::settingNames()) {
			std::vector<tjs_char> w(n.begin(), n.end());
			w.push_back(0);
			tTJSVariant v;
			if (TJS_SUCCEEDED(dict->PropGet(0, w.data(), nullptr, &v, dict)) && v.Type() != tvtVoid)
				set(ttstr(w.data()), tjs_real(v));
		}
	}

	static tjs_error TJS_INTF_METHOD getPresets(tTJSVariant *result, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
		static const tjs_char *const names[] = { TJS_W("none"), TJS_W("echo"), TJS_W("phone"), TJS_W("muffled"),
		                                         TJS_W("hall"), TJS_W("cave"), TJS_W("radio") };
		iTJSDispatch2 *arr = TJSCreateArrayObject();
		for (tjs_int k = 0; k < tjs_int(sizeof names / sizeof names[0]); k++) {
			tTJSVariant v(names[k]);
			arr->PropSetByNum(TJS_MEMBERENSURE, k, &v, arr);
		}
		if (result) *result = tTJSVariant(arr, arr);
		arr->Release();
		return TJS_S_OK;
	}

#define VOICEFX_SETTING(n)                                                                                             \
	tjs_real get_##n() {                                                                                               \
		std::lock_guard<std::mutex> hold(lock);                                                                        \
		double v = 0;                                                                                                  \
		settings.get(#n, v);                                                                                           \
		return tjs_real(v);                                                                                            \
	}                                                                                                                  \
	void set_##n(tjs_real v) { set(TJS_W(#n), v); }
	VOICEFX_SETTING(lowcut)
	VOICEFX_SETTING(highcut)
	VOICEFX_SETTING(order)
	VOICEFX_SETTING(drive)
	VOICEFX_SETTING(mono)
	VOICEFX_SETTING(delay)
	VOICEFX_SETTING(feedback)
	VOICEFX_SETTING(echo)
	VOICEFX_SETTING(damping)
	VOICEFX_SETTING(room)
	VOICEFX_SETTING(reverb)
	VOICEFX_SETTING(dry)
	VOICEFX_SETTING(gain)
	VOICEFX_SETTING(tail)
#undef VOICEFX_SETTING
};

NCB_REGISTER_CLASS(VoiceFX) {
	NCB_CONSTRUCTOR(());
	NCB_PROPERTY_RO(interface, getInterface);
	NCB_PROPERTY_RO(connected, getConnected);
	NCB_PROPERTY(preset, getPreset, setPreset);
	NCB_METHOD(set);
	NCB_METHOD(get);
	NCB_METHOD(load);
	NCB_METHOD_RAW_CALLBACK(presets, &VoiceFX::getPresets, TJS_STATICMEMBER);
	NCB_PROPERTY(lowcut, get_lowcut, set_lowcut);
	NCB_PROPERTY(highcut, get_highcut, set_highcut);
	NCB_PROPERTY(order, get_order, set_order);
	NCB_PROPERTY(drive, get_drive, set_drive);
	NCB_PROPERTY(mono, get_mono, set_mono);
	NCB_PROPERTY(delay, get_delay, set_delay);
	NCB_PROPERTY(feedback, get_feedback, set_feedback);
	NCB_PROPERTY(echo, get_echo, set_echo);
	NCB_PROPERTY(damping, get_damping, set_damping);
	NCB_PROPERTY(room, get_room, set_room);
	NCB_PROPERTY(reverb, get_reverb, set_reverb);
	NCB_PROPERTY(dry, get_dry, set_dry);
	NCB_PROPERTY(gain, get_gain, set_gain);
	NCB_PROPERTY(tail, get_tail, set_tail);
}
