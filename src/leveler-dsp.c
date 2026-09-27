/*
 * Live Volume Balancer
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "leveler-dsp.h"

#include <math.h>
#include <stdint.h>

static float clampf(float value, float minimum, float maximum)
{
	if (!isfinite(value))
		return minimum;
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

static float db_to_linear(float db)
{
	return powf(10.0f, db * 0.05f);
}

static float linear_to_db(float linear)
{
	return 20.0f * log10f(linear);
}

void lvb_state_init(struct lvb_state *state)
{
	if (state)
		state->gain = 1.0f;
}

void lvb_process(struct lvb_state *state, const struct lvb_settings *settings, size_t channels, float *const *audio,
		 size_t frames, float sample_rate)
{
	if (!state || !settings || !audio || channels == 0 || frames == 0 || !isfinite(sample_rate) ||
	    sample_rate <= 0.0f)
		return;

	if (settings->bypass) {
		state->gain = 1.0f;
		return;
	}

	double sum_squares = 0.0;
	uint64_t sample_count = 0;
	float input_peak = 0.0f;

	for (size_t channel = 0; channel < channels; channel++) {
		const float *samples = audio[channel];
		if (!samples)
			continue;

		for (size_t frame = 0; frame < frames; frame++) {
			const float sample = samples[frame];
			if (!isfinite(sample))
				continue;

			sum_squares += (double)sample * (double)sample;
			sample_count++;
			const float magnitude = fabsf(sample);
			if (magnitude > input_peak)
				input_peak = magnitude;
		}
	}

	if (sample_count == 0)
		return;

	const float rms = (float)sqrt(sum_squares / (double)sample_count);
	const float level_db = rms > 0.0f ? linear_to_db(rms) : -120.0f;
	const float target_level_db = clampf(settings->target_level_db, -30.0f, -9.0f);
	const float floor_db = clampf(settings->noise_floor_db, -100.0f, 0.0f);
	const bool below_floor = level_db < floor_db;
	float target_gain = 1.0f;

	if (!below_floor) {
		const float requested_gain_db = target_level_db - level_db;
		const float minimum_gain_db = -clampf(settings->max_reduction_db, 0.0f, 48.0f);
		const float maximum_gain_db = clampf(settings->max_boost_db, 0.0f, 24.0f);
		target_gain = db_to_linear(clampf(requested_gain_db, minimum_gain_db, maximum_gain_db));
	}

	const float attack_ms = clampf(settings->attack_ms, 1.0f, 1000.0f);
	const float release_ms = clampf(settings->release_ms, 1.0f, 5000.0f);
	const float time_ms = target_gain < state->gain ? attack_ms : release_ms;
	const float elapsed_seconds = (float)frames / sample_rate;
	const float smoothing = expf(-elapsed_seconds / (time_ms * 0.001f));
	state->gain = target_gain + smoothing * (state->gain - target_gain);

	float applied_gain = state->gain;
	if (below_floor && applied_gain > 1.0f)
		applied_gain = 1.0f;
	const float ceiling_db = clampf(settings->peak_ceiling_db, -24.0f, 0.0f);
	const float peak_ceiling = db_to_linear(ceiling_db);
	if (input_peak > 0.0f) {
		const float peak_safe_gain = peak_ceiling / input_peak;
		if (applied_gain > peak_safe_gain)
			applied_gain = peak_safe_gain;
	}

	for (size_t channel = 0; channel < channels; channel++) {
		float *samples = audio[channel];
		if (!samples)
			continue;

		for (size_t frame = 0; frame < frames; frame++) {
			const float sample = samples[frame];
			if (!isfinite(sample))
				continue;

			const float output = sample * applied_gain;
			samples[frame] = clampf(output, -peak_ceiling, peak_ceiling);
		}
	}
}
