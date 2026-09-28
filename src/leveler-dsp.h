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
#define LVB_TARGET_LUFS_MIN (-36.0f)
#define LVB_TARGET_LUFS_MAX 0.0f
#define LVB_METER_BUCKETS 300
#define LVB_MOMENTARY_BUCKETS 40
#define LVB_TRUE_PEAK_TAPS 12
#define LVB_TRUE_PEAK_LATENCY 6
#define LVB_FADER_SMOOTHNESS_MIN 0.0f
#define LVB_FADER_SMOOTHNESS_MAX 100.0f
#define LVB_FADER_SMOOTHNESS_DEFAULT 85.0f
#define LVB_QUIET_ATTENUATION_MIN 0.0f
#define LVB_QUIET_ATTENUATION_MAX 12.0f

struct lvb_settings {
	float target_lufs;
	float max_boost_db;
	float max_reduction_db;
	float noise_floor_db;
	float fader_smoothness;
	float quiet_attenuation_db;
	float peak_ceiling_db;
	bool bypass;
};

struct lvb_stats {
	float input_fast_rms_dbfs;
	float output_fast_rms_dbfs;
	float input_momentary_lufs;
	float input_short_term_lufs;
	float output_momentary_lufs;
	float output_short_term_lufs;
	float true_peak_dbtp;
	float peak_hold_dbtp;
	float gain_db;
	float target_lufs;
	float peak_ceiling_dbtp;
	float max_boost_db;
	float max_reduction_db;
	float noise_floor_dbfs;
	float fader_smoothness;
	float quiet_attenuation_db;
	float sample_rate_hz;
	bool activity_open;
	bool bypass;
};

struct lvb_biquad_state {
	double z1;
	double z2;
};

struct lvb_meter_state {
	double energy[LVB_METER_BUCKETS];
	double momentary_energy[LVB_MOMENTARY_BUCKETS];
	double short_term_sum;
	double momentary_sum;
	double current_bucket_sum;
	uint32_t index;
	uint32_t count;
	uint32_t momentary_index;
	uint32_t momentary_count;
	uint32_t frames_in_bucket;
};

/* Fixed-size state: processing audio never allocates or waits on a lock. */
struct lvb_state {
	double gain;
	double gain_db;
	double gain_target_db;
	float gain_rate_db_per_second;
	float peak_guard_gain;
	float sample_rate;
	float activity_energy_coefficient;
	float fast_meter_attack_coefficient;
	float fast_meter_release_coefficient;
	float peak_envelope_attack_coefficient;
	float peak_envelope_release_coefficient;
	float input_peak_envelope;
	float activity_reference_dbfs;
	float activity_hold_seconds;
	float quiet_transition_seconds;
	float peak_hold_dbtp;
	float peak_hold_seconds;
	double activity_energy;
	double input_fast_meter_energy;
	double output_fast_meter_energy;
	uint32_t bucket_frames;
	uint32_t true_peak_history_index;
	uint32_t output_peak_history_index;
	uint32_t true_peak_delay_index;
	uint32_t true_peak_delay_frames;
	bool activity_reference_valid;
	bool activity_open;
	float true_peak_history[LVB_MAX_CHANNELS][LVB_TRUE_PEAK_TAPS];
	float output_peak_history[LVB_MAX_CHANNELS][LVB_TRUE_PEAK_TAPS];
	float true_peak_delay[LVB_MAX_CHANNELS][LVB_TRUE_PEAK_LATENCY];
	struct lvb_meter_state input_meter;
	struct lvb_meter_state output_meter;
	struct lvb_biquad_state input_k_shelf[LVB_MAX_CHANNELS];
	struct lvb_biquad_state input_k_highpass[LVB_MAX_CHANNELS];
	struct lvb_biquad_state output_k_shelf[LVB_MAX_CHANNELS];
	struct lvb_biquad_state output_k_highpass[LVB_MAX_CHANNELS];
	double shelf_b[3];
	double shelf_a[2];
	double highpass_b[3];
	double highpass_a[2];
	struct lvb_stats stats;
};

#ifdef __cplusplus
extern "C" {
#endif

void lvb_state_init(struct lvb_state *state);
void lvb_get_stats(const struct lvb_state *state, struct lvb_stats *stats);
float lvb_output_latency_ms(float sample_rate);

/*
 * Process planar float audio with linked-channel, rolling K-weighted loudness
 * estimates and smoothed gain riding. The 400 ms and 3 s readings are rolling
 * estimates, not integrated-program normalization or a BS.1770 conformance
 * claim. A 4x FIR peak guard uses a fixed six-sample delay. No memory is
 * allocated by this function.
 */
void lvb_process(struct lvb_state *state, const struct lvb_settings *settings, size_t channels, float *const *audio,
		 size_t frames, float sample_rate);

#ifdef __cplusplus
}
#endif
