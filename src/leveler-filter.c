/*
 * Live Volume Balancer
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include <media-io/audio-io.h>

#ifdef _MSC_VER
#include <windows.h>
#else
#include <stdatomic.h>
#endif
#include <stdint.h>
#include <string.h>

#include "leveler-dsp.h"

#ifdef _MSC_VER
typedef volatile LONG lvb_atomic_uint32_t;
#else
typedef _Atomic(uint32_t) lvb_atomic_uint32_t;
#endif

#define SETTING_BYPASS "bypass"
#define SETTING_TARGET "target_level_db"
#define SETTING_MAX_BOOST "max_boost_db"
#define SETTING_MAX_REDUCTION "max_reduction_db"
#define SETTING_ATTACK "attack_ms"
#define SETTING_RELEASE "release_ms"
#define SETTING_NOISE_FLOOR "noise_floor_db"
#define SETTING_CEILING "peak_ceiling_db"

struct leveler_filter_data {
	struct lvb_state state;
	lvb_atomic_uint32_t bypass;
	lvb_atomic_uint32_t target_level_db;
	lvb_atomic_uint32_t max_boost_db;
	lvb_atomic_uint32_t max_reduction_db;
	lvb_atomic_uint32_t attack_ms;
	lvb_atomic_uint32_t release_ms;
	lvb_atomic_uint32_t noise_floor_db;
	lvb_atomic_uint32_t peak_ceiling_db;
	size_t channels;
	float sample_rate;
};

#ifdef _MSC_VER
static void lvb_atomic_init(lvb_atomic_uint32_t *value, uint32_t initial)
{
	*value = (LONG)initial;
}

static void lvb_atomic_store(lvb_atomic_uint32_t *value, uint32_t next)
{
	InterlockedExchange(value, (LONG)next);
}

static uint32_t lvb_atomic_load(const lvb_atomic_uint32_t *value)
{
	return (uint32_t)InterlockedCompareExchange((volatile LONG *)value, 0, 0);
}
#else
static void lvb_atomic_init(lvb_atomic_uint32_t *value, uint32_t initial)
{
	atomic_init(value, initial);
}

static void lvb_atomic_store(lvb_atomic_uint32_t *value, uint32_t next)
{
	atomic_store_explicit(value, next, memory_order_relaxed);
}

static uint32_t lvb_atomic_load(const lvb_atomic_uint32_t *value)
{
	return atomic_load_explicit(value, memory_order_relaxed);
}
#endif

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static float bits_float(uint32_t bits)
{
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static struct lvb_settings leveler_settings_snapshot(const struct leveler_filter_data *filter)
{
	return (struct lvb_settings){
		.target_level_db = bits_float(lvb_atomic_load(&filter->target_level_db)),
		.max_boost_db = bits_float(lvb_atomic_load(&filter->max_boost_db)),
		.max_reduction_db = bits_float(lvb_atomic_load(&filter->max_reduction_db)),
		.attack_ms = bits_float(lvb_atomic_load(&filter->attack_ms)),
		.release_ms = bits_float(lvb_atomic_load(&filter->release_ms)),
		.noise_floor_db = bits_float(lvb_atomic_load(&filter->noise_floor_db)),
		.peak_ceiling_db = bits_float(lvb_atomic_load(&filter->peak_ceiling_db)),
		.bypass = lvb_atomic_load(&filter->bypass) != 0,
	};
}

static const char *leveler_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LiveVolumeBalancer");
}

static void leveler_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, SETTING_BYPASS, false);
	obs_data_set_default_double(settings, SETTING_TARGET, -18.0);
	obs_data_set_default_double(settings, SETTING_MAX_BOOST, 12.0);
	obs_data_set_default_double(settings, SETTING_MAX_REDUCTION, 18.0);
	obs_data_set_default_int(settings, SETTING_ATTACK, 20);
	obs_data_set_default_int(settings, SETTING_RELEASE, 350);
	obs_data_set_default_double(settings, SETTING_NOISE_FLOOR, -55.0);
	obs_data_set_default_double(settings, SETTING_CEILING, -1.0);
}

static void leveler_update(void *opaque, obs_data_t *settings)
{
	struct leveler_filter_data *filter = opaque;
	lvb_atomic_store(&filter->bypass, obs_data_get_bool(settings, SETTING_BYPASS) ? 1U : 0U);
	lvb_atomic_store(&filter->target_level_db, float_bits((float)obs_data_get_double(settings, SETTING_TARGET)));
	lvb_atomic_store(&filter->max_boost_db, float_bits((float)obs_data_get_double(settings, SETTING_MAX_BOOST)));
	lvb_atomic_store(&filter->max_reduction_db,
			 float_bits((float)obs_data_get_double(settings, SETTING_MAX_REDUCTION)));
	lvb_atomic_store(&filter->attack_ms, float_bits((float)obs_data_get_int(settings, SETTING_ATTACK)));
	lvb_atomic_store(&filter->release_ms, float_bits((float)obs_data_get_int(settings, SETTING_RELEASE)));
	lvb_atomic_store(&filter->noise_floor_db,
			 float_bits((float)obs_data_get_double(settings, SETTING_NOISE_FLOOR)));
	lvb_atomic_store(&filter->peak_ceiling_db, float_bits((float)obs_data_get_double(settings, SETTING_CEILING)));
}

static void *leveler_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct leveler_filter_data *filter = bzalloc(sizeof(*filter));
	lvb_state_init(&filter->state);
	lvb_atomic_init(&filter->bypass, 0U);
	lvb_atomic_init(&filter->target_level_db, float_bits(-18.0f));
	lvb_atomic_init(&filter->max_boost_db, float_bits(12.0f));
	lvb_atomic_init(&filter->max_reduction_db, float_bits(18.0f));
	lvb_atomic_init(&filter->attack_ms, float_bits(20.0f));
	lvb_atomic_init(&filter->release_ms, float_bits(350.0f));
	lvb_atomic_init(&filter->noise_floor_db, float_bits(-55.0f));
	lvb_atomic_init(&filter->peak_ceiling_db, float_bits(-1.0f));

	audio_t *audio = obs_get_audio();
	if (audio) {
		filter->channels = audio_output_get_channels(audio);
		filter->sample_rate = (float)audio_output_get_sample_rate(audio);
	}
	if (filter->channels > MAX_AV_PLANES)
		filter->channels = MAX_AV_PLANES;

	leveler_update(filter, settings);
	return filter;
}

static void leveler_destroy(void *opaque)
{
	bfree(opaque);
}

static struct obs_audio_data *leveler_filter_audio(void *opaque, struct obs_audio_data *audio)
{
	struct leveler_filter_data *filter = opaque;
	if (!filter || !audio || filter->channels == 0 || filter->sample_rate <= 0.0f)
		return audio;

	float *planes[MAX_AV_PLANES] = {0};
	for (size_t channel = 0; channel < filter->channels; channel++)
		planes[channel] = (float *)audio->data[channel];

	const struct lvb_settings settings = leveler_settings_snapshot(filter);
	lvb_process(&filter->state, &settings, filter->channels, planes, audio->frames, filter->sample_rate);
	return audio;
}

static obs_properties_t *leveler_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *property;

	obs_properties_add_bool(properties, SETTING_BYPASS, obs_module_text("Bypass"));

	property = obs_properties_add_float_slider(properties, SETTING_TARGET, obs_module_text("TargetLevel"), -30.0,
						   -9.0, 0.5);
	obs_property_float_set_suffix(property, " dBFS RMS");

	property = obs_properties_add_float_slider(properties, SETTING_MAX_BOOST, obs_module_text("MaximumBoost"), 0.0,
						   24.0, 0.5);
	obs_property_float_set_suffix(property, " dB");

	property = obs_properties_add_float_slider(properties, SETTING_MAX_REDUCTION,
						   obs_module_text("MaximumReduction"), 0.0, 36.0, 0.5);
	obs_property_float_set_suffix(property, " dB");

	property = obs_properties_add_int_slider(properties, SETTING_ATTACK, obs_module_text("Attack"), 5, 200, 1);
	obs_property_int_set_suffix(property, " ms");

	property = obs_properties_add_int_slider(properties, SETTING_RELEASE, obs_module_text("Release"), 50, 2000, 10);
	obs_property_int_set_suffix(property, " ms");

	property = obs_properties_add_float_slider(properties, SETTING_NOISE_FLOOR, obs_module_text("NoiseFloor"),
						   -80.0, -24.0, 1.0);
	obs_property_float_set_suffix(property, " dBFS");

	property = obs_properties_add_float_slider(properties, SETTING_CEILING, obs_module_text("PeakCeiling"), -12.0,
						   0.0, 0.1);
	obs_property_float_set_suffix(property, " dBFS");

	return properties;
}

struct obs_source_info live_volume_balancer_filter = {
	.id = "live_volume_balancer_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = leveler_name,
	.create = leveler_create,
	.destroy = leveler_destroy,
	.update = leveler_update,
	.filter_audio = leveler_filter_audio,
	.get_defaults = leveler_defaults,
	.get_properties = leveler_properties,
};
