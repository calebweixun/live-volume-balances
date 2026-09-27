/*
 * Live Volume Balancer
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

struct lvb_settings {
	float target_level_db;
	float max_boost_db;
	float max_reduction_db;
	float attack_ms;
	float release_ms;
	float noise_floor_db;
	float peak_ceiling_db;
	bool bypass;
};

struct lvb_state {
	float gain;
};

void lvb_state_init(struct lvb_state *state);

/*
 * Process planar float audio. One linked RMS detector and one gain value are
 * used for all channels, preserving the stereo image. The function does not
 * allocate memory and never changes non-finite samples.
 */
void lvb_process(struct lvb_state *state, const struct lvb_settings *settings, size_t channels, float *const *audio,
		 size_t frames, float sample_rate);
