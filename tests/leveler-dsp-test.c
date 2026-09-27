/*
 * Live Volume Balancer DSP tests
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "leveler-dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition, message)                                                               \
	do {                                                                                      \
		if (!(condition)) {                                                                 \
			fprintf(stderr, "FAIL: %s\n", message);                                     \
			return 1;                                                                       \
		}                                                                                     \
	} while (0)

static struct lvb_settings test_settings(void)
{
	return (struct lvb_settings){
		.target_level_db = -18.0f,
		.max_boost_db = 12.0f,
		.max_reduction_db = 18.0f,
		.attack_ms = 20.0f,
		.release_ms = 350.0f,
		.noise_floor_db = -55.0f,
		.peak_ceiling_db = -1.0f,
		.bypass = false,
	};
}

static float block_rms(const float *samples, size_t frames)
{
	double sum_squares = 0.0;
	for (size_t i = 0; i < frames; i++)
		sum_squares += (double)samples[i] * (double)samples[i];
	return (float)sqrt(sum_squares / (double)frames);
}

static int test_quiet_signal_is_boosted(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float left[1024];
	float right[1024];
	float *planes[] = {left, right};
	for (size_t i = 0; i < 1024; i++) {
		left[i] = 0.02f;
		right[i] = -0.02f;
	}

	lvb_process(&state, &settings, 2, planes, 1024, 48000.0f);
	CHECK(block_rms(left, 1024) > 0.02f, "quiet signal should receive upward gain");
	CHECK(fabsf(left[0] + right[0]) < 1e-6f, "linked gain must preserve the relationship between stereo channels");
	return 0;
}

static int test_loud_signal_is_reduced_and_ceiling_is_respected(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float loud[1024];
	float *planes[] = {loud};
	for (size_t i = 0; i < 1024; i++)
		loud[i] = (i & 1) ? 1.0f : -1.0f;

	lvb_process(&state, &settings, 1, planes, 1024, 48000.0f);
	const float ceiling = powf(10.0f, settings.peak_ceiling_db / 20.0f);
	CHECK(fabsf(loud[0]) < 1.0f, "loud signal should receive downward gain");
	for (size_t i = 0; i < 1024; i++)
		CHECK(fabsf(loud[i]) <= ceiling + 1e-6f, "output must stay below the sample-peak ceiling");
	return 0;
}

static int test_peak_ceiling_catches_a_transient(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float transient[1024] = {0.0f};
	float *planes[] = {transient};
	transient[256] = 1.0f;

	lvb_process(&state, &settings, 1, planes, 1024, 48000.0f);
	const float ceiling = powf(10.0f, settings.peak_ceiling_db / 20.0f);
	CHECK(fabsf(transient[256]) <= ceiling + 1e-6f,
	      "a transient must stay below the sample-peak ceiling even while RMS gain rises");
	return 0;
}

static int test_noise_floor_does_not_boost(void)
{
	struct lvb_settings settings = test_settings();
	struct lvb_state state;
	lvb_state_init(&state);
	float quiet[1024];
	float noise[1024];
	float *quiet_planes[] = {quiet};
	float *planes[] = {noise};
	for (size_t i = 0; i < 1024; i++)
		quiet[i] = 0.02f;
	for (size_t i = 0; i < 1024; i++)
		noise[i] = (i & 1) ? 0.0001f : -0.0001f;

	lvb_process(&state, &settings, 1, quiet_planes, 1024, 48000.0f);
	const float gain_before_floor = state.gain;
	lvb_process(&state, &settings, 1, planes, 1024, 48000.0f);
	CHECK(fabsf(noise[0]) <= 0.0001f, "signal under the noise floor must not be boosted");
	CHECK(state.gain < gain_before_floor && state.gain > 1.0f,
	      "gain state should ease back toward unity below the noise floor");
	for (size_t block = 0; block < 12; block++)
		lvb_process(&state, &settings, 1, planes, 1024, 48000.0f);
	CHECK(state.gain <= 1.001f, "sustained noise should return to unity gain");
	return 0;
}

static int test_null_planes_and_non_finite_samples_are_safe(void)
{
	struct lvb_settings settings = test_settings();
	settings.target_level_db = NAN;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[] = {NAN, INFINITY, 0.01f};
	float *planes[] = {samples, NULL};

	lvb_process(&state, &settings, 2, planes, 3, 48000.0f);
	CHECK(isnan(samples[0]), "NaN input should pass through untouched");
	CHECK(isinf(samples[1]), "infinite input should pass through untouched");
	CHECK(isfinite(samples[2]), "finite samples should still be processed");
	CHECK(isfinite(state.gain), "an invalid target level should be clamped safely");
	return 0;
}

static int test_bypass_preserves_audio(void)
{
	struct lvb_settings settings = test_settings();
	settings.bypass = true;
	struct lvb_state state;
	lvb_state_init(&state);
	float samples[] = {-0.5f, 0.0f, 0.25f};
	float original[] = {-0.5f, 0.0f, 0.25f};
	float *planes[] = {samples};

	lvb_process(&state, &settings, 1, planes, 3, 48000.0f);
	for (size_t i = 0; i < 3; i++)
		CHECK(samples[i] == original[i], "bypass must leave samples unchanged");
	CHECK(state.gain == 1.0f, "bypass should reset gain state");
	return 0;
}

int main(void)
{
	CHECK(test_quiet_signal_is_boosted() == 0, "quiet signal test");
	CHECK(test_loud_signal_is_reduced_and_ceiling_is_respected() == 0, "loud signal test");
	CHECK(test_peak_ceiling_catches_a_transient() == 0, "transient ceiling test");
	CHECK(test_noise_floor_does_not_boost() == 0, "noise floor test");
	CHECK(test_null_planes_and_non_finite_samples_are_safe() == 0, "null/non-finite test");
	CHECK(test_bypass_preserves_audio() == 0, "bypass test");
	puts("All leveler DSP tests passed.");
	return 0;
}
