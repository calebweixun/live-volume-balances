/*
 * Live Volume Balancer DSP tests
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "leveler-dsp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, message)                                                                                \
	do {                                                                                                       \
		if (!(condition)) {                                                                                \
			fprintf(stderr, "FAIL: %s\n", message);                                                    \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

static struct lvb_settings test_settings(void)
{
	return (struct lvb_settings){
		.target_lufs = -18.0f,
		.max_boost_db = 18.0f,
		.max_reduction_db = 18.0f,
		.attack_ms = 180.0f,
		.release_ms = 1800.0f,
		.noise_floor_db = -46.0f,
		.peak_ceiling_db = -1.0f,
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

static float noise_value(size_t frame, float amplitude)
{
	uint32_t value = (uint32_t)(frame + 1U) * 747796405U + 2891336453U;
	value = ((value >> ((value >> 28U) + 4U)) ^ value) * 277803737U;
	value = (value >> 22U) ^ value;
	return amplitude * ((float)value / 2147483648.0f - 1.0f);
}

static float rms(const float *samples, size_t frames, size_t skip)
{
	double sum = 0.0;
	for (size_t i = skip; i < frames; i++)
		sum += (double)samples[i] * samples[i];
	return frames > skip ? (float)sqrt(sum / (double)(frames - skip)) : 0.0f;
}

static float db_to_linear(float db)
{
	return powf(10.0f, db / 20.0f);
}

static float process_segment(struct lvb_state *state, const struct lvb_settings *settings, float sample_rate,
			     float frequency, float amplitude, size_t frames, size_t measure_from,
			     float *maximum_peak_dbtp)
{
	float samples[521];
	float *planes[] = {samples};
	const size_t block_sizes[] = {137, 511, 233, 89, 521, 317};
	size_t position = 0;
	double output_sum = 0.0;
	size_t output_count = 0;
	while (position < frames) {
		const size_t block = block_sizes[(position / 137U) % (sizeof(block_sizes) / sizeof(block_sizes[0]))];
		const size_t count = block < frames - position ? block : frames - position;
		fill_sine(samples, count, sample_rate, frequency, amplitude, position, 0.23f);
		lvb_process(state, settings, 1, planes, count, sample_rate);
		if (maximum_peak_dbtp && state->stats.true_peak_dbtp > *maximum_peak_dbtp)
			*maximum_peak_dbtp = state->stats.true_peak_dbtp;
		for (size_t i = 0; i < count; i++) {
			if (position + i >= measure_from) {
				output_sum += (double)samples[i] * samples[i];
				output_count++;
			}
		}
		position += count;
	}
	return output_count ? (float)sqrt(output_sum / (double)output_count) : 0.0f;
}

static float process_noise_segment(struct lvb_state *state, const struct lvb_settings *settings, float sample_rate,
				   float amplitude, size_t frames, size_t measure_from, float *maximum_peak_dbtp)
{
	float samples[521];
	float *planes[] = {samples};
	const size_t block_sizes[] = {137, 511, 233, 89, 521, 317};
	size_t position = 0;
	double output_sum = 0.0;
	size_t output_count = 0;
	while (position < frames) {
		const size_t block = block_sizes[(position / 137U) % (sizeof(block_sizes) / sizeof(block_sizes[0]))];
		const size_t count = block < frames - position ? block : frames - position;
		for (size_t i = 0; i < count; i++)
			samples[i] = noise_value(position + i, amplitude);
		lvb_process(state, settings, 1, planes, count, sample_rate);
		if (maximum_peak_dbtp && state->stats.true_peak_dbtp > *maximum_peak_dbtp)
			*maximum_peak_dbtp = state->stats.true_peak_dbtp;
		for (size_t i = 0; i < count; i++) {
			if (position + i >= measure_from) {
				output_sum += (double)samples[i] * samples[i];
				output_count++;
			}
		}
		position += count;
	}
	return output_count ? (float)sqrt(output_sum / (double)output_count) : 0.0f;
}

static int test_quiet_audio_is_raised_with_linked_stereo_and_live_meters(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float left[96000];
	float right[96000];
	float *planes[] = {left, right};
	fill_sine(left, 96000, 48000.0f, 550.0f, 0.025f, 0, 0.0f);
	for (size_t i = 0; i < 96000; i++)
		right[i] = -left[i];

	lvb_process(&state, &settings, 2, planes, 96000, 48000.0f);
	CHECK(rms(left, 96000, LVB_TRUE_PEAK_LATENCY) > 0.030f, "quiet audio should receive useful automatic lift");
	for (size_t i = LVB_TRUE_PEAK_LATENCY; i < 96000; i++)
		CHECK(fabsf(left[i] + right[i]) < 2e-6f, "linked gain must preserve the stereo image");
	CHECK(state.stats.gain_db > 1.0f, "gain telemetry should include positive makeup gain");
	CHECK(state.stats.output_momentary_lufs > state.stats.input_momentary_lufs,
	      "the live OUT meter should reflect processed audio");
	CHECK(state.stats.output_short_term_lufs > -120.0f, "the rolling OUT short-term meter should be available");
	return 0;
}

static int test_full_band_to_quiet_sermon_reopens_relative_gate(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	const size_t band_frames = 48000U * 2U;
	const size_t sermon_frames = 48000U * 3U;
	const float band_rms = process_segment(&state, &settings, 48000.0f, 900.0f, 0.62f, band_frames, 0, NULL);
	const float gain_during_band = state.stats.gain_db;
	CHECK(gain_during_band < -1.0f, "loud full-band audio should be gently reduced");
	const float sermon_rms = process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, sermon_frames,
						 sermon_frames - 48000U / 2U, NULL);
	CHECK(state.stats.input_momentary_lufs > -40.0f, "quiet sermon should remain above the activity floor");
	CHECK(state.stats.gain_db > 3.0f,
	      "the relative activity reference must fall quickly enough to lift quiet speech after a loud band");
	CHECK(sermon_rms > 0.020f, "the final sermon output should be raised toward the selected target");
	CHECK(sermon_rms > band_rms * 0.035f, "the reduced band-to-sermon level gap should be materially smaller");
	return 0;
}

static int test_steady_band_and_sermon_outputs_move_toward_same_target(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state band_state;
	struct lvb_state sermon_state;
	lvb_state_init(&band_state);
	lvb_state_init(&sermon_state);
	process_segment(&band_state, &settings, 48000.0f, 900.0f, 0.62f, 48000U * 6U, 0, NULL);
	process_segment(&sermon_state, &settings, 48000.0f, 440.0f, 0.035f, 48000U * 6U, 0, NULL);
	CHECK(band_state.stats.output_short_term_lufs > -21.0f && band_state.stats.output_short_term_lufs < -15.0f,
	      "steady band output should ride near the target while retaining some dynamics");
	CHECK(sermon_state.stats.output_short_term_lufs > -21.0f && sermon_state.stats.output_short_term_lufs < -15.0f,
	      "steady quiet sermon should rise near the same rolling target");
	CHECK(fabsf(band_state.stats.output_short_term_lufs - sermon_state.stats.output_short_term_lufs) < 2.0f,
	      "the automatic rider should materially narrow steady band-to-sermon loudness differences");
	return 0;
}

static int test_quiet_sermon_to_loud_band_reduces_smoothly_and_safely(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U * 2U, 0, NULL);
	const float quiet_gain = state.stats.gain_db;
	CHECK(quiet_gain > 3.0f, "quiet sermon should first establish upward gain");
	float peak = -120.0f;
	process_segment(&state, &settings, 48000.0f, 900.0f, 0.62f, 48000U, 0, &peak);
	CHECK(state.stats.gain_db < quiet_gain - 4.0f,
	      "gain riding should lower gain within a second when a loud band starts");
	CHECK(peak <= settings.peak_ceiling_db + 0.35f,
	      "the output peak guard should remain active during transitions");
	return 0;
}

static int test_short_pauses_do_not_lift_minus_50_dbfs_ambience(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U * 2U, 0, NULL);
	CHECK(state.stats.gain_db > 3.0f, "speech before a pause should have audible automatic lift");

	/* Deterministic broadband noise at about -50 dBFS models room ambience. */
	float pause_peak = -120.0f;
	const float first_half_rms =
		process_noise_segment(&state, &settings, 48000.0f, 0.0055f, 24000U, 0, &pause_peak);
	const float second_half_rms =
		process_noise_segment(&state, &settings, 48000.0f, 0.0055f, 72000U, 48000U, &pause_peak);
	CHECK(!state.stats.activity_open, "the absolute activity floor should close during a quiet pause");
	CHECK(state.stats.input_momentary_lufs < settings.noise_floor_db,
	      "broadband -50 dBFS room noise should sit below the default activity floor");
	CHECK(first_half_rms < 0.0048f, "the first half-second pause should not pull room ambience toward target");
	CHECK(second_half_rms < 0.0037f, "a sustained pause must remain near its original ambience level");
	CHECK(pause_peak <= settings.peak_ceiling_db + 0.35f, "peak protection must remain safe during a pause");
	return 0;
}

static int test_activity_gate_transition_is_smoothed(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[480];
	float *planes[] = {samples};
	for (size_t block = 0; block < 200; block++) {
		fill_sine(samples, 480, 48000.0f, 440.0f, 0.035f, block * 480, 0.23f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
	}
	CHECK(state.stats.gain_db > 5.0f, "speech should establish meaningful upward gain before the pause");
	float previous_gain = state.stats.gain_db;
	float maximum_gain_step = 0.0f;
	for (size_t block = 0; block < 100; block++) {
		fill_sine(samples, 480, 48000.0f, 310.0f, 0.0045f, block * 480, 0.23f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
		const float gain_step = fabsf(state.stats.gain_db - previous_gain);
		if (gain_step > maximum_gain_step)
			maximum_gain_step = gain_step;
		previous_gain = state.stats.gain_db;
	}
	CHECK(maximum_gain_step < 4.0f,
	      "activity closure should fade boosted allowance instead of stepping abruptly to unity");
	CHECK(!state.stats.activity_open, "gate transition should settle closed during the ambience");
	return 0;
}

static int test_stereo_room_noise_uses_per_channel_activity_floor(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float left[480];
	float right[480];
	float *planes[] = {left, right};
	for (size_t block = 0; block < 200; block++) {
		fill_sine(left, 480, 48000.0f, 440.0f, 0.035f, block * 480, 0.23f);
		memcpy(right, left, sizeof(left));
		lvb_process(&state, &settings, 2, planes, 480, 48000.0f);
	}
	CHECK(state.stats.gain_db > 5.0f, "quiet stereo speech should receive automatic compensation");
	double output_energy = 0.0;
	size_t output_frames = 0;
	for (size_t block = 0; block < 50; block++) {
		for (size_t i = 0; i < 480; i++) {
			left[i] = noise_value(block * 480U + i, 0.0055f);
			right[i] = noise_value(block * 480U + i + 91021U, 0.0055f);
		}
		lvb_process(&state, &settings, 2, planes, 480, 48000.0f);
		if (block >= 25) {
			for (size_t i = 0; i < 480; i++) {
				output_energy += (double)left[i] * left[i] + (double)right[i] * right[i];
				output_frames += 2;
			}
		}
	}
	const float output_rms = (float)sqrt(output_energy / (double)output_frames);
	CHECK(!state.stats.activity_open, "two-channel -50 dBFS room ambience must not open the activity gate");
	CHECK(output_rms < 0.0048f, "stereo room noise should remain near its input level during a pause");
	return 0;
}

static float run_peak_tone(float sample_rate, bool bypass, float *maximum_dbtp, float *reported_sample_rate)
{
	struct lvb_settings settings = test_settings();
	settings.max_boost_db = 0.0f;
	settings.max_reduction_db = 0.0f;
	settings.peak_ceiling_db = -2.0f;
	settings.bypass = bypass;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[521];
	float *planes[] = {samples};
	const float frequency = 0.225f * sample_rate;
	const float angular = 6.28318530717958647692f * frequency / sample_rate;
	const float phase = 1.57079632679489661923f - angular * 0.5f;
	const size_t block_sizes[] = {193, 257, 521, 131};
	size_t position = 0;
	float maximum_sample = 0.0f;
	*maximum_dbtp = -120.0f;
	while (position < 48000U) {
		const size_t count = block_sizes[(position / 193U) % 4U];
		const size_t frames = count < 48000U - position ? count : 48000U - position;
		fill_sine(samples, frames, sample_rate, frequency, 0.89f, position, phase);
		lvb_process(&state, &settings, 1, planes, frames, sample_rate);
		if (state.stats.true_peak_dbtp > *maximum_dbtp)
			*maximum_dbtp = state.stats.true_peak_dbtp;
		for (size_t i = 0; i < frames; i++) {
			const float absolute = fabsf(samples[i]);
			if (absolute > maximum_sample)
				maximum_sample = absolute;
		}
		position += frames;
	}
	if (reported_sample_rate)
		*reported_sample_rate = state.stats.sample_rate_hz;
	return maximum_sample;
}

static int test_fir_peak_guard_and_meter_at_44100_and_48000(void)
{
	float bypass_peak_48k;
	float limited_peak_48k;
	float bypass_peak_44k;
	float limited_peak_44k;
	float reported_rate_48k;
	float reported_rate_limited_48k;
	float reported_rate_44k;
	float reported_rate_limited_44k;
	const float bypass_sample_48k = run_peak_tone(48000.0f, true, &bypass_peak_48k, &reported_rate_48k);
	const float limited_sample_48k = run_peak_tone(48000.0f, false, &limited_peak_48k, &reported_rate_limited_48k);
	const float bypass_sample_44k = run_peak_tone(44100.0f, true, &bypass_peak_44k, &reported_rate_44k);
	const float limited_sample_44k = run_peak_tone(44100.0f, false, &limited_peak_44k, &reported_rate_limited_44k);
	CHECK(reported_rate_48k == 48000.0f && reported_rate_limited_48k == 48000.0f && reported_rate_44k == 44100.0f &&
		      reported_rate_limited_44k == 44100.0f,
	      "meter telemetry uses the actual 44.1 and 48 kHz processing rates");
	CHECK(bypass_peak_48k > -2.6f && bypass_peak_44k > -2.6f,
	      "4x FIR detector should report intersample peaks above the selected ceiling");
	CHECK(limited_peak_48k <= -1.8f && limited_peak_44k <= -1.8f,
	      "4x FIR peak guard output should stay near or below the -2 dBTP estimate ceiling at both rates");
	CHECK(bypass_sample_48k > db_to_linear(-2.0f) && bypass_sample_44k > db_to_linear(-2.0f),
	      "unlimited waveform should show the guard has real work to do");
	CHECK(limited_sample_48k <= db_to_linear(-2.0f) + 1e-5f && limited_sample_44k <= db_to_linear(-2.0f) + 1e-5f,
	      "sample fallback must enforce the configured ceiling at both rates");
	return 0;
}

static int test_meter_windows_and_bypass_latency(void)
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
	CHECK(state.stats.input_momentary_lufs > -35.0f && state.stats.input_momentary_lufs < -8.0f,
	      "400 ms rolling IN loudness should report a plausible active level");
	CHECK(fabsf(state.stats.input_short_term_lufs - state.stats.input_momentary_lufs) < 0.2f,
	      "3 s IN loudness should settle to a steady signal");
	CHECK(fabsf(state.stats.output_momentary_lufs - state.stats.input_momentary_lufs) < 0.5f,
	      "bypassed output meter should match the delayed input meter");
	CHECK(state.true_peak_delay_frames == LVB_TRUE_PEAK_LATENCY, "bypass must preserve fixed output delay");
	return 0;
}

static int test_null_planes_nonfinite_samples_and_invalid_settings(void)
{
	struct lvb_settings settings = test_settings();
	settings.target_lufs = NAN;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[16] = {NAN, INFINITY, 0.01f};
	float *planes[] = {samples, NULL};
	lvb_process(&state, &settings, 2, planes, 16, 48000.0f);
	CHECK(samples[LVB_TRUE_PEAK_LATENCY] == 0.0f,
	      "non-finite samples should be made safe rather than propagated to OBS output");
	CHECK(isfinite(state.gain), "invalid saved targets should be clamped safely");
	CHECK(state.stats.true_peak_dbtp <= 0.1f, "sanitized output peak telemetry should remain finite");
	return 0;
}

static int test_fast_meter_ballistics_and_reported_sample_rate(void)
{
	struct lvb_settings settings = test_settings();
	settings.bypass = true;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[2400];
	float *planes[] = {samples};
	fill_sine(samples, 2400, 48000.0f, 900.0f, 0.25f, 0, 0.0f);
	lvb_process(&state, &settings, 1, planes, 2400, 48000.0f);
	CHECK(state.stats.input_fast_rms_dbfs > -18.0f && state.stats.input_fast_rms_dbfs < -14.0f,
	      "30 ms fast meter should respond to a 50 ms signal envelope");
	CHECK(state.stats.sample_rate_hz == 48000.0f, "live telemetry reports the active 48 kHz rate");
	memset(samples, 0, sizeof(samples));
	for (size_t block = 0; block < 6; block++)
		lvb_process(&state, &settings, 1, planes, 2400, 48000.0f);
	CHECK(state.stats.input_fast_rms_dbfs > -30.0f && state.stats.input_fast_rms_dbfs < -21.0f,
	      "150 ms fast meter decay should fall smoothly during silence");
	return 0;
}

static int test_expanded_control_ranges_are_effective(void)
{
	struct lvb_settings settings = test_settings();
	settings.target_lufs = LVB_TARGET_LUFS_MAX;
	settings.max_boost_db = 36.0f;
	settings.max_reduction_db = 36.0f;
	settings.release_ms = 50.0f;
	settings.noise_floor_db = -100.0f;
	settings.peak_ceiling_db = 0.0f;
	struct lvb_state quiet_state;
	lvb_state_init(&quiet_state);
	process_segment(&quiet_state, &settings, 48000.0f, 900.0f, 0.015f, 48000U * 6U, 0, NULL);
	CHECK(quiet_state.stats.target_lufs == LVB_TARGET_LUFS_MAX,
	      "0 LUFS target slider upper endpoint is accepted by DSP");
	CHECK(quiet_state.stats.gain_db > 20.0f, "36 dB upward-compensation range is not truncated by legacy limits");

	settings.target_lufs = LVB_TARGET_LUFS_MAX + 6.0f;
	settings.peak_ceiling_db = -1.0f;
	struct lvb_state peak_limited_state;
	lvb_state_init(&peak_limited_state);
	float peak_limited_dbtp = -120.0f;
	process_segment(&peak_limited_state, &settings, 48000.0f, 900.0f, 0.015f, 48000U * 6U, 48000U * 5U,
			&peak_limited_dbtp);
	CHECK(peak_limited_state.stats.target_lufs == LVB_TARGET_LUFS_MAX,
	      "saved targets above 0 LUFS are clamped to the slider maximum");
	CHECK(peak_limited_dbtp <= -0.8f && peak_limited_state.stats.output_short_term_lufs < -0.5f,
	      "peak ceiling can keep the rolling output below a 0 LUFS target");

	settings.target_lufs = LVB_TARGET_LUFS_MIN;
	settings.max_boost_db = 0.0f;
	settings.attack_ms = 10.0f;
	struct lvb_state loud_state;
	lvb_state_init(&loud_state);
	process_segment(&loud_state, &settings, 48000.0f, 900.0f, 0.80f, 48000U * 4U, 0, NULL);
	CHECK(loud_state.stats.target_lufs == LVB_TARGET_LUFS_MIN,
	      "−36 LUFS target slider lower endpoint is accepted by DSP");
	CHECK(loud_state.stats.gain_db < -25.0f, "36 dB downward-reduction range is not truncated by legacy limits");

	settings.target_lufs = -18.0f;
	settings.max_boost_db = 36.0f;
	settings.noise_floor_db = -100.0f;
	struct lvb_state floor_state;
	lvb_state_init(&floor_state);
	process_segment(&floor_state, &settings, 48000.0f, 900.0f, 0.000025f, 48000U / 10U, 0, NULL);
	CHECK(floor_state.stats.activity_open, "-100 dBFS activity-floor endpoint allows a very quiet signal");

	settings.max_boost_db = 0.0f;
	settings.max_reduction_db = 0.0f;
	settings.peak_ceiling_db = -24.0f;
	struct lvb_state ceiling_state;
	lvb_state_init(&ceiling_state);
	const float measured_peak =
		process_segment(&ceiling_state, &settings, 48000.0f, 900.0f, 0.8f, 48000U, 4800U, NULL);
	CHECK(ceiling_state.stats.peak_ceiling_dbtp == -24.0f,
	      "-24 dBTP estimated ceiling endpoint is accepted by DSP");
	CHECK(measured_peak < db_to_linear(-23.8f), "minimum peak-ceiling range is enforced on output samples");

	struct lvb_settings attack_settings = test_settings();
	attack_settings.target_lufs = -36.0f;
	attack_settings.max_boost_db = 0.0f;
	attack_settings.max_reduction_db = 36.0f;
	attack_settings.noise_floor_db = -100.0f;
	attack_settings.peak_ceiling_db = 0.0f;
	attack_settings.attack_ms = 10.0f;
	struct lvb_state fast_attack_state;
	lvb_state_init(&fast_attack_state);
	process_segment(&fast_attack_state, &attack_settings, 48000.0f, 900.0f, 0.8f, 480U, 0, NULL);
	attack_settings.attack_ms = 20.0f;
	struct lvb_state slower_attack_state;
	lvb_state_init(&slower_attack_state);
	process_segment(&slower_attack_state, &attack_settings, 48000.0f, 900.0f, 0.8f, 480U, 0, NULL);
	CHECK(fast_attack_state.stats.gain_db < slower_attack_state.stats.gain_db - 1.0f,
	      "10 ms attack endpoint responds faster than 20 ms, below the legacy 20 ms minimum");

	struct lvb_settings recovery_settings = test_settings();
	recovery_settings.max_boost_db = 18.0f;
	recovery_settings.noise_floor_db = -100.0f;
	recovery_settings.release_ms = 10000.0f;
	struct lvb_state slow_recovery_state;
	lvb_state_init(&slow_recovery_state);
	process_segment(&slow_recovery_state, &recovery_settings, 48000.0f, 900.0f, 0.035f, 48000U * 2U, 0, NULL);
	recovery_settings.release_ms = 5000.0f;
	struct lvb_state faster_recovery_state;
	lvb_state_init(&faster_recovery_state);
	process_segment(&faster_recovery_state, &recovery_settings, 48000.0f, 900.0f, 0.035f, 48000U * 2U, 0, NULL);
	CHECK(faster_recovery_state.stats.gain_db > slow_recovery_state.stats.gain_db + 0.6f,
	      "10000 ms recovery endpoint is slower than 5000 ms, above the legacy 5000 ms maximum");
	return 0;
}

static int test_plugin_latency_calculation_at_common_sample_rates(void)
{
	const float at_48000 = lvb_output_latency_ms(48000.0f);
	const float at_44100 = lvb_output_latency_ms(44100.0f);
	CHECK(fabsf(at_48000 - 0.125f) < 1e-6f, "six samples equal 0.125 ms at 48 kHz");
	CHECK(fabsf(at_44100 - 0.13605443f) < 1e-6f, "six samples equal about 0.136 ms at 44.1 kHz");
	CHECK(lvb_output_latency_ms(0.0f) == 0.0f && lvb_output_latency_ms(NAN) == 0.0f,
	      "invalid sample rates have no fabricated latency value");
	return 0;
}

int main(void)
{
	CHECK(test_quiet_audio_is_raised_with_linked_stereo_and_live_meters() == 0, "quiet lift, linkage and meters");
	CHECK(test_full_band_to_quiet_sermon_reopens_relative_gate() == 0, "full-band to quiet sermon transition");
	CHECK(test_steady_band_and_sermon_outputs_move_toward_same_target() == 0,
	      "steady band and sermon normalization");
	CHECK(test_quiet_sermon_to_loud_band_reduces_smoothly_and_safely() == 0,
	      "quiet sermon to full-band transition");
	CHECK(test_short_pauses_do_not_lift_minus_50_dbfs_ambience() == 0, "pause and room-noise gate");
	CHECK(test_activity_gate_transition_is_smoothed() == 0, "smooth activity gate closure");
	CHECK(test_stereo_room_noise_uses_per_channel_activity_floor() == 0, "stereo activity floor");
	CHECK(test_fir_peak_guard_and_meter_at_44100_and_48000() == 0, "4x FIR guard at 44.1 and 48 kHz");
	CHECK(test_meter_windows_and_bypass_latency() == 0, "rolling meters and fixed bypass latency");
	CHECK(test_null_planes_nonfinite_samples_and_invalid_settings() == 0,
	      "null planes, non-finite samples and invalid settings");
	CHECK(test_fast_meter_ballistics_and_reported_sample_rate() == 0, "fast meter and sample-rate telemetry");
	CHECK(test_expanded_control_ranges_are_effective() == 0, "expanded DSP control ranges");
	CHECK(test_plugin_latency_calculation_at_common_sample_rates() == 0,
	      "plugin input-to-output latency at 44.1 and 48 kHz");
	puts("All leveler DSP tests passed.");
	return 0;
}
