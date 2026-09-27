/*
 * Live Volume Balancer DSP tests
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "leveler-dsp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, message)                                                                                       \
	do {                                                                                                              \
		if (!(condition)) {                                                                                       \
			fprintf(stderr, "FAIL: %s\n", message);                                                           \
			return 1;                                                                                         \
		}                                                                                                             \
	} while (0)

static struct lvb_settings test_settings(void)
{
	return (struct lvb_settings){
		.target_lufs = -18.0f,
		.max_boost_db = 12.0f,
		.max_reduction_db = 18.0f,
		.attack_ms = 60.0f,
		.release_ms = 350.0f,
		.noise_floor_db = -55.0f,
		.peak_ceiling_db = -1.0f,
		.mode = LVB_MODE_CUSTOM,
		.bypass = false,
	};
}

static void fill_sine(float *samples, size_t frames, float sample_rate, float frequency, float amplitude,
		      size_t first_frame, float phase)
{
	for (size_t i = 0; i < frames; i++)
		samples[i] = amplitude *
			     sinf(6.28318530717958647692f * frequency * (float)(first_frame + i) / sample_rate + phase);
}

static float rms(const float *samples, size_t frames, size_t skip)
{
	double sum = 0.0;
	for (size_t i = skip; i < frames; i++)
		sum += (double)samples[i] * samples[i];
	return frames > skip ? (float)sqrt(sum / (double)(frames - skip)) : 0.0f;
}

static float sample_peak(const float *samples, size_t frames)
{
	float peak = 0.0f;
	for (size_t i = 0; i < frames; i++) {
		const float magnitude = fabsf(samples[i]);
		if (magnitude > peak)
			peak = magnitude;
	}
	return peak;
}

static int test_quiet_signal_is_boosted_and_channels_are_linked(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float left[9600];
	float right[9600];
	float *planes[] = {left, right};
	fill_sine(left, 9600, 48000.0f, 550.0f, 0.02f, 0, 0.0f);
	for (size_t i = 0; i < 9600; i++)
		right[i] = -left[i];

	lvb_process(&state, &settings, 2, planes, 9600, 48000.0f);
	CHECK(rms(left, 9600, LVB_TRUE_PEAK_LATENCY) > 0.016f, "quiet signal should receive upward gain");
	for (size_t i = LVB_TRUE_PEAK_LATENCY; i < 9600; i++)
		CHECK(fabsf(left[i] + right[i]) < 2e-6f, "linked gain must preserve the stereo image");
	return 0;
}

static int test_loud_signal_is_reduced_and_sample_ceiling_is_respected(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float loud[12000];
	float original[12000];
	float *planes[] = {loud};
	fill_sine(loud, 12000, 48000.0f, 1000.0f, 0.90f, 0, 0.3f);
	memcpy(original, loud, sizeof(loud));

	lvb_process(&state, &settings, 1, planes, 12000, 48000.0f);
	CHECK(rms(loud, 12000, 1000) < rms(original, 12000, 1000), "loud signal should receive downward gain");
	const float ceiling = powf(10.0f, settings.peak_ceiling_db / 20.0f);
	CHECK(sample_peak(loud, 12000) <= ceiling + 1e-5f, "sample fallback must stay under the selected ceiling");
	CHECK(state.stats.gain_reduction_db > 0.0f, "meter should report active gain reduction");
	return 0;
}

static int run_peak_tone(float sample_rate, bool bypass, struct lvb_stats *result)
{
	struct lvb_settings settings = test_settings();
	settings.mode = LVB_MODE_CUSTOM;
	settings.target_lufs = -9.0f;
	settings.max_boost_db = 0.0f;
	settings.max_reduction_db = 0.0f;
	settings.peak_ceiling_db = -2.0f;
	settings.bypass = bypass;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[1024];
	float *planes[] = {samples};
	const float frequency = 0.225f * sample_rate;
	const float angular = 6.28318530717958647692f * frequency / sample_rate;
	const float phase = 1.57079632679489661923f - angular * 0.5f;
	size_t position = 0;
	for (size_t block = 0; block < 40; block++) {
		const size_t frames = block % 3 == 0 ? 193 : (block % 3 == 1 ? 257 : 511);
		fill_sine(samples, frames, sample_rate, frequency, 0.89f, position, phase);
		lvb_process(&state, &settings, 1, planes, frames, sample_rate);
		position += frames;
	}
	*result = state.stats;
	return 0;
}

static int run_peak_burst(float sample_rate, float *maximum_dbtp)
{
	struct lvb_settings settings = test_settings();
	settings.mode = LVB_MODE_CUSTOM;
	settings.target_lufs = -9.0f;
	settings.max_boost_db = 0.0f;
	settings.max_reduction_db = 0.0f;
	settings.peak_ceiling_db = -2.0f;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[257];
	float *planes[] = {samples};
	const float frequency = 0.225f * sample_rate;
	const float angular = 6.28318530717958647692f * frequency / sample_rate;
	float peak = -120.0f;
	size_t position = 0;
	for (size_t block = 0; block < 100; block++) {
		const size_t frames = block % 2 == 0 ? 193 : 257;
		for (size_t i = 0; i < frames; i++) {
			const bool burst_on = (position + i) % 173U < 61U;
			samples[i] = burst_on ? 0.99f * sinf(angular * (float)(position + i) + 0.7f) : 0.0f;
		}
		lvb_process(&state, &settings, 1, planes, frames, sample_rate);
		if (state.stats.true_peak_dbtp > peak)
			peak = state.stats.true_peak_dbtp;
		position += frames;
	}
	*maximum_dbtp = peak;
	return 0;
}

static int test_fir_intersample_peak_guard_at_both_rates(void)
{
	struct lvb_stats bypassed;
	struct lvb_stats limited_48k;
	struct lvb_stats limited_44k;
	float burst_peak_48k;
	float burst_peak_44k;
	CHECK(run_peak_tone(48000.0f, true, &bypassed) == 0, "bypassed FIR peak sample");
	CHECK(run_peak_tone(48000.0f, false, &limited_48k) == 0, "48 kHz FIR peak sample");
	CHECK(run_peak_tone(44100.0f, false, &limited_44k) == 0, "44.1 kHz FIR peak sample");
	CHECK(bypassed.true_peak_dbtp > -2.5f,
	      "4x FIR should detect the high-frequency intersample peak above the -2 dBTP ceiling");
	CHECK(limited_48k.true_peak_dbtp <= -1.85f, "48 kHz FIR limiter should meet the requested output ceiling");
	CHECK(limited_44k.true_peak_dbtp <= -1.80f, "44.1 kHz FIR limiter should meet the requested output ceiling");
	CHECK(run_peak_burst(48000.0f, &burst_peak_48k) == 0, "48 kHz burst peak sample");
	CHECK(run_peak_burst(44100.0f, &burst_peak_44k) == 0, "44.1 kHz burst peak sample");
	CHECK(burst_peak_48k <= -1.70f, "48 kHz FIR guard should contain short intersample peak bursts");
	CHECK(burst_peak_44k <= -1.70f, "44.1 kHz FIR guard should contain short intersample peak bursts");
	return 0;
}

static int test_meter_windows_and_voice_activity(void)
{
	struct lvb_settings settings = test_settings();
	settings.bypass = true;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[4800];
	float *planes[] = {samples};
	for (size_t block = 0; block < 35; block++) {
		fill_sine(samples, 4800, 48000.0f, 900.0f, 0.08f, block * 4800, 0.0f);
		lvb_process(&state, &settings, 1, planes, 4800, 48000.0f);
	}
	CHECK(state.stats.momentary_lufs > -35.0f && state.stats.momentary_lufs < -8.0f,
	      "400 ms LUFS meter should report a plausible active signal level");
	CHECK(fabsf(state.stats.short_term_lufs - state.stats.momentary_lufs) < 0.2f,
	      "3 s short-term meter should settle to a steady signal");
	CHECK(state.stats.voice_active, "voice-like activity detector should report a sustained vocal-band tone");
	return 0;
}

static int test_calibration_is_timed_and_mode_specific(void)
{
	struct lvb_settings settings = test_settings();
	settings.mode = LVB_MODE_WORSHIP;
	settings.mode_trim_db[LVB_MODE_WORSHIP] = 0.5f;
	struct lvb_state worship_state;
	lvb_state_init(&worship_state);
	lvb_calibration_start(&worship_state, &settings);
	float samples[4800];
	float *planes[] = {samples};
	for (size_t block = 0; block < 101; block++) {
		if (block == 50)
			settings.mode = LVB_MODE_SERMON;
		fill_sine(samples, 4800, 48000.0f, 440.0f, 0.15f, block * 4800, 0.0f);
		lvb_process(&worship_state, &settings, 1, planes, 4800, 48000.0f);
	}
	CHECK(worship_state.stats.calibration_ready, "10 s calibration should return an applyable suggestion");
	const float worship_suggestion = worship_state.stats.calibration_suggestion_db;
	CHECK(fabsf(worship_suggestion - (-20.0f - worship_state.stats.calibration_measured_lufs)) < 0.05f,
	      "worship calibration should correct the measured processed output toward its preset target");
	CHECK(worship_state.stats.mode == LVB_MODE_SERMON,
	      "the selected mode should still be tracked during calibration");
	CHECK(worship_state.stats.calibration_mode == LVB_MODE_WORSHIP,
	      "calibration suggestion should remain assigned to the mode where measurement started");

	settings.mode = LVB_MODE_SERMON;
	settings.mode_trim_db[LVB_MODE_SERMON] = 0.0f;
	struct lvb_state sermon_state;
	lvb_state_init(&sermon_state);
	lvb_calibration_start(&sermon_state, &settings);
	for (size_t block = 0; block < 101; block++) {
		fill_sine(samples, 4800, 48000.0f, 440.0f, 0.15f, block * 4800, 0.0f);
		lvb_process(&sermon_state, &settings, 1, planes, 4800, 48000.0f);
	}
	CHECK(sermon_state.stats.calibration_ready, "sermon calibration should also complete");
	CHECK(sermon_state.stats.mode == LVB_MODE_SERMON, "second calibration should retain the sermon mode");
	CHECK(sermon_state.stats.calibration_mode == LVB_MODE_SERMON,
	      "sermon calibration should retain its own saved mode");
	CHECK(fabsf(sermon_state.stats.calibration_suggestion_db -
		    (-18.0f - sermon_state.stats.calibration_measured_lufs)) < 0.05f,
	      "sermon calibration should correct the measured processed output toward its preset target");

	settings.mode = LVB_MODE_WORSHIP_ACOUSTIC;
	settings.mode_trim_db[LVB_MODE_WORSHIP_ACOUSTIC] = 1.25f;
	struct lvb_state acoustic_state;
	lvb_state_init(&acoustic_state);
	lvb_calibration_start(&acoustic_state, &settings);
	for (size_t block = 0; block < 101; block++) {
		fill_sine(samples, 4800, 48000.0f, 440.0f, 0.15f, block * 4800, 0.0f);
		lvb_process(&acoustic_state, &settings, 1, planes, 4800, 48000.0f);
	}
	CHECK(acoustic_state.stats.calibration_ready, "acoustic worship calibration should complete");
	CHECK(acoustic_state.stats.mode == LVB_MODE_WORSHIP_ACOUSTIC,
	      "calibration should retain its separate acoustic worship mode");
	CHECK(acoustic_state.stats.calibration_mode == LVB_MODE_WORSHIP_ACOUSTIC,
	      "acoustic calibration should keep a separate saved mode");
	CHECK(fabsf(acoustic_state.stats.calibration_suggestion_db -
		    (-18.0f - acoustic_state.stats.calibration_measured_lufs)) < 0.05f,
	      "acoustic calibration should correct processed output while retaining its separate trim");
	return 0;
}

static int test_calibration_moves_quiet_sermon_toward_target(void)
{
	struct lvb_settings settings = test_settings();
	settings.mode = LVB_MODE_SERMON;
	settings.noise_floor_db = -70.0f;
	settings.mode_trim_db[LVB_MODE_SERMON] = 0.0f;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[4800];
	float *planes[] = {samples};

	lvb_calibration_start(&state, &settings);
	for (size_t block = 0; block < 101; block++) {
		fill_sine(samples, 4800, 48000.0f, 440.0f, 0.025f, block * 4800, 0.0f);
		lvb_process(&state, &settings, 1, planes, 4800, 48000.0f);
	}
	CHECK(state.stats.calibration_ready, "quiet sermon calibration should finish");
	const float measured_before = state.stats.calibration_measured_lufs;
	const float target = -18.0f;
	CHECK(measured_before < target - 1.0f,
	      "boost-limited sermon output should measure below target before calibration");
	CHECK(state.stats.calibration_suggestion_db > 0.0f,
	      "calibration should recommend positive output makeup for a quiet sermon");
	settings.mode_trim_db[LVB_MODE_SERMON] = state.stats.calibration_suggestion_db;

	lvb_calibration_start(&state, &settings);
	for (size_t block = 0; block < 101; block++) {
		fill_sine(samples, 4800, 48000.0f, 440.0f, 0.025f, block * 4800, 0.0f);
		lvb_process(&state, &settings, 1, planes, 4800, 48000.0f);
	}
	const float measured_after = state.stats.calibration_measured_lufs;
	CHECK(fabsf(target - measured_after) < fabsf(target - measured_before),
	      "applying the measured output trim should move quiet sermon level toward target");
	CHECK(measured_after > measured_before + 3.0f,
	      "output makeup should still work after the level rider reaches its maximum boost");
	CHECK(state.stats.true_peak_dbtp <= settings.peak_ceiling_db + 0.25f,
	      "calibrated makeup should remain inside the output peak ceiling");
	return 0;
}

static float run_worship_mode(enum lvb_mode mode, struct lvb_stats *stats)
{
	struct lvb_settings settings = test_settings();
	settings.mode = mode;
	settings.noise_floor_db = -80.0f;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[480];
	float *planes[] = {samples};
	float result_rms = 0.0f;
	for (size_t block = 0; block < 200; block++) {
		fill_sine(samples, 480, 48000.0f, 440.0f, 0.002f, block * 480, 0.2f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
		if (block == 199)
			result_rms = rms(samples, 480, LVB_TRUE_PEAK_LATENCY);
	}
	lvb_get_stats(&state, stats);
	return result_rms;
}

static int test_acoustic_mode_lifts_more_and_mode_switch_is_smooth(void)
{
	struct lvb_stats full_band_stats;
	struct lvb_stats acoustic_stats;
	const float full_band_rms = run_worship_mode(LVB_MODE_WORSHIP, &full_band_stats);
	const float acoustic_rms = run_worship_mode(LVB_MODE_WORSHIP_ACOUSTIC, &acoustic_stats);
	CHECK(acoustic_stats.gain_db > full_band_stats.gain_db + 4.0f,
	      "acoustic worship should allow more lift than full-band worship");
	CHECK(acoustic_rms > full_band_rms * 1.4f,
	      "acoustic worship output should lift a quiet feed more than the gentle full-band mode");

	struct lvb_settings settings = test_settings();
	settings.mode = LVB_MODE_WORSHIP_ACOUSTIC;
	settings.noise_floor_db = -80.0f;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[480];
	float *planes[] = {samples};
	struct lvb_stats before;
	struct lvb_stats after;
	for (size_t block = 0; block < 200; block++) {
		fill_sine(samples, 480, 48000.0f, 440.0f, 0.002f, block * 480, 0.2f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	}
	lvb_get_stats(&state, &before);
	settings.mode = LVB_MODE_WORSHIP;
	fill_sine(samples, 480, 48000.0f, 440.0f, 0.002f, 96000, 0.2f);
	lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	lvb_get_stats(&state, &after);
	CHECK(fabsf(after.gain_db - before.gain_db) < 0.6f,
	      "mode switching should smooth gain changes instead of stepping immediately");
	return 0;
}

static int test_noise_floor_and_auto_voice_gate_never_apply_boost(void)
{
	struct lvb_settings settings = test_settings();
	settings.mode = LVB_MODE_CUSTOM;
	settings.max_boost_db = 12.0f;
	settings.noise_floor_db = -80.0f;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[480];
	float *planes[] = {samples};
	for (size_t block = 0; block < 120; block++) {
		fill_sine(samples, 480, 48000.0f, 440.0f, 0.05f, block * 480, 0.1f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	}
	CHECK(state.gain > 1.5f, "a quiet but audible input should establish prior boost for floor safety test");

	settings.noise_floor_db = -40.0f;
	for (size_t block = 0; block < 100; block++) {
		fill_sine(samples, 480, 48000.0f, 440.0f, 0.001f, block * 480, 0.1f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	}
	CHECK(state.stats.momentary_lufs < settings.noise_floor_db,
	      "quiet tail should fall below the configured floor");
	fill_sine(samples, 480, 48000.0f, 440.0f, 0.001f, 48000, 0.1f);
	lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	CHECK(rms(samples, 480, LVB_TRUE_PEAK_LATENCY) <= 0.001f,
	      "noise-floor protection must not carry previous positive gain into quiet input");

	settings.mode = LVB_MODE_AUTO_ASSIST;
	settings.noise_floor_db = -100.0f;
	state = (struct lvb_state){0};
	lvb_state_init(&state);
	for (size_t block = 0; block < 100; block++) {
		fill_sine(samples, 480, 48000.0f, 1000.0f, 0.01f, block * 480, 0.1f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	}
	CHECK(state.stats.voice_active, "voice-like program should open the Auto Assist gate");
	for (size_t block = 0; block < 50; block++) {
		fill_sine(samples, 480, 48000.0f, 25.0f, 0.01f, block * 480, 0.1f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	}
	CHECK(!state.stats.voice_active, "very low-frequency non-voice should close the assist gate");
	fill_sine(samples, 480, 48000.0f, 25.0f, 0.01f, 24000, 0.1f);
	lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	CHECK(rms(samples, 480, LVB_TRUE_PEAK_LATENCY) <= 0.01f,
	      "Auto Assist must not carry previous voice boost into a non-voice segment");
	CHECK(state.stats.gain_db <= 0.01f, "signed gain meter should not report active boost while voice is gated");
	return 0;
}

static int test_latency_is_fixed_across_bypass_and_mode_changes(void)
{
	struct lvb_settings settings = test_settings();
	settings.bypass = true;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[32];
	float original[32];
	float *planes[] = {samples};
	for (size_t i = 0; i < 32; i++)
		samples[i] = (float)i / 40.0f;
	memcpy(original, samples, sizeof(samples));
	lvb_process(&state, &settings, 1, planes, 16, 48000.0f);
	CHECK(samples[0] == 0.0f, "fixed lookahead should start with six samples of silence");
	CHECK(samples[LVB_TRUE_PEAK_LATENCY] == original[0], "bypass should preserve the fixed six-sample delay");

	settings.bypass = false;
	settings.mode = LVB_MODE_SERMON;
	lvb_process(&state, &settings, 1, planes, 16, 48000.0f);
	CHECK(state.true_peak_delay_frames == LVB_TRUE_PEAK_LATENCY, "mode changes must not alter limiter latency");
	return 0;
}

static int test_null_planes_and_non_finite_samples_are_safe(void)
{
	struct lvb_settings settings = test_settings();
	settings.target_lufs = NAN;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[16] = {NAN, INFINITY, 0.01f};
	float *planes[] = {samples, NULL};

	lvb_process(&state, &settings, 2, planes, 16, 48000.0f);
	CHECK(isnan(samples[LVB_TRUE_PEAK_LATENCY]), "NaN audio should remain unchanged after fixed latency");
	CHECK(isinf(samples[LVB_TRUE_PEAK_LATENCY + 1]), "infinite audio should remain unchanged after fixed latency");
	CHECK(isfinite(state.gain), "invalid targets should be clamped safely");
	return 0;
}

int main(void)
{
	CHECK(test_quiet_signal_is_boosted_and_channels_are_linked() == 0, "quiet linked signal");
	CHECK(test_loud_signal_is_reduced_and_sample_ceiling_is_respected() == 0, "loud signal and sample ceiling");
	CHECK(test_fir_intersample_peak_guard_at_both_rates() == 0, "FIR true peak guard");
	CHECK(test_meter_windows_and_voice_activity() == 0, "LUFS windows and voice activity");
	CHECK(test_calibration_is_timed_and_mode_specific() == 0, "timed mode calibration");
	CHECK(test_calibration_moves_quiet_sermon_toward_target() == 0,
	      "processed-output calibration should lift a quiet sermon");
	CHECK(test_acoustic_mode_lifts_more_and_mode_switch_is_smooth() == 0,
	      "acoustic mode and smooth mode switching");
	CHECK(test_noise_floor_and_auto_voice_gate_never_apply_boost() == 0,
	      "noise-floor and voice-gate boost protection");
	CHECK(test_latency_is_fixed_across_bypass_and_mode_changes() == 0, "fixed limiter latency");
	CHECK(test_null_planes_and_non_finite_samples_are_safe() == 0, "null and non-finite audio");
	puts("All leveler DSP tests passed.");
	return 0;
}
