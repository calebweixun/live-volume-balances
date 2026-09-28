/*
 * Live Volume Balancer telemetry tests
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "monitor-telemetry.h"

#include <stdio.h>

#define CHECK(condition, message)                                                                                \
	do {                                                                                                       \
		if (!(condition)) {                                                                                \
			fprintf(stderr, "FAIL: %s\n", message);                                                    \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

static struct lvb_stats make_stats(float gain, float sample_rate)
{
	return (struct lvb_stats){
		.input_fast_rms_dbfs = -31.0f + gain,
		.output_fast_rms_dbfs = -28.0f + gain,
		.input_momentary_lufs = -29.0f + gain,
		.input_short_term_lufs = -30.0f + gain,
		.output_momentary_lufs = -26.0f + gain,
		.output_short_term_lufs = -27.0f + gain,
		.true_peak_dbtp = -4.0f + gain,
		.peak_hold_dbtp = -3.0f + gain,
		.gain_db = gain,
		.target_lufs = -18.0f,
		.peak_ceiling_dbtp = -1.0f,
		.max_boost_db = 18.0f + gain,
		.max_reduction_db = 18.0f - gain,
		.noise_floor_dbfs = -46.0f + gain,
		.attack_ms = 180.0f + gain,
		.recovery_ms = 1800.0f + gain,
		.fader_smoothness = 85.0f + gain,
		.quiet_attenuation_db = 3.0f + gain,
		.sample_rate_hz = sample_rate,
		.activity_open = true,
		.bypass = gain < 0.0f,
	};
}

int main(void)
{
	lvb_telemetry_init();
	CHECK(lvb_telemetry_register_slot(2, 101U), "register first instance slot");
	CHECK(lvb_telemetry_register_slot(5, 202U), "register second instance slot");

	const struct lvb_stats first = make_stats(2.5f, 48000.0f);
	const struct lvb_stats second = make_stats(-4.0f, 44100.0f);
	CHECK(lvb_telemetry_publish(2, 101U, &first), "publish first instance telemetry");
	CHECK(lvb_telemetry_publish(5, 202U, &second), "publish second instance telemetry");

	struct lvb_telemetry_snapshot snapshot;
	CHECK(lvb_telemetry_read(2, 101U, &snapshot), "read first instance snapshot");
	CHECK(snapshot.available && snapshot.stats_available && snapshot.sequence == 1U,
	      "first instance snapshot is complete");
	CHECK(snapshot.instance_id == 101U && snapshot.stats.gain_db == 2.5f &&
		      snapshot.stats.sample_rate_hz == 48000.0f,
	      "first instance reads only its own values");
	CHECK(snapshot.stats.max_boost_db == 20.5f && snapshot.stats.max_reduction_db == 15.5f &&
		      snapshot.stats.noise_floor_dbfs == -43.5f && snapshot.stats.attack_ms == 182.5f &&
		      snapshot.stats.recovery_ms == 1802.5f && snapshot.stats.fader_smoothness == 87.5f &&
		      snapshot.stats.quiet_attenuation_db == 5.5f && !snapshot.stats.bypass,
	      "first snapshot keeps meter readings and effective control settings together");
	CHECK(lvb_telemetry_read(5, 202U, &snapshot), "read second instance snapshot");
	CHECK(snapshot.available && snapshot.stats_available && snapshot.sequence == 1U,
	      "second instance snapshot is complete");
	CHECK(snapshot.instance_id == 202U && snapshot.stats.gain_db == -4.0f &&
		      snapshot.stats.sample_rate_hz == 44100.0f,
	      "second instance reads only its own values");
	CHECK(snapshot.stats.max_boost_db == 14.0f && snapshot.stats.max_reduction_db == 22.0f &&
		      snapshot.stats.noise_floor_dbfs == -50.0f && snapshot.stats.attack_ms == 176.0f &&
		      snapshot.stats.recovery_ms == 1796.0f && snapshot.stats.fader_smoothness == 81.0f &&
		      snapshot.stats.quiet_attenuation_db == -1.0f && snapshot.stats.bypass,
	      "second snapshot retains its own effective settings and bypass flag");

	CHECK(lvb_telemetry_unregister_slot(2, 101U), "unregister first instance");
	CHECK(!lvb_telemetry_read(2, 101U, &snapshot), "unloaded instance becomes unavailable");
	CHECK(lvb_telemetry_register_slot(2, 303U), "reuse released telemetry slot");
	CHECK(!lvb_telemetry_publish(2, 101U, &first), "stale callback cannot publish into a reused slot");
	CHECK(!lvb_telemetry_unregister_slot(2, 101U), "stale unload cannot clear a reused slot");
	CHECK(lvb_telemetry_read(2, 303U, &snapshot) && snapshot.available && !snapshot.stats_available,
	      "reused slot starts without stale readings");
	const struct lvb_stats third = make_stats(7.0f, 48000.0f);
	CHECK(lvb_telemetry_publish(2, 303U, &third), "new instance publishes after slot reuse");
	CHECK(lvb_telemetry_read(2, 303U, &snapshot) && snapshot.stats.gain_db == 7.0f &&
		      snapshot.stats.sample_rate_hz == 48000.0f,
	      "new owner remains isolated after reuse");

	puts("All monitor telemetry tests passed.");
	return 0;
}
