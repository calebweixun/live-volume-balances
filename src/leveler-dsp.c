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
#define LVB_CALIBRATION_SECONDS 10.0

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

struct lvb_profile {
	float target_lufs;
	float max_boost_db;
	float max_reduction_db;
	float attack_ms;
	float release_ms;
	float ratio;
	bool vad_gate_boost;
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

static float linear_to_db(float value)
{
	return value > 0.0f ? 20.0f * log10f(value) : -120.0f;
}

static float energy_to_lufs(double energy)
{
	if (!isfinite(energy) || energy <= 1e-12)
		return -120.0f;
	return (float)(LVB_LUFS_OFFSET + 10.0 * log10(energy));
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

	memset(state->k_shelf, 0, sizeof(state->k_shelf));
	memset(state->k_highpass, 0, sizeof(state->k_highpass));
	memset(state->calibration_k_shelf, 0, sizeof(state->calibration_k_shelf));
	memset(state->calibration_k_highpass, 0, sizeof(state->calibration_k_highpass));
	memset(state->voice_highpass, 0, sizeof(state->voice_highpass));
	memset(state->voice_lowpass, 0, sizeof(state->voice_lowpass));
	state->voice_envelope_coefficient = (float)(1.0 - exp(-1.0 / (0.08 * frequency)));
	state->voice_highpass_alpha = exp(-2.0 * LVB_PI * 120.0 / frequency);
	state->voice_lowpass_alpha = exp(-2.0 * LVB_PI * 3800.0 / frequency);
	state->sample_rate = sample_rate;
	state->bucket_frames = (uint32_t)fmax(1.0, floor(frequency * LVB_BUCKET_SECONDS + 0.5));
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

static void push_meter_bucket(struct lvb_state *state, double energy)
{
	if (state->meter_count == LVB_METER_BUCKETS)
		state->short_term_sum -= state->meter_energy[state->meter_index];
	else
		state->meter_count++;
	state->meter_energy[state->meter_index] = energy;
	state->short_term_sum += energy;
	state->meter_index = (state->meter_index + 1U) % LVB_METER_BUCKETS;

	if (state->momentary_count == 40U)
		state->momentary_sum -= state->momentary_energy[state->momentary_index];
	else
		state->momentary_count++;
	state->momentary_energy[state->momentary_index] = energy;
	state->momentary_sum += energy;
	state->momentary_index = (state->momentary_index + 1U) % 40U;
}

static struct lvb_profile profile_for(const struct lvb_settings *settings, enum lvb_mode mode)
{
	struct lvb_profile profile;
	switch (mode) {
	case LVB_MODE_WORSHIP:
		profile = (struct lvb_profile){-20.0f, 4.0f, 6.0f, 180.0f, 1800.0f, 1.5f, false};
		break;
	case LVB_MODE_WORSHIP_ACOUSTIC:
		profile = (struct lvb_profile){-18.0f, 10.0f, 6.0f, 160.0f, 1400.0f, 1.7f, false};
		break;
	case LVB_MODE_SERMON:
		profile = (struct lvb_profile){-18.0f, 10.0f, 12.0f, 90.0f, 1200.0f, 3.0f, false};
		break;
	case LVB_MODE_AUTO_ASSIST:
		profile = (struct lvb_profile){-19.0f, 6.0f, 8.0f, 140.0f, 1600.0f, 2.0f, true};
		break;
	case LVB_MODE_CUSTOM:
	default:
		profile = (struct lvb_profile){clampf(settings->target_lufs, -30.0f, -9.0f),
					       clampf(settings->max_boost_db, 0.0f, 18.0f),
					       clampf(settings->max_reduction_db, 0.0f, 24.0f),
					       clampf(settings->attack_ms, 20.0f, 1000.0f),
					       clampf(settings->release_ms, 100.0f, 5000.0f),
					       2.0f,
					       false};
		break;
	}
	return profile;
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

static void update_voice_detection(struct lvb_state *state, double band_energy, double mix_energy, float sample_rate)
{
	state->voice_envelope += state->voice_envelope_coefficient * ((float)band_energy - state->voice_envelope);
	const bool candidate = state->voice_envelope > 3.2e-5f && mix_energy > 1e-7 && band_energy / mix_energy > 0.18;
	if (candidate) {
		state->voice_active = true;
		state->voice_hold_seconds = 0.30f;
	} else if (state->voice_hold_seconds > 0.0f) {
		state->voice_hold_seconds = fmaxf(0.0f, state->voice_hold_seconds - 1.0f / sample_rate);
	} else {
		state->voice_active = false;
	}
	state->stats.voice_active = state->voice_active;
	state->stats.voice_activity = candidate ? 1.0f : (state->voice_active ? 0.5f : 0.0f);
}

void lvb_state_init(struct lvb_state *state)
{
	if (!state)
		return;
	memset(state, 0, sizeof(*state));
	state->gain = 1.0f;
	state->sample_rate = 0.0f;
	state->stats.momentary_lufs = -120.0f;
	state->stats.short_term_lufs = -120.0f;
	state->stats.true_peak_dbtp = -120.0f;
	state->stats.calibration_measured_lufs = -120.0f;
	state->stats.mode = LVB_MODE_WORSHIP;
	state->stats.calibration_mode = LVB_MODE_WORSHIP;
}

void lvb_calibration_start(struct lvb_state *state, const struct lvb_settings *settings)
{
	if (!state || !settings)
		return;
	const enum lvb_mode mode =
		settings->mode >= LVB_MODE_CUSTOM && settings->mode < LVB_MODE_COUNT ? settings->mode : LVB_MODE_CUSTOM;
	const struct lvb_profile profile = profile_for(settings, mode);
	state->calibration_mode = (uint32_t)mode;
	state->calibration_target_lufs = clampf(profile.target_lufs, -36.0f, -6.0f);
	state->calibration_base_trim_db = clampf(settings->mode_trim_db[mode], -12.0f, 12.0f);
	state->calibration_elapsed = 0.0;
	state->calibration_energy_sum = 0.0;
	state->calibration_sample_count = 0;
	state->calibration_frame_count = 0;
	memset(state->calibration_k_shelf, 0, sizeof(state->calibration_k_shelf));
	memset(state->calibration_k_highpass, 0, sizeof(state->calibration_k_highpass));
	state->calibration_active = true;
	state->calibration_ready = false;
	state->calibration_suggestion_db = 0.0f;
	state->calibration_measured_lufs = -120.0f;
}

void lvb_calibration_reset(struct lvb_state *state)
{
	if (!state)
		return;
	state->calibration_active = false;
	state->calibration_ready = false;
	state->calibration_elapsed = 0.0;
	state->calibration_suggestion_db = 0.0f;
	state->calibration_measured_lufs = -120.0f;
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

	const enum lvb_mode mode =
		settings->mode >= LVB_MODE_CUSTOM && settings->mode < LVB_MODE_COUNT ? settings->mode : LVB_MODE_CUSTOM;
	const struct lvb_profile profile = profile_for(settings, mode);
	const float mode_trim = clampf(settings->mode_trim_db[mode], -12.0f, 12.0f);
	const float target_lufs = clampf(profile.target_lufs, -36.0f, -6.0f);
	const float noise_floor_db = clampf(settings->noise_floor_db, -100.0f, 0.0f);
	const float peak_ceiling_db = clampf(settings->peak_ceiling_db, -12.0f, 0.0f);
	const float peak_ceiling = db_to_linear(peak_ceiling_db);

	for (size_t frame = 0; frame < frames; frame++) {
		double frame_energy = 0.0;
		double frame_voice_energy = 0.0;
		for (size_t channel = 0; channel < channels; channel++) {
			const float *samples = audio[channel];
			if (!samples)
				continue;
			const double weight = channel_weight(channel, channels);
			const double input = isfinite(samples[frame]) ? samples[frame] : 0.0;
			const double shelf =
				process_biquad(input, state->shelf_b, state->shelf_a, &state->k_shelf[channel]);
			const double filtered = process_biquad(shelf, state->highpass_b, state->highpass_a,
							       &state->k_highpass[channel]);
			frame_energy += weight * filtered * filtered;

			const double hp = input - state->voice_highpass[channel].x1 +
					  state->voice_highpass_alpha * state->voice_highpass[channel].y1;
			state->voice_highpass[channel].x1 = input;
			state->voice_highpass[channel].y1 = hp;
			const double band = (1.0 - state->voice_lowpass_alpha) * hp +
					    state->voice_lowpass_alpha * state->voice_lowpass[channel].y1;
			state->voice_lowpass[channel].y1 = band;
			frame_voice_energy += weight * band * band;
		}
		state->current_bucket_sum += frame_energy;
		state->frames_in_bucket++;
		if (state->frames_in_bucket >= state->bucket_frames) {
			const double bucket_energy = state->current_bucket_sum / (double)state->frames_in_bucket;
			push_meter_bucket(state, bucket_energy);
			state->current_bucket_sum = 0.0;
			state->frames_in_bucket = 0;
		}
		update_voice_detection(state, frame_voice_energy, frame_energy, sample_rate);
	}

	const double momentary_average = state->momentary_count ? state->momentary_sum / state->momentary_count : 0.0;
	const double short_average = state->meter_count ? state->short_term_sum / state->meter_count : 0.0;
	const float momentary_lufs = energy_to_lufs(momentary_average);
	const float short_term_lufs = energy_to_lufs(short_average);
	const float reported_level = momentary_lufs;
	const bool below_floor = reported_level < noise_floor_db;
	const bool voice_gate_blocked = profile.vad_gate_boost && !state->voice_active;
	float requested_gain_db = 0.0f;
	if (!below_floor) {
		const float difference_db = target_lufs - reported_level;
		if (difference_db >= 0.0f) {
			if (!voice_gate_blocked)
				requested_gain_db = fminf(difference_db, profile.max_boost_db);
		} else {
			requested_gain_db = -soft_knee_reduction(-difference_db, profile.ratio, 6.0f);
		}
	}
	requested_gain_db = clampf(requested_gain_db, -profile.max_reduction_db, profile.max_boost_db);
	const float loudness_target_gain = db_to_linear(requested_gain_db + mode_trim);
	const float attack_ms = clampf(profile.attack_ms, 5.0f, 2000.0f);
	const float release_ms = clampf(profile.release_ms, 50.0f, 5000.0f);
	const float time_ms = loudness_target_gain < state->gain ? attack_ms : release_ms;
	const float smoothing = expf(-1.0f / (0.001f * time_ms * sample_rate));

	state->stats.momentary_lufs = momentary_lufs;
	state->stats.short_term_lufs = short_term_lufs;
	state->stats.true_peak_dbtp = -120.0f;
	state->stats.mode = mode;
	float maximum_output_peak = 0.0f;
	float last_applied_gain = 1.0f;
	const float guard_release = expf(-1.0f / (0.075f * sample_rate));
	for (size_t frame = 0; frame < frames; frame++) {
		float input_values[LVB_MAX_CHANNELS] = {0};
		for (size_t channel = 0; channel < channels; channel++) {
			if (audio[channel])
				input_values[channel] = audio[channel][frame];
		}
		const float estimated_peak = true_peak_fir_values(
			state->true_peak_history, &state->true_peak_history_index, input_values, channels);

		if (settings->bypass) {
			state->gain = 1.0f;
			state->peak_guard_gain = 1.0f;
		} else {
			state->gain = loudness_target_gain + smoothing * (state->gain - loudness_target_gain);
			float desired_guard_gain = 1.0f;
			if (estimated_peak > 0.0f && state->gain * estimated_peak > peak_ceiling)
				desired_guard_gain = peak_ceiling / (state->gain * estimated_peak);
			if (desired_guard_gain < state->peak_guard_gain)
				state->peak_guard_gain = desired_guard_gain;
			else
				state->peak_guard_gain = desired_guard_gain +
							 guard_release * (state->peak_guard_gain - desired_guard_gain);
		}

		float applied_gain = settings->bypass ? 1.0f : state->gain * state->peak_guard_gain;
		/* Never let stored boost carry into silence/noise or a VAD-gated segment. */
		if (!settings->bypass && (below_floor || voice_gate_blocked) && applied_gain > 1.0f)
			applied_gain = 1.0f;
		last_applied_gain = applied_gain;
		float output_values[LVB_MAX_CHANNELS] = {0};
		double calibration_frame_energy = 0.0;
		for (size_t channel = 0; channel < channels; channel++) {
			float *samples = audio[channel];
			const float input = input_values[channel];
			const float delayed = state->true_peak_delay[channel][state->true_peak_delay_index];
			state->true_peak_delay[channel][state->true_peak_delay_index] = input;
			if (!samples)
				continue;
			float output = state->true_peak_delay_frames >= LVB_TRUE_PEAK_LATENCY ? delayed : 0.0f;
			if (isfinite(output)) {
				output *= applied_gain;
				if (!settings->bypass)
					output = clampf(output, -peak_ceiling, peak_ceiling);
			}
			samples[frame] = output;
			output_values[channel] = output;
			if (state->calibration_active) {
				const double clean_output = isfinite(output) ? output : 0.0;
				const double calibrated_shelf = process_biquad(clean_output, state->shelf_b,
									       state->shelf_a,
									       &state->calibration_k_shelf[channel]);
				const double calibrated_filtered =
					process_biquad(calibrated_shelf, state->highpass_b, state->highpass_a,
						       &state->calibration_k_highpass[channel]);
				calibration_frame_energy +=
					channel_weight(channel, channels) * calibrated_filtered * calibrated_filtered;
			}
		}
		if (state->calibration_active) {
			state->calibration_energy_sum += calibration_frame_energy;
			state->calibration_sample_count++;
			state->calibration_frame_count++;
			state->calibration_elapsed += 1.0 / (double)sample_rate;
		}
		if (state->true_peak_delay_frames < LVB_TRUE_PEAK_LATENCY)
			state->true_peak_delay_frames++;
		state->true_peak_delay_index = (state->true_peak_delay_index + 1U) % LVB_TRUE_PEAK_LATENCY;
		const float output_peak = true_peak_fir_values(
			state->output_peak_history, &state->output_peak_history_index, output_values, channels);
		if (output_peak > maximum_output_peak)
			maximum_output_peak = output_peak;
	}

	state->stats.gain_reduction_db = settings->bypass ? 0.0f : fmaxf(0.0f, -linear_to_db(last_applied_gain));
	state->stats.gain_db = settings->bypass ? 0.0f : linear_to_db(last_applied_gain);
	state->stats.true_peak_dbtp = linear_to_db(maximum_output_peak);
	state->meter_true_peak = maximum_output_peak;
	if (state->calibration_active && state->calibration_elapsed >= LVB_CALIBRATION_SECONDS) {
		state->calibration_measured_lufs = energy_to_lufs(
			state->calibration_energy_sum /
			(double)(state->calibration_sample_count ? state->calibration_sample_count : 1U));
		const float requested_correction =
			clampf(state->calibration_target_lufs - state->calibration_measured_lufs, -12.0f, 12.0f);
		const float corrected_trim =
			clampf(state->calibration_base_trim_db + requested_correction, -12.0f, 12.0f);
		state->calibration_suggestion_db = corrected_trim - state->calibration_base_trim_db;
		state->calibration_active = false;
		state->calibration_ready = state->calibration_measured_lufs > -90.0f;
	}
	state->stats.calibration_active = state->calibration_active;
	state->stats.calibration_ready = state->calibration_ready;
	state->stats.calibration_progress =
		state->calibration_active
			? clampf((float)(state->calibration_elapsed / LVB_CALIBRATION_SECONDS), 0.0f, 1.0f)
			: (state->calibration_ready ? 1.0f : 0.0f);
	state->stats.calibration_measured_lufs = state->calibration_measured_lufs;
	state->stats.calibration_suggestion_db = state->calibration_suggestion_db;
	state->stats.calibration_mode = (enum lvb_mode)state->calibration_mode;
}
