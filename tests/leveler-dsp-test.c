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
		.noise_floor_db = -46.0f,
		.fader_smoothness = LVB_FADER_SMOOTHNESS_DEFAULT,
		.quiet_attenuation_db = 0.0f,
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

static float process_sermon_with_room_noise(struct lvb_state *state, const struct lvb_settings *settings,
					    size_t first_frame, size_t frames, bool voice_active, float voice_amplitude,
					    float noise_amplitude, float *maximum_gain_step_db,
					    float *maximum_peak_dbtp)
{
	float samples[521];
	float *planes[] = {samples};
	const float sample_rate = 48000.0f;
	const size_t block_sizes[] = {137, 511, 233, 89, 521, 317};
	size_t position = 0;
	size_t block_index = 0;
	double output_energy = 0.0;
	float previous_gain_db = state->stats.gain_db;
	if (maximum_gain_step_db)
		*maximum_gain_step_db = 0.0f;
	while (position < frames) {
		const size_t requested_count =
			block_sizes[block_index++ % (sizeof(block_sizes) / sizeof(block_sizes[0]))];
		const size_t count = requested_count < frames - position ? requested_count : frames - position;
		for (size_t i = 0; i < count; i++) {
			const size_t frame = first_frame + position + i;
			const float time = (float)frame / sample_rate;
			const float voice =
				voice_active
					? voice_amplitude * (0.72f * sinf(6.28318530717958647692f * 180.0f * time) +
							     0.20f * sinf(6.28318530717958647692f * 370.0f * time) +
							     0.08f * sinf(6.28318530717958647692f * 820.0f * time))
					: 0.0f;
			samples[i] = voice + noise_value(frame, noise_amplitude);
		}
		lvb_process(state, settings, 1, planes, count, sample_rate);
		if (maximum_gain_step_db) {
			const float step = fabsf(state->stats.gain_db - previous_gain_db);
			if (step > *maximum_gain_step_db)
				*maximum_gain_step_db = step;
		}
		previous_gain_db = state->stats.gain_db;
		if (maximum_peak_dbtp && state->stats.true_peak_dbtp > *maximum_peak_dbtp)
			*maximum_peak_dbtp = state->stats.true_peak_dbtp;
		for (size_t i = 0; i < count; i++)
			output_energy += (double)samples[i] * samples[i];
		position += count;
	}
	return frames ? (float)sqrt(output_energy / (double)frames) : 0.0f;
}

static float mixed_syllable_gain_swing(float smoothness, float *maximum_peak_dbtp,
				       float *maximum_quiet_transition_seconds)
{
	struct lvb_settings settings = test_settings();
	settings.fader_smoothness = smoothness;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[521];
	float *planes[] = {samples};
	size_t position = 0;
	double maximum_swing_db = 0.0;
	const size_t block_sizes[] = {137, 511, 233, 89, 521, 317};
	size_t block_index = 0;
	if (maximum_peak_dbtp)
		*maximum_peak_dbtp = -120.0f;
	if (maximum_quiet_transition_seconds)
		*maximum_quiet_transition_seconds = 0.0f;

	/* Establish a continuous music bed before adding short loud vocal phrases. */
	process_segment(&state, &settings, 48000.0f, 330.0f, 0.045f, 48000U * 4U, 0, NULL);
	position = 48000U * 4U;

	for (size_t cycle = 0; cycle < 8; cycle++) {
		double minimum_gain_db = state.gain_db;
		double maximum_gain_db = state.gain_db;
		size_t cycle_position = 0;
		while (cycle_position < 48000U * 6U / 5U) {
			const size_t requested_count =
				block_sizes[block_index++ % (sizeof(block_sizes) / sizeof(block_sizes[0]))];
			const size_t remaining = 48000U * 6U / 5U - cycle_position;
			const size_t count = requested_count < remaining ? requested_count : remaining;
			for (size_t i = 0; i < count; i++) {
				const float time = (float)(position + i) / 48000.0f;
				const float music = 0.045f * sinf(6.28318530717958647692f * 330.0f * time);
				const float voice = cycle_position + i < 48000U / 5U
							    ? 0.24f * sinf(6.28318530717958647692f * 1250.0f * time)
							    : 0.0f;
				samples[i] = music + voice;
			}
			lvb_process(&state, &settings, 1, planes, count, 48000.0f);
			if (cycle_position < 48000U / 5U) {
				if (state.gain_db < minimum_gain_db)
					minimum_gain_db = state.gain_db;
				if (state.gain_db > maximum_gain_db)
					maximum_gain_db = state.gain_db;
			}
			if (maximum_peak_dbtp && state.stats.true_peak_dbtp > *maximum_peak_dbtp)
				*maximum_peak_dbtp = state.stats.true_peak_dbtp;
			if (maximum_quiet_transition_seconds &&
			    state.quiet_transition_seconds > *maximum_quiet_transition_seconds)
				*maximum_quiet_transition_seconds = state.quiet_transition_seconds;
			position += count;
			cycle_position += count;
		}
		const double swing_db = maximum_gain_db - minimum_gain_db;
		if (swing_db > maximum_swing_db)
			maximum_swing_db = swing_db;
	}
	return (float)maximum_swing_db;
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
	const float band_rms = process_segment(&state, &settings, 48000.0f, 900.0f, 0.62f, band_frames, 0, NULL);
	const float gain_during_band = state.stats.gain_db;
	CHECK(gain_during_band < -1.0f, "loud full-band audio should be gently reduced");
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U / 2U, 0, NULL);
	const double gain_after_half_second = state.gain_db;
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U / 2U, 0, NULL);
	const double gain_after_one_second = state.gain_db;
	const float quiet_timer_after_one_second = state.quiet_transition_seconds;
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U, 0, NULL);
	const double gain_after_two_seconds = state.gain_db;
	const float quiet_timer_after_two_seconds = state.quiet_transition_seconds;
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U, 0, NULL);
	const double gain_after_three_seconds = state.gain_db;
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U, 0, NULL);
	const double gain_after_four_seconds = state.gain_db;
	const float sermon_rms =
		process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U, 48000U / 2U, NULL);
	CHECK(state.stats.input_momentary_lufs > -40.0f, "quiet sermon should remain above the activity floor");
	CHECK(gain_after_half_second >= gain_during_band - 0.5f && gain_after_one_second > gain_during_band,
	      "peak headroom may briefly hold the fader after the band, then quiet-sermon gain should rise smoothly");
	CHECK(quiet_timer_after_one_second < 0.80f && quiet_timer_after_two_seconds >= 0.80f,
	      "faster quiet-transition assist should require sustained active low-level audio");
	CHECK(state.stats.gain_db > 3.0f,
	      "a quiet sermon should rise toward target within five seconds after a loud band");
	CHECK(gain_after_three_seconds > gain_after_two_seconds && gain_after_four_seconds > gain_after_three_seconds &&
		      state.gain_db > gain_after_four_seconds,
	      "quiet-sermon compensation should move smoothly upward through the transition");
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
	process_segment(&band_state, &settings, 48000.0f, 900.0f, 0.62f, 48000U * 8U, 0, NULL);
	process_segment(&sermon_state, &settings, 48000.0f, 440.0f, 0.035f, 48000U * 8U, 0, NULL);
	CHECK(band_state.stats.output_short_term_lufs > -21.0f && band_state.stats.output_short_term_lufs < -15.0f,
	      "steady band output should ride near the target while retaining some dynamics");
	CHECK(sermon_state.stats.output_short_term_lufs > -21.0f && sermon_state.stats.output_short_term_lufs < -15.0f,
	      "steady quiet sermon should rise near the same rolling target");
	CHECK(fabsf(band_state.stats.output_short_term_lufs - sermon_state.stats.output_short_term_lufs) < 2.5f,
	      "the automatic rider should materially narrow steady band-to-sermon loudness differences");
	CHECK(band_state.peak_guard_gain > 0.99f,
	      "the rider should settle sustained band level without relying on continuous peak-guard reduction");
	return 0;
}

static int test_fader_curve_reduces_mixed_program_syllable_pumping(void)
{
	float legacy_peak = -120.0f;
	float smooth_peak = -120.0f;
	float maximum_quiet_transition_seconds = 0.0f;
	const float legacy_swing = mixed_syllable_gain_swing(0.0f, &legacy_peak, NULL);
	const float smooth_swing = mixed_syllable_gain_swing(LVB_FADER_SMOOTHNESS_DEFAULT, &smooth_peak,
							     &maximum_quiet_transition_seconds);
	const float maximum_smooth_swing = mixed_syllable_gain_swing(100.0f, NULL, NULL);
	CHECK(legacy_swing > 1.0f, "test phrases must exercise the legacy gain rider");
	CHECK(smooth_swing <= 3.0f, "default fader curve should keep short mixed-program gain swings at or below 3 dB");
	CHECK(maximum_smooth_swing <= 3.0f, "maximum smoothness should keep short phrase gain swings below 3 dB");
	CHECK(maximum_quiet_transition_seconds < 0.80f,
	      "one-second phrase gaps in an active music bed should not trigger the recovery assist");
	CHECK(smooth_swing <= legacy_swing - 5.0f,
	      "default fader curve should reduce short mixed-program gain swings by at least 5 dB");
	CHECK(smooth_peak <= -0.65f, "fader movement must retain independent fast peak protection");
	return 0;
}

static int test_tiny_gain_steps_keep_linear_and_db_state_synchronized(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U * 8U, 0, NULL);

	const double starting_gain_db = state.gain_db - 0.5;
	state.gain_db = starting_gain_db;
	state.gain_target_db = starting_gain_db;
	state.gain = pow(10.0, starting_gain_db / 20.0);
	state.gain_rate_db_per_second = 0.0f;
	const double starting_gain = state.gain;
	float sample[] = {0.02f};
	float *planes[] = {sample};
	lvb_process(&state, &settings, 1, planes, 1, 48000.0f);

	CHECK(state.gain > starting_gain && state.gain_db > starting_gain_db,
	      "sub-ULP per-sample dB movements should accumulate instead of stalling at unity precision");
	CHECK(fabs(20.0 * log10(state.gain) - state.gain_db) < 1e-9,
	      "linear rider gain and its dB control state should remain synchronized");
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

static int test_speech_gaps_hold_fader_and_floor_plus_two_does_not_reopen(void)
{
	struct lvb_settings settings = test_settings();
	settings.target_lufs = -10.0f;
	settings.max_boost_db = 17.0f;
	settings.max_reduction_db = 12.5f;
	settings.noise_floor_db = -50.0f;
	settings.fader_smoothness = 85.0f;
	settings.quiet_attenuation_db = 3.0f;
	settings.peak_ceiling_db = -7.1f;
	struct lvb_state state;
	lvb_state_init(&state);
	float maximum_peak = -120.0f;
	process_sermon_with_room_noise(&state, &settings, 0, 48000U * 4U, true, 0.025f, 0.0069f, NULL, &maximum_peak);
	CHECK(state.stats.gain_db > 8.0f, "quiet sermon should receive a useful lift before short phrase gaps");
	const float gain_before_short_gap = state.stats.gain_db;
	const float first_pause_rms = process_sermon_with_room_noise(&state, &settings, 48000U * 4U, 48000U * 3U / 10U,
								     false, 0.0f, 0.0069f, NULL, &maximum_peak);
	CHECK(fabsf(state.stats.gain_db - gain_before_short_gap) < 0.35f,
	      "a 300 ms speech gap should hold the same fader position instead of quickly cancelling boost");
	CHECK(fabs((double)state.gain_target_db - state.gain_db) < 0.02,
	      "the fader destination should freeze at its current position through a short word gap");
	process_sermon_with_room_noise(&state, &settings, 48000U * 4U + 48000U * 3U / 10U, 48000U / 4U, true, 0.025f,
				       0.0069f, NULL, &maximum_peak);
	const float gain_before_longer_gap = state.stats.gain_db;
	const float second_pause_rms =
		process_sermon_with_room_noise(&state, &settings, 48000U * 4U + 48000U * 3U / 10U + 48000U / 4U, 48000U,
					       false, 0.0f, 0.0069f, NULL, &maximum_peak);
	CHECK(fabsf(state.stats.gain_db - gain_before_longer_gap) < 0.45f,
	      "a one-second inter-phrase pause should remain inside the fader hold interval");
	CHECK(first_pause_rms > 0.0f && second_pause_rms > 0.0f &&
		      fabsf(20.0f * log10f(second_pause_rms / first_pause_rms)) < 1.5f,
	      "room tone should remain at a similar level across short gaps instead of repeatedly appearing and disappearing");
	CHECK(state.stats.activity_open, "the one-second pause should remain inside the 1.2-second activity hold");
	CHECK(state.quiet_transition_seconds < 0.80f,
	      "room tone below the floor should not trigger the active quiet-sermon recovery assist");
	process_sermon_with_room_noise(&state, &settings, 48000U * 5U + 48000U * 55U / 100U, 48000U * 6U, false, 0.0f,
				       0.0069f, NULL, &maximum_peak);
	CHECK(fabsf(state.stats.gain_db + settings.quiet_attenuation_db) < 0.5f,
	      "after a long pause the same fader should settle at the configured quiet attenuation");
	CHECK(!state.stats.activity_open,
	      "noise at about two dB above the absolute floor must not re-open the activity gate");
	const float gain_before_reentry = state.stats.gain_db;
	float maximum_reentry_step = 0.0f;
	process_sermon_with_room_noise(&state, &settings, 48000U * 11U + 48000U * 55U / 100U, 48000U * 3U / 10U, true,
				       0.025f, 0.0069f, &maximum_reentry_step, &maximum_peak);
	CHECK(state.stats.gain_db > gain_before_reentry,
	      "speech should smoothly resume gain riding after long quiet attenuation");
	CHECK(maximum_reentry_step < 0.35f, "reopening the gate should not create a callback-sized gain jump");
	CHECK(maximum_peak <= settings.peak_ceiling_db + 0.35f,
	      "peak protection remains active through speech pauses and re-entry");
	return 0;
}

static int test_optional_quiet_attenuation_fades_without_hard_gating(void)
{
	struct lvb_settings settings = test_settings();
	settings.quiet_attenuation_db = 6.0f;
	struct lvb_state state;
	lvb_state_init(&state);
	process_segment(&state, &settings, 48000.0f, 440.0f, 0.035f, 48000U * 2U, 0, NULL);
	CHECK(state.stats.gain_db > 3.0f, "speech before a pause should receive automatic lift");
	const float input_noise_rms = 0.0055f / sqrtf(3.0f);
	const float output_noise_rms =
		process_noise_segment(&state, &settings, 48000.0f, 0.0055f, 48000U * 6U, 48000U * 5U, NULL);
	CHECK(!state.stats.activity_open, "the activity gate should close during room ambience");
	CHECK(state.stats.quiet_attenuation_db == 6.0f && fabsf(state.stats.gain_db + 6.0f) < 0.5f,
	      "quiet attenuation should be the settled endpoint of the same fader after a long hold");
	CHECK(output_noise_rms < input_noise_rms * 0.58f && output_noise_rms > input_noise_rms * 0.38f,
	      "room ambience should settle near the selected attenuation without being hard-muted");
	return 0;
}

static int test_ambiguous_room_noise_does_not_extend_activity_hangover(void)
{
	struct lvb_settings settings = test_settings();
	settings.target_lufs = -10.0f;
	settings.max_boost_db = 17.0f;
	settings.max_reduction_db = 12.5f;
	settings.noise_floor_db = -50.0f;
	settings.quiet_attenuation_db = 3.0f;
	settings.peak_ceiling_db = -7.1f;
	struct lvb_state state;
	lvb_state_init(&state);
	process_sermon_with_room_noise(&state, &settings, 0, 48000U * 2U, true, 0.025f, 0.0f, NULL, NULL);
	CHECK(state.stats.activity_open && state.stats.gain_db > 3.0f,
	      "speech should open the activity gate and establish a boosted fader position");
	const float gain_after_speech = state.stats.gain_db;
	const float ambiguous_noise_amplitude = db_to_linear(-45.0f) * sqrtf(3.0f);
	process_noise_segment(&state, &settings, 48000.0f, ambiguous_noise_amplitude, 48000U * 8U / 10U, 0, NULL);
	CHECK(state.stats.activity_open,
	      "ambience above the floor but below the opening threshold should retain the normal speech hangover briefly");
	process_noise_segment(&state, &settings, 48000.0f, ambiguous_noise_amplitude, 48000U * 5U, 0, NULL);
	CHECK(!state.stats.activity_open,
	      "steady −45 dBFS ambience above the floor must not reset the 1.2-second hold indefinitely");
	CHECK(state.stats.gain_db < gain_after_speech - 2.0f,
	      "after the hold expires, sustained ambiguous ambience should let the fader leave its old boosted position");
	const float gain_after_close = state.stats.gain_db;
	process_noise_segment(&state, &settings, 48000.0f, ambiguous_noise_amplitude, 48000U * 2U, 0, NULL);
	CHECK(!state.stats.activity_open && fabsf(state.stats.gain_db - gain_after_close) < 0.25f,
	      "ambience below the opening threshold should not repeatedly reopen the gate or restart the fader ramp");
	return 0;
}

static int test_brief_phrase_freezes_current_fader_through_room_tone(void)
{
	struct lvb_settings settings = test_settings();
	settings.target_lufs = -10.0f;
	settings.max_boost_db = 17.0f;
	settings.max_reduction_db = 12.5f;
	settings.noise_floor_db = -50.0f;
	settings.fader_smoothness = 85.0f;
	settings.quiet_attenuation_db = 3.0f;
	settings.peak_ceiling_db = -7.1f;
	struct lvb_state state;
	lvb_state_init(&state);
	process_sermon_with_room_noise(&state, &settings, 0, 48000U / 5U, true, 0.025f, 0.0069f, NULL, NULL);
	CHECK(state.stats.activity_open, "a brief sermon phrase should open the activity gate");
	const float gain_after_phrase = state.stats.gain_db;
	CHECK(state.gain_target_db > state.gain_db + 0.5,
	      "the brief phrase should end with an unfinished upward fader ramp for the pause test to exercise");
	float pause_gain_step = 0.0f;
	process_sermon_with_room_noise(&state, &settings, 48000U / 5U, 48000U, false, 0.0f, 0.0069f, &pause_gain_step,
				       NULL);
	CHECK(state.stats.activity_open, "a one-second room-tone gap should remain inside the hold interval");
	CHECK(fabsf(state.stats.gain_db - gain_after_phrase) < 0.10f && pause_gain_step < 0.10f,
	      "a short phrase must freeze the actual fader position so room tone does not keep rising in the pause");
	CHECK(fabs(state.gain_target_db - state.gain_db) < 0.02,
	      "a brief phrase pause should freeze the target at the actual fader position, not keep its unfinished destination");
	CHECK(fabsf(state.gain_rate_db_per_second) < 1e-5f,
	      "the fader rate should stop while its current position is held between phrases");
	float reentry_gain_step = 0.0f;
	process_sermon_with_room_noise(&state, &settings, 48000U * 6U / 5U, 48000U / 5U, true, 0.025f, 0.0069f,
				       &reentry_gain_step, NULL);
	CHECK(state.stats.gain_db > gain_after_phrase,
	      "the fader should resume toward the loudness target when speech returns");
	CHECK(reentry_gain_step < 0.20f,
	      "speech re-entry should begin moving from the held position without a block-boundary jump");
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
	for (size_t block = 0; block < 180; block++) {
		fill_sine(samples, 480, 48000.0f, 310.0f, 0.0045f, block * 480, 0.23f);
		lvb_process(&state, &settings, 1, planes, 480, 48000.0f);
		const float gain_step = fabsf(state.stats.gain_db - previous_gain);
		if (gain_step > maximum_gain_step)
			maximum_gain_step = gain_step;
		previous_gain = state.stats.gain_db;
	}
	CHECK(maximum_gain_step < 0.35f,
	      "activity closure should move the dB fader gradually instead of stepping abruptly");
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
	for (size_t block = 0; block < 350; block++) {
		for (size_t i = 0; i < 480; i++) {
			left[i] = noise_value(block * 480U + i, 0.0055f);
			right[i] = noise_value(block * 480U + i + 91021U, 0.0055f);
		}
		lvb_process(&state, &settings, 2, planes, 480, 48000.0f);
		if (block >= 300) {
			for (size_t i = 0; i < 480; i++) {
				output_energy += (double)left[i] * left[i] + (double)right[i] * right[i];
				output_frames += 2;
			}
		}
	}
	const float output_rms = (float)sqrt(output_energy / (double)output_frames);
	CHECK(!state.stats.activity_open, "two-channel -50 dBFS room ambience must not open the activity gate");
	CHECK(output_rms < 0.0048f, "stereo room noise should settle near its input level after the fader hold");
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
	settings.fader_smoothness = 150.0f;
	settings.quiet_attenuation_db = -20.0f;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[16] = {NAN, INFINITY, 0.01f};
	float *planes[] = {samples, NULL};
	lvb_process(&state, &settings, 2, planes, 16, 48000.0f);
	CHECK(samples[LVB_TRUE_PEAK_LATENCY] == 0.0f,
	      "non-finite samples should be made safe rather than propagated to OBS output");
	CHECK(isfinite(state.gain), "invalid saved targets should be clamped safely");
	CHECK(state.stats.true_peak_dbtp <= 0.1f, "sanitized output peak telemetry should remain finite");
	CHECK(state.stats.fader_smoothness == LVB_FADER_SMOOTHNESS_MAX &&
		      state.stats.quiet_attenuation_db == LVB_QUIET_ATTENUATION_MIN,
	      "invalid saved fader and quiet attenuation controls are clamped to their UI ranges");
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
	settings.fader_smoothness = LVB_FADER_SMOOTHNESS_MIN;
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
	process_segment(&floor_state, &settings, 48000.0f, 900.0f, 0.00004f, 48000U / 10U, 0, NULL);
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

	struct lvb_settings fast_fader_settings = test_settings();
	fast_fader_settings.fader_smoothness = LVB_FADER_SMOOTHNESS_MIN;
	fast_fader_settings.target_lufs = -36.0f;
	fast_fader_settings.max_boost_db = 0.0f;
	fast_fader_settings.max_reduction_db = 36.0f;
	fast_fader_settings.noise_floor_db = -100.0f;
	fast_fader_settings.peak_ceiling_db = 0.0f;
	struct lvb_state fast_fader_state;
	lvb_state_init(&fast_fader_state);
	process_segment(&fast_fader_state, &fast_fader_settings, 48000.0f, 900.0f, 0.8f, 48000U / 2U, 0, NULL);
	fast_fader_settings.fader_smoothness = LVB_FADER_SMOOTHNESS_MAX;
	struct lvb_state smooth_fader_state;
	lvb_state_init(&smooth_fader_state);
	process_segment(&smooth_fader_state, &fast_fader_settings, 48000.0f, 900.0f, 0.8f, 48000U / 2U, 0, NULL);
	CHECK(fast_fader_state.stats.gain_db < smooth_fader_state.stats.gain_db - 4.0f,
	      "smoothness 0 should move the main gain faster than smoothness 100");
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
	CHECK(test_fader_curve_reduces_mixed_program_syllable_pumping() == 0,
	      "fader curve reduces mixed-program syllable pumping");
	CHECK(test_tiny_gain_steps_keep_linear_and_db_state_synchronized() == 0,
	      "small per-sample gain steps preserve linear and dB precision");
	CHECK(test_quiet_sermon_to_loud_band_reduces_smoothly_and_safely() == 0,
	      "quiet sermon to full-band transition");
	CHECK(test_speech_gaps_hold_fader_and_floor_plus_two_does_not_reopen() == 0,
	      "short speech gaps, held fader, quiet endpoint and noise-floor behavior");
	CHECK(test_ambiguous_room_noise_does_not_extend_activity_hangover() == 0,
	      "ambiguous room noise cannot hold activity open indefinitely");
	CHECK(test_optional_quiet_attenuation_fades_without_hard_gating() == 0, "optional quiet-period attenuation");
	CHECK(test_brief_phrase_freezes_current_fader_through_room_tone() == 0,
	      "brief phrase holds the current fader through room tone");
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
