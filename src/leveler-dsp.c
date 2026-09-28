/*
 * Live Volume Balancer
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "leveler-dsp.h"

#include <math.h>
#include <string.h>

#define LVB_PI 3.14159265358979323846
#define LVB_LUFS_OFFSET (-0.691)
#define LVB_BUCKET_SECONDS 0.01
#define LVB_FAST_METER_ATTACK_SECONDS 0.030
#define LVB_FAST_METER_RELEASE_SECONDS 0.150
#define LVB_ACTIVITY_HOLD_SECONDS 1.20f
#define LVB_ACTIVITY_RELATIVE_DB 12.0f
#define LVB_ACTIVITY_OPEN_MARGIN_DB 6.0f
#define LVB_ACTIVITY_REFERENCE_MARGIN_DB 3.0f
#define LVB_PEAK_RIDER_HEADROOM_DB 0.5f
#define LVB_NEPER_PER_DB 0.1151292546497022842

/*
 * ITU-R BS.1770-5 Annex 2, order-48 four-phase interpolating FIR.
 * Rows are stored newest-to-oldest in the per-phase tap table below.
 */
static const float true_peak_coefficients[4][LVB_TRUE_PEAK_TAPS] = {
	{-0.00830078125f, 0.014892578125f, -0.026611328125f, 0.047607421875f, -0.102294921875f, 0.97216796875f,
	 0.1373291015625f, -0.0594482421875f, 0.033203125f, -0.0196533203125f, 0.010986328125f, 0.001708984375f},
	{-0.0189208984375f, 0.0330810546875f, -0.0582275390625f, 0.1015625f, -0.2003173828125f, 0.77978515625f,
	 0.465087890625f, -0.16650390625f, 0.089111328125f, -0.0517578125f, 0.029296875f, -0.0291748046875f},
	{-0.0291748046875f, 0.029296875f, -0.0517578125f, 0.089111328125f, -0.16650390625f, 0.465087890625f,
	 0.77978515625f, -0.2003173828125f, 0.1015625f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f},
	{0.001708984375f, 0.010986328125f, -0.0196533203125f, 0.033203125f, -0.0594482421875f, 0.1373291015625f,
	 0.97216796875f, -0.102294921875f, 0.047607421875f, -0.026611328125f, 0.014892578125f, -0.00830078125f},
};

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
	return powf(10.0f, db / 20.0f);
}

static double linear_to_db(double value)
{
	return value > 0.0 ? 20.0 * log10(value) : -120.0;
}

static float energy_to_lufs(double energy)
{
	if (!isfinite(energy) || energy <= 1e-12)
		return -120.0f;
	return (float)(LVB_LUFS_OFFSET + 10.0 * log10(energy));
}

static float energy_to_dbfs(double energy)
{
	if (!isfinite(energy) || energy <= 1e-12)
		return -120.0f;
	return (float)(10.0 * log10(energy));
}

static void configure_k_weighting(struct lvb_state *state, float sample_rate)
{
	const double frequency = sample_rate > 0.0f ? sample_rate : 48000.0;
	const double shelf_k = tan(LVB_PI * 1681.974450955533 / frequency);
	const double shelf_q = 0.7071752369554196;
	const double shelf_vh = pow(10.0, 3.999843853973347 / 20.0);
	const double shelf_vb = pow(shelf_vh, 0.4996667741545416);
	const double shelf_k2 = shelf_k * shelf_k;
	const double shelf_a0 = 1.0 + shelf_k / shelf_q + shelf_k2;
	state->shelf_b[0] = (shelf_vh + shelf_vb * shelf_k / shelf_q + shelf_k2) / shelf_a0;
	state->shelf_b[1] = 2.0 * (shelf_k2 - shelf_vh) / shelf_a0;
	state->shelf_b[2] = (shelf_vh - shelf_vb * shelf_k / shelf_q + shelf_k2) / shelf_a0;
	state->shelf_a[0] = 2.0 * (shelf_k2 - 1.0) / shelf_a0;
	state->shelf_a[1] = (1.0 - shelf_k / shelf_q + shelf_k2) / shelf_a0;

	const double highpass_k = tan(LVB_PI * 38.13547087602444 / frequency);
	const double highpass_q = 0.5003270373238773;
	const double highpass_k2 = highpass_k * highpass_k;
	const double highpass_a0 = 1.0 + highpass_k / highpass_q + highpass_k2;
	state->highpass_b[0] = 1.0 / highpass_a0;
	state->highpass_b[1] = -2.0 / highpass_a0;
	state->highpass_b[2] = 1.0 / highpass_a0;
	state->highpass_a[0] = 2.0 * (highpass_k2 - 1.0) / highpass_a0;
	state->highpass_a[1] = (1.0 - highpass_k / highpass_q + highpass_k2) / highpass_a0;
	memset(state->input_k_shelf, 0, sizeof(state->input_k_shelf));
	memset(state->input_k_highpass, 0, sizeof(state->input_k_highpass));
	memset(state->output_k_shelf, 0, sizeof(state->output_k_shelf));
	memset(state->output_k_highpass, 0, sizeof(state->output_k_highpass));
	memset(&state->input_meter, 0, sizeof(state->input_meter));
	memset(&state->output_meter, 0, sizeof(state->output_meter));
	state->sample_rate = sample_rate;
	state->activity_energy_coefficient = (float)(1.0 - exp(-1.0 / (0.005 * frequency)));
	state->fast_meter_attack_coefficient = (float)exp(-1.0 / (LVB_FAST_METER_ATTACK_SECONDS * frequency));
	state->fast_meter_release_coefficient = (float)exp(-1.0 / (LVB_FAST_METER_RELEASE_SECONDS * frequency));
	state->peak_envelope_attack_coefficient = (float)exp(-1.0 / (0.005 * frequency));
	state->peak_envelope_release_coefficient = (float)exp(-1.0 / (1.5 * frequency));
	state->input_fast_meter_energy = 0.0;
	state->output_fast_meter_energy = 0.0;
	state->bucket_frames = (uint32_t)fmax(1.0, floor(frequency * LVB_BUCKET_SECONDS + 0.5));
}

static double update_fast_meter_energy(double previous, double input, float attack_coefficient,
				       float release_coefficient)
{
	const float coefficient = input > previous ? attack_coefficient : release_coefficient;
	return input + coefficient * (previous - input);
}

static double process_biquad(double input, const double b[3], const double a[2], struct lvb_biquad_state *state)
{
	const double output = b[0] * input + state->z1;
	state->z1 = b[1] * input - a[0] * output + state->z2;
	state->z2 = b[2] * input - a[1] * output;
	return isfinite(output) ? output : 0.0;
}

static double channel_weight(size_t channel, size_t channels)
{
	/* ITU-style 5.1 / 7.1 weighting where the OBS plane layout is known. */
	if ((channels == 6 || channels == 8) && channel == 3)
		return 0.0; /* LFE */
	if ((channels == 6 && channel >= 4) || (channels == 8 && channel >= 4))
		return 1.41;
	return 1.0;
}

static void meter_add_frame(struct lvb_meter_state *meter, double energy, uint32_t bucket_frames)
{
	meter->current_bucket_sum += energy;
	meter->frames_in_bucket++;
	if (meter->frames_in_bucket < bucket_frames)
		return;

	const double bucket_energy = meter->current_bucket_sum / (double)meter->frames_in_bucket;
	if (meter->count == LVB_METER_BUCKETS)
		meter->short_term_sum -= meter->energy[meter->index];
	else
		meter->count++;
	meter->energy[meter->index] = bucket_energy;
	meter->short_term_sum += bucket_energy;
	meter->index = (meter->index + 1U) % LVB_METER_BUCKETS;

	if (meter->momentary_count == LVB_MOMENTARY_BUCKETS)
		meter->momentary_sum -= meter->momentary_energy[meter->momentary_index];
	else
		meter->momentary_count++;
	meter->momentary_energy[meter->momentary_index] = bucket_energy;
	meter->momentary_sum += bucket_energy;
	meter->momentary_index = (meter->momentary_index + 1U) % LVB_MOMENTARY_BUCKETS;
	meter->current_bucket_sum = 0.0;
	meter->frames_in_bucket = 0;
}

static float meter_momentary_lufs(const struct lvb_meter_state *meter)
{
	const double average = meter->momentary_count ? meter->momentary_sum / meter->momentary_count : 0.0;
	return energy_to_lufs(average);
}

static float meter_short_term_lufs(const struct lvb_meter_state *meter)
{
	const double average = meter->count ? meter->short_term_sum / meter->count : 0.0;
	return energy_to_lufs(average);
}

static float update_activity(struct lvb_state *state, float level_dbfs, float floor_db, float elapsed_seconds)
{
	const float minimum_open_level = floor_db + LVB_ACTIVITY_OPEN_MARGIN_DB;
	float open_threshold = minimum_open_level;
	if (state->activity_reference_valid)
		open_threshold = fmaxf(open_threshold, state->activity_reference_dbfs - LVB_ACTIVITY_RELATIVE_DB);
	const bool absolute_silence = level_dbfs <= floor_db;
	const bool candidate = !absolute_silence && level_dbfs >= open_threshold;

	/*
	 * Let the relative reference move down quickly when a band gives way to
	 * quieter speech. Only clear activity above the floor margin can move it, so
	 * near-floor room noise during a pause cannot hold the activity gate open.
	 */
	if (level_dbfs > floor_db + LVB_ACTIVITY_REFERENCE_MARGIN_DB) {
		if (!state->activity_reference_valid) {
			state->activity_reference_dbfs = level_dbfs;
			state->activity_reference_valid = true;
		} else {
			const float time_constant = level_dbfs > state->activity_reference_dbfs ? 0.20f : 0.45f;
			const float coefficient = 1.0f - expf(-elapsed_seconds / time_constant);
			state->activity_reference_dbfs += coefficient * (level_dbfs - state->activity_reference_dbfs);
		}
	}

	if (candidate) {
		state->activity_open = true;
		state->activity_hold_seconds = LVB_ACTIVITY_HOLD_SECONDS;
	} else if (state->activity_hold_seconds > elapsed_seconds) {
		state->activity_hold_seconds -= elapsed_seconds;
	} else {
		state->activity_hold_seconds = 0.0f;
		state->activity_open = false;
	}

	/* Only a fresh candidate may reset the hangover or update the loudness target. */
	return candidate ? 1.0f : 0.0f;
}

static float soft_knee_reduction(float above_target_db, float ratio, float knee_db)
{
	if (above_target_db <= -0.5f * knee_db)
		return 0.0f;
	if (above_target_db >= 0.5f * knee_db)
		return above_target_db * (1.0f - 1.0f / ratio);

	const float shifted = above_target_db + 0.5f * knee_db;
	return (1.0f - 1.0f / ratio) * shifted * shifted / (2.0f * knee_db);
}

static float true_peak_fir_values(float history[LVB_MAX_CHANNELS][LVB_TRUE_PEAK_TAPS], uint32_t *index,
				  const float values[LVB_MAX_CHANNELS], size_t channels)
{
	float maximum = 0.0f;
	for (size_t channel = 0; channel < channels && channel < LVB_MAX_CHANNELS; channel++) {
		const float input = isfinite(values[channel]) ? values[channel] : 0.0f;
		history[channel][*index] = input;
		for (unsigned phase = 0; phase < 4; phase++) {
			double estimate = 0.0;
			for (unsigned tap = 0; tap < LVB_TRUE_PEAK_TAPS; tap++) {
				const uint32_t history_index = (*index + LVB_TRUE_PEAK_TAPS - tap) % LVB_TRUE_PEAK_TAPS;
				estimate +=
					(double)true_peak_coefficients[phase][tap] * history[channel][history_index];
			}
			const float magnitude = (float)fabs(estimate);
			if (isfinite(magnitude) && magnitude > maximum)
				maximum = magnitude;
		}
	}
	*index = (*index + 1U) % LVB_TRUE_PEAK_TAPS;
	return maximum;
}

void lvb_state_init(struct lvb_state *state)
{
	if (!state)
		return;
	memset(state, 0, sizeof(*state));
	state->gain = 1.0;
	state->gain_db = 0.0f;
	state->gain_target_db = 0.0f;
	state->gain_rate_db_per_second = 0.0f;
	state->peak_guard_gain = 1.0f;
	state->activity_reference_dbfs = -120.0f;
	state->stats.input_momentary_lufs = -120.0f;
	state->stats.input_short_term_lufs = -120.0f;
	state->stats.input_fast_rms_dbfs = -120.0f;
	state->stats.output_fast_rms_dbfs = -120.0f;
	state->stats.output_momentary_lufs = -120.0f;
	state->stats.output_short_term_lufs = -120.0f;
	state->stats.true_peak_dbtp = -120.0f;
	state->stats.peak_hold_dbtp = -120.0f;
	state->peak_hold_dbtp = -120.0f;
}

float lvb_output_latency_ms(float sample_rate)
{
	if (!isfinite(sample_rate) || sample_rate <= 0.0f)
		return 0.0f;
	return (float)LVB_TRUE_PEAK_LATENCY * 1000.0f / sample_rate;
}

void lvb_get_stats(const struct lvb_state *state, struct lvb_stats *stats)
{
	if (state && stats)
		*stats = state->stats;
}

void lvb_process(struct lvb_state *state, const struct lvb_settings *settings, size_t channels, float *const *audio,
		 size_t frames, float sample_rate)
{
	if (!state || !settings || !audio || channels == 0 || frames == 0 || !isfinite(sample_rate) ||
	    sample_rate <= 0.0f)
		return;
	if (channels > LVB_MAX_CHANNELS)
		channels = LVB_MAX_CHANNELS;
	if (fabsf(state->sample_rate - sample_rate) > 0.5f)
		configure_k_weighting(state, sample_rate);

	const float target_lufs = clampf(settings->target_lufs, LVB_TARGET_LUFS_MIN, LVB_TARGET_LUFS_MAX);
	const float max_boost_db = clampf(settings->max_boost_db, 0.0f, 36.0f);
	const float max_reduction_db = clampf(settings->max_reduction_db, 0.0f, 36.0f);
	const float noise_floor_db = clampf(settings->noise_floor_db, -100.0f, -6.0f);
	const float fader_smoothness =
		clampf(settings->fader_smoothness, LVB_FADER_SMOOTHNESS_MIN, LVB_FADER_SMOOTHNESS_MAX);
	const float quiet_attenuation_db =
		clampf(settings->quiet_attenuation_db, LVB_QUIET_ATTENUATION_MIN, LVB_QUIET_ATTENUATION_MAX);
	const float peak_ceiling_db = clampf(settings->peak_ceiling_db, -24.0f, 0.0f);
	const float peak_ceiling = db_to_linear(peak_ceiling_db);

	/* Sense the current block without modifying it. */
	for (size_t frame = 0; frame < frames; frame++) {
		double frame_energy = 0.0;
		double frame_activity_energy = 0.0;
		size_t activity_channels = 0;
		for (size_t channel = 0; channel < channels; channel++) {
			if (!audio[channel])
				continue;
			const double weight = channel_weight(channel, channels);
			const double input = isfinite(audio[channel][frame]) ? audio[channel][frame] : 0.0;
			const double shelf =
				process_biquad(input, state->shelf_b, state->shelf_a, &state->input_k_shelf[channel]);
			const double filtered = process_biquad(shelf, state->highpass_b, state->highpass_a,
							       &state->input_k_highpass[channel]);
			frame_energy += weight * filtered * filtered;
			if (weight > 0.0) {
				frame_activity_energy += input * input;
				activity_channels++;
			}
		}
		if (activity_channels > 0)
			frame_activity_energy /= (double)activity_channels;
		state->activity_energy +=
			state->activity_energy_coefficient * (frame_activity_energy - state->activity_energy);
		state->input_fast_meter_energy = update_fast_meter_energy(state->input_fast_meter_energy,
									  frame_activity_energy,
									  state->fast_meter_attack_coefficient,
									  state->fast_meter_release_coefficient);
		meter_add_frame(&state->input_meter, frame_energy, state->bucket_frames);
	}

	const float input_fast_rms_dbfs = energy_to_dbfs(state->input_fast_meter_energy);
	const float input_momentary_lufs = meter_momentary_lufs(&state->input_meter);
	const float input_short_term_lufs = meter_short_term_lufs(&state->input_meter);
	const float block_seconds = (float)((double)frames / sample_rate);
	const float fast_activity_dbfs = energy_to_dbfs(state->activity_energy);
	const float activity_allows_gain = update_activity(state, fast_activity_dbfs, noise_floor_db, block_seconds);
	if (!settings->bypass && activity_allows_gain <= 0.5f && state->activity_open) {
		/* Freeze the actual fader position during word gaps, not just its previous destination. */
		state->gain_target_db = state->gain_db;
		state->gain_rate_db_per_second = 0.0f;
	}
	/* A persistent low active level after a louder mix can use faster recovery; closed pauses reset the timer. */
	const bool quiet_transition_candidate = state->activity_open && input_momentary_lufs > -100.0f &&
						input_short_term_lufs > -100.0f && activity_allows_gain > 0.5f &&
						input_short_term_lufs - input_momentary_lufs > 8.0f;
	if (quiet_transition_candidate)
		state->quiet_transition_seconds = fminf(1.0f, state->quiet_transition_seconds + block_seconds);
	else
		state->quiet_transition_seconds = 0.0f;
	const bool recovery_assist_active = state->quiet_transition_seconds >= 0.80f;
	/* Keep the rolling detector engaged throughout: short phrases should not briefly retarget the fader. */
	const float long_term_detector_weight = (fader_smoothness / 100.0f) * (recovery_assist_active ? 0.25f : 1.0f);
	const float detector_lufs =
		input_momentary_lufs + long_term_detector_weight * (input_short_term_lufs - input_momentary_lufs);
	float requested_gain_db = (float)state->gain_target_db;
	if (activity_allows_gain > 0.5f) {
		const float difference_db = target_lufs - detector_lufs;
		if (difference_db >= 0.0f)
			requested_gain_db = fminf(difference_db, max_boost_db);
		else
			requested_gain_db = -soft_knee_reduction(-difference_db, 10.0f, 1.5f);
		requested_gain_db = clampf(requested_gain_db, -max_reduction_db, max_boost_db);
	} else if (!state->activity_open) {
		/* Long quiet periods settle at the configured attenuation on this fader. */
		requested_gain_db = -quiet_attenuation_db;
	}
	const float deadband_db = 0.20f + 0.008f * fader_smoothness;
	if (activity_allows_gain > 0.5f && fabs((double)requested_gain_db - state->gain_target_db) <= deadband_db)
		requested_gain_db = (float)state->gain_target_db;
	/*
	 * Let the slow rider take over sustained peak-ceiling work. The instantaneous
	 * FIR guard below still catches a transient before this envelope and fader can
	 * react. The envelope is updated sample-wise, while this target is refreshed
	 * per OBS block to avoid a logarithm in the audio hot loop.
	 */
	const float sustained_input_peak_db = linear_to_db(state->input_peak_envelope);
	const float peak_aware_target_db =
		fminf(requested_gain_db, peak_ceiling_db - sustained_input_peak_db - LVB_PEAK_RIDER_HEADROOM_DB);
	const float smoothness_ratio = fader_smoothness / 100.0f;
	const float fader_response_seconds = 0.050f + 0.450f * smoothness_ratio;
	const float smoothing = expf(-1.0f / (fader_response_seconds * sample_rate));
	/* Peak attack remains immediate; a longer release prevents speech peaks from pumping between words. */
	const float guard_release = expf(-1.0f / (0.300f * sample_rate));
	const float rate_complement = 1.0f - smoothness_ratio;
	const float downward_rate_limit = 3.0f + 80.0f * powf(rate_complement, 2.4f);
	const float upward_rate_limit =
		(3.5f + 44.5f * rate_complement * rate_complement) * (recovery_assist_active ? 1.8f : 1.0f);
	const float downward_acceleration = 36.0f + 1164.0f * rate_complement * rate_complement;
	const float upward_acceleration =
		(18.0f + 282.0f * rate_complement * rate_complement) * (recovery_assist_active ? 1.8f : 1.0f);
	const float target_persistence_seconds =
		(0.15f + 1.50f * smoothness_ratio) * (recovery_assist_active ? 0.40f : 1.0f);
	const float target_persistence_coefficient = expf(-1.0f / (target_persistence_seconds * sample_rate));

	float maximum_output_peak = 0.0f;
	double last_applied_gain = 1.0;
	for (size_t frame = 0; frame < frames; frame++) {
		float input_values[LVB_MAX_CHANNELS] = {0};
		for (size_t channel = 0; channel < channels; channel++) {
			if (audio[channel])
				input_values[channel] = isfinite(audio[channel][frame]) ? audio[channel][frame] : 0.0f;
		}
		const float estimated_peak = true_peak_fir_values(
			state->true_peak_history, &state->true_peak_history_index, input_values, channels);
		const float peak_envelope_coefficient = estimated_peak > state->input_peak_envelope
								? state->peak_envelope_attack_coefficient
								: state->peak_envelope_release_coefficient;
		state->input_peak_envelope =
			estimated_peak + peak_envelope_coefficient * (state->input_peak_envelope - estimated_peak);

		if (settings->bypass) {
			state->gain = 1.0;
			state->gain_db = 0.0f;
			state->gain_target_db = 0.0f;
			state->gain_rate_db_per_second = 0.0f;
			state->peak_guard_gain = 1.0f;
		} else {
			if (activity_allows_gain > 0.5f && fader_smoothness > 0.0f) {
				if (peak_aware_target_db < state->gain_target_db)
					state->gain_target_db = peak_aware_target_db;
				else
					state->gain_target_db = peak_aware_target_db +
								target_persistence_coefficient *
									(state->gain_target_db - peak_aware_target_db);
			} else if (activity_allows_gain > 0.5f || !state->activity_open) {
				state->gain_target_db = peak_aware_target_db;
			}
			if (fader_smoothness <= 0.0f) {
				const double target_gain = db_to_linear(peak_aware_target_db);
				state->gain = target_gain + smoothing * (state->gain - target_gain);
				state->gain_db = linear_to_db(state->gain);
				state->gain_rate_db_per_second = 0.0f;
			} else {
				const double remaining_db = state->gain_target_db - state->gain_db;
				float desired_rate = (float)(remaining_db / fader_response_seconds);
				desired_rate = clampf(desired_rate, -downward_rate_limit, upward_rate_limit);
				if (desired_rate * state->gain_rate_db_per_second < 0.0f)
					state->gain_rate_db_per_second = 0.0f;
				const float acceleration = remaining_db < 0.0f ? downward_acceleration
									       : upward_acceleration;
				const float max_rate_step = acceleration / sample_rate;
				const float rate_error = desired_rate - state->gain_rate_db_per_second;
				state->gain_rate_db_per_second += clampf(rate_error, -max_rate_step, max_rate_step);
				double gain_step_db = (double)state->gain_rate_db_per_second / sample_rate;
				if (gain_step_db * remaining_db <= 0.0 || fabs(gain_step_db) >= fabs(remaining_db)) {
					gain_step_db = remaining_db;
					state->gain_rate_db_per_second = 0.0f;
				}
				state->gain_db += gain_step_db;
				/* Double precision preserves tiny dB steps; the second-order expansion avoids exp() per sample. */
				const double linear_step = LVB_NEPER_PER_DB * gain_step_db;
				state->gain *= 1.0 + linear_step + 0.5 * linear_step * linear_step;
			}
			float desired_guard_gain = 1.0f;
			if (estimated_peak > 0.0f && state->gain * estimated_peak > peak_ceiling)
				desired_guard_gain = (float)(peak_ceiling / (state->gain * estimated_peak));
			if (desired_guard_gain < state->peak_guard_gain)
				state->peak_guard_gain = desired_guard_gain;
			else
				state->peak_guard_gain = desired_guard_gain +
							 guard_release * (state->peak_guard_gain - desired_guard_gain);
		}

		double applied_gain = settings->bypass ? 1.0 : state->gain * state->peak_guard_gain;
		last_applied_gain = applied_gain;
		float output_values[LVB_MAX_CHANNELS] = {0};
		double frame_output_energy = 0.0;
		double frame_output_fast_energy = 0.0;
		size_t output_fast_channels = 0;
		for (size_t channel = 0; channel < channels; channel++) {
			float *samples = audio[channel];
			const float input = input_values[channel];
			const float delayed = state->true_peak_delay[channel][state->true_peak_delay_index];
			state->true_peak_delay[channel][state->true_peak_delay_index] = input;
			if (!samples)
				continue;
			float output = state->true_peak_delay_frames >= LVB_TRUE_PEAK_LATENCY ? delayed : 0.0f;
			output = (float)((double)output * applied_gain);
			if (!settings->bypass)
				output = clampf(output, -peak_ceiling, peak_ceiling);
			samples[frame] = output;
			output_values[channel] = output;

			const double weight = channel_weight(channel, channels);
			const double shelf =
				process_biquad(output, state->shelf_b, state->shelf_a, &state->output_k_shelf[channel]);
			const double filtered = process_biquad(shelf, state->highpass_b, state->highpass_a,
							       &state->output_k_highpass[channel]);
			frame_output_energy += weight * filtered * filtered;
			if (weight > 0.0) {
				frame_output_fast_energy += (double)output * output;
				output_fast_channels++;
			}
		}
		if (output_fast_channels > 0)
			frame_output_fast_energy /= (double)output_fast_channels;
		state->output_fast_meter_energy = update_fast_meter_energy(state->output_fast_meter_energy,
									   frame_output_fast_energy,
									   state->fast_meter_attack_coefficient,
									   state->fast_meter_release_coefficient);
		meter_add_frame(&state->output_meter, frame_output_energy, state->bucket_frames);
		if (state->true_peak_delay_frames < LVB_TRUE_PEAK_LATENCY)
			state->true_peak_delay_frames++;
		state->true_peak_delay_index = (state->true_peak_delay_index + 1U) % LVB_TRUE_PEAK_LATENCY;
		const float output_peak = true_peak_fir_values(
			state->output_peak_history, &state->output_peak_history_index, output_values, channels);
		if (output_peak > maximum_output_peak)
			maximum_output_peak = output_peak;
	}

	state->stats.input_momentary_lufs = input_momentary_lufs;
	state->stats.input_short_term_lufs = input_short_term_lufs;
	state->stats.input_fast_rms_dbfs = input_fast_rms_dbfs;
	state->stats.output_fast_rms_dbfs = energy_to_dbfs(state->output_fast_meter_energy);
	state->stats.output_momentary_lufs = meter_momentary_lufs(&state->output_meter);
	state->stats.output_short_term_lufs = meter_short_term_lufs(&state->output_meter);
	state->stats.true_peak_dbtp = (float)linear_to_db(maximum_output_peak);
	state->stats.target_lufs = target_lufs;
	state->stats.peak_ceiling_dbtp = peak_ceiling_db;
	state->stats.max_boost_db = max_boost_db;
	state->stats.max_reduction_db = max_reduction_db;
	state->stats.noise_floor_dbfs = noise_floor_db;
	state->stats.fader_smoothness = fader_smoothness;
	state->stats.quiet_attenuation_db = quiet_attenuation_db;
	state->stats.sample_rate_hz = sample_rate;
	if (state->stats.true_peak_dbtp >= state->peak_hold_dbtp) {
		state->peak_hold_dbtp = state->stats.true_peak_dbtp;
		state->peak_hold_seconds = 1.5f;
	} else if (state->peak_hold_seconds > block_seconds) {
		state->peak_hold_seconds -= block_seconds;
	} else {
		state->peak_hold_seconds = 0.0f;
		state->peak_hold_dbtp =
			fmaxf(state->stats.true_peak_dbtp, state->peak_hold_dbtp - 12.0f * block_seconds);
	}
	state->stats.peak_hold_dbtp = state->peak_hold_dbtp;
	state->stats.gain_db = settings->bypass ? 0.0f : (float)linear_to_db(last_applied_gain);
	state->stats.activity_open = state->activity_open;
	state->stats.bypass = settings->bypass;
}
