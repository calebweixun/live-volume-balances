/*
 * Live Volume Balancer
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LVB_MAX_CHANNELS 8
#define LVB_METER_BUCKETS 300
#define LVB_TRUE_PEAK_TAPS 12
#define LVB_TRUE_PEAK_LATENCY 6
#define LVB_MODE_COUNT 5

enum lvb_mode {
	LVB_MODE_CUSTOM = 0,
	LVB_MODE_WORSHIP = 1,
	LVB_MODE_SERMON = 2,
	LVB_MODE_AUTO_ASSIST = 3,
	LVB_MODE_WORSHIP_ACOUSTIC = 4,
};

struct lvb_settings {
	float target_lufs;
	float max_boost_db;
	float max_reduction_db;
	float attack_ms;
	float release_ms;
	float noise_floor_db;
	float peak_ceiling_db;
	float mode_trim_db[LVB_MODE_COUNT]; /* post-rider makeup, before peak protection */
	enum lvb_mode mode;
	bool bypass;
};

struct lvb_stats {
	float momentary_lufs;
	float short_term_lufs;
	float true_peak_dbtp;
	float gain_reduction_db;
	float gain_db;
	float voice_activity;
	float calibration_progress;
	float calibration_measured_lufs;
	float calibration_suggestion_db;
	enum lvb_mode mode;
	enum lvb_mode calibration_mode;
	bool calibration_active;
	bool calibration_ready;
	bool voice_active;
};

/* All DSP state is fixed-size so the audio callback never allocates memory. */
struct lvb_biquad_state {
	double z1;
	double z2;
};

struct lvb_filter_state {
	double x1;
	double y1;
};

struct lvb_state {
	float gain;
	float peak_guard_gain;
	float sample_rate;
	float voice_envelope_coefficient;
	double voice_highpass_alpha;
	double voice_lowpass_alpha;
	double meter_energy[LVB_METER_BUCKETS];
	double momentary_energy[40];
	double short_term_sum;
	double momentary_sum;
	double current_bucket_sum;
	double calibration_energy_sum;
	uint64_t calibration_sample_count;
	uint64_t calibration_frame_count;
	uint32_t bucket_frames;
	uint32_t frames_in_bucket;
	uint32_t meter_index;
	uint32_t meter_count;
	uint32_t momentary_index;
	uint32_t momentary_count;
	uint32_t calibration_mode;
	double calibration_elapsed;
	float calibration_target_lufs;
	float calibration_base_trim_db;
	float calibration_measured_lufs;
	bool calibration_active;
	bool calibration_ready;
	float calibration_suggestion_db;
	float meter_true_peak;
	float peak_history[LVB_MAX_CHANNELS][2];
	bool peak_history_valid;
	float true_peak_history[LVB_MAX_CHANNELS][LVB_TRUE_PEAK_TAPS];
	float output_peak_history[LVB_MAX_CHANNELS][LVB_TRUE_PEAK_TAPS];
	float true_peak_delay[LVB_MAX_CHANNELS][LVB_TRUE_PEAK_LATENCY];
	uint32_t true_peak_history_index;
	uint32_t output_peak_history_index;
	uint32_t true_peak_delay_index;
	uint32_t true_peak_delay_frames;
	float voice_envelope;
	float voice_hold_seconds;
	bool voice_active;
	struct lvb_biquad_state k_shelf[LVB_MAX_CHANNELS];
	struct lvb_biquad_state k_highpass[LVB_MAX_CHANNELS];
	struct lvb_biquad_state calibration_k_shelf[LVB_MAX_CHANNELS];
	struct lvb_biquad_state calibration_k_highpass[LVB_MAX_CHANNELS];
	struct lvb_filter_state voice_highpass[LVB_MAX_CHANNELS];
	struct lvb_filter_state voice_lowpass[LVB_MAX_CHANNELS];
	double shelf_b[3];
	double shelf_a[2];
	double highpass_b[3];
	double highpass_a[2];
	struct lvb_stats stats;
};

void lvb_state_init(struct lvb_state *state);
void lvb_calibration_start(struct lvb_state *state, const struct lvb_settings *settings);
void lvb_calibration_reset(struct lvb_state *state);
void lvb_get_stats(const struct lvb_state *state, struct lvb_stats *stats);

/*
 * Process planar float audio using linked-channel K-weighted loudness sensing,
 * a 400 ms detector, a smoothed gain envelope, and a lookahead guard using
 * the 4x FIR coefficients from ITU-R BS.1770-5 Annex 2. LUFS values are
 * BS.1770-style estimates, not a conformance implementation. The function
 * does not allocate memory.
 */
void lvb_process(struct lvb_state *state, const struct lvb_settings *settings, size_t channels, float *const *audio,
		 size_t frames, float sample_rate);
