/*
 * Live Volume Balancer
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <media-io/audio-io.h>

#ifdef _MSC_VER
#include <windows.h>
#else
#include <pthread.h>
#include <stdatomic.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "leveler-dsp.h"
#include "monitor-bridge.h"
#include "monitor-telemetry.h"

#ifdef _MSC_VER
typedef volatile LONG lvb_atomic_uint32_t;
static SRWLOCK instances_lock = SRWLOCK_INIT;
#define INSTANCES_LOCK() AcquireSRWLockExclusive(&instances_lock)
#define INSTANCES_UNLOCK() ReleaseSRWLockExclusive(&instances_lock)
#else
typedef _Atomic(uint32_t) lvb_atomic_uint32_t;
static pthread_mutex_t instances_lock = PTHREAD_MUTEX_INITIALIZER;
#define INSTANCES_LOCK() pthread_mutex_lock(&instances_lock)
#define INSTANCES_UNLOCK() pthread_mutex_unlock(&instances_lock)
#endif

#define SETTING_BYPASS "bypass"
#define SETTING_TARGET_LUFS "target_lufs"
#define SETTING_MAX_BOOST "max_boost_db"
#define SETTING_MAX_REDUCTION "max_reduction_db"
#define SETTING_ATTACK "attack_ms"
#define SETTING_RELEASE "release_ms"
#define SETTING_NOISE_FLOOR "noise_floor_db"
#define SETTING_CEILING "peak_ceiling_db"
#define SETTING_SCHEMA_VERSION "settings_schema_version"

#define LVB_MAX_FILTER_INSTANCES LVB_MONITOR_MAX_SOURCES

struct leveler_filter_data {
	struct lvb_state state;
	lvb_atomic_uint32_t bypass;
	lvb_atomic_uint32_t target_lufs;
	lvb_atomic_uint32_t max_boost_db;
	lvb_atomic_uint32_t max_reduction_db;
	lvb_atomic_uint32_t attack_ms;
	lvb_atomic_uint32_t release_ms;
	lvb_atomic_uint32_t noise_floor_db;
	lvb_atomic_uint32_t peak_ceiling_db;
	obs_source_t *filter_source;
	char parent_source_name[LVB_MONITOR_SOURCE_NAME_LENGTH];
	uint32_t instance_id;
	size_t telemetry_slot;
	size_t channels;
	float sample_rate;
};

static struct leveler_filter_data *filter_instances[LVB_MAX_FILTER_INSTANCES];
static uint32_t next_instance_id = 1;
static bool telemetry_initialized;

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
	atomic_store_explicit(value, next, memory_order_release);
}

static uint32_t lvb_atomic_load(const lvb_atomic_uint32_t *value)
{
	return atomic_load_explicit(value, memory_order_acquire);
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

static bool leveler_register_instance(struct leveler_filter_data *filter)
{
	bool registered = false;
	INSTANCES_LOCK();
	if (!telemetry_initialized) {
		lvb_telemetry_init();
		telemetry_initialized = true;
	}
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (!filter_instances[i]) {
			filter->instance_id = next_instance_id++;
			if (next_instance_id == 0)
				next_instance_id = 1;
			filter->telemetry_slot = i;
			lvb_telemetry_register_slot(i, filter->instance_id);
			filter_instances[i] = filter;
			registered = true;
			break;
		}
	}
	INSTANCES_UNLOCK();
	return registered;
}

static void leveler_unregister_instance(struct leveler_filter_data *filter)
{
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (filter_instances[i] == filter) {
			filter_instances[i] = NULL;
			lvb_telemetry_unregister_slot(filter->telemetry_slot, filter->instance_id);
			break;
		}
	}
	INSTANCES_UNLOCK();
}

static struct lvb_settings leveler_settings_snapshot(const struct leveler_filter_data *filter)
{
	return (struct lvb_settings){
		.target_lufs = bits_float(lvb_atomic_load(&filter->target_lufs)),
		.max_boost_db = bits_float(lvb_atomic_load(&filter->max_boost_db)),
		.max_reduction_db = bits_float(lvb_atomic_load(&filter->max_reduction_db)),
		.attack_ms = bits_float(lvb_atomic_load(&filter->attack_ms)),
		.release_ms = bits_float(lvb_atomic_load(&filter->release_ms)),
		.noise_floor_db = bits_float(lvb_atomic_load(&filter->noise_floor_db)),
		.peak_ceiling_db = bits_float(lvb_atomic_load(&filter->peak_ceiling_db)),
		.bypass = lvb_atomic_load(&filter->bypass) != 0,
	};
}

size_t lvb_monitor_list_sources(struct lvb_monitor_source *sources, size_t capacity)
{
	size_t count = 0;
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		struct leveler_filter_data *filter = filter_instances[i];
		if (!filter)
			continue;
		if (sources && count < capacity) {
			sources[count].instance_id = filter->instance_id;
			sources[count].telemetry_slot = filter->telemetry_slot;
			const char *name = filter->parent_source_name;
			if (name[0] != '\0')
				snprintf(sources[count].name, sizeof(sources[count].name), "%s (#%u)", name,
					 filter->instance_id);
			else
				snprintf(sources[count].name, sizeof(sources[count].name), "Audio source #%u",
					 filter->instance_id);
		}
		count++;
	}
	INSTANCES_UNLOCK();
	return count < capacity ? count : capacity;
}

void lvb_monitor_read_source(size_t telemetry_slot, uint32_t instance_id, struct lvb_monitor_snapshot *snapshot)
{
	if (!snapshot)
		return;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->source_instance_id = instance_id;
	struct lvb_telemetry_snapshot telemetry;
	if (!lvb_telemetry_read(telemetry_slot, instance_id, &telemetry))
		return;
	snapshot->source_available = telemetry.available;
	snapshot->stats_available = telemetry.stats_available;
	snapshot->sequence = telemetry.sequence;
	snapshot->stats = telemetry.stats;
}

bool lvb_monitor_open_source_properties(uint32_t instance_id)
{
	obs_source_t *filter_source = NULL;
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (filter_instances[i] && filter_instances[i]->instance_id == instance_id) {
			filter_source = obs_source_get_ref(filter_instances[i]->filter_source);
			break;
		}
	}
	INSTANCES_UNLOCK();
	if (!filter_source)
		return false;
	obs_frontend_open_source_properties(filter_source);
	obs_source_release(filter_source);
	return true;
}

static const char *leveler_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LiveVolumeBalancer");
}

static void leveler_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, SETTING_BYPASS, false);
	obs_data_set_default_double(settings, SETTING_TARGET_LUFS, -18.0);
	obs_data_set_default_double(settings, SETTING_MAX_BOOST, 18.0);
	obs_data_set_default_double(settings, SETTING_MAX_REDUCTION, 18.0);
	obs_data_set_default_int(settings, SETTING_ATTACK, 180);
	obs_data_set_default_int(settings, SETTING_RELEASE, 1800);
	obs_data_set_default_double(settings, SETTING_NOISE_FLOOR, -46.0);
	obs_data_set_default_double(settings, SETTING_CEILING, -1.0);
}

static void migrate_legacy_settings(obs_data_t *settings)
{
	if (!settings || obs_data_get_int(settings, SETTING_SCHEMA_VERSION) >= 2)
		return;

	/*
	 * Every pre-v2 filter needs the automatic-leveling defaults. The old mode
	 * field may only be a default (not a user value), so schema version is the
	 * reliable one-time migration marker. Newly created filters pass through
	 * here once too; this happens before their settings can be edited.
	 * Preserve the chosen target and explicit safety controls.
	 */
	if (!obs_data_has_user_value(settings, SETTING_TARGET_LUFS))
		obs_data_set_double(settings, SETTING_TARGET_LUFS, -18.0);
	obs_data_set_double(settings, SETTING_MAX_BOOST, 18.0);
	obs_data_set_double(settings, SETTING_MAX_REDUCTION, 18.0);
	obs_data_set_int(settings, SETTING_ATTACK, 180);
	obs_data_set_int(settings, SETTING_RELEASE, 1800);
	obs_data_set_double(settings, SETTING_NOISE_FLOOR, -46.0);
	obs_data_set_int(settings, SETTING_SCHEMA_VERSION, 2);
}

static void set_property_help(obs_property_t *property, const char *key)
{
	if (property)
		obs_property_set_long_description(property, obs_module_text(key));
}

static void leveler_update(void *opaque, obs_data_t *settings)
{
	struct leveler_filter_data *filter = opaque;
	if (!filter || !settings)
		return;
	migrate_legacy_settings(settings);
	lvb_atomic_store(&filter->bypass, obs_data_get_bool(settings, SETTING_BYPASS) ? 1U : 0U);
	lvb_atomic_store(&filter->target_lufs, float_bits((float)obs_data_get_double(settings, SETTING_TARGET_LUFS)));
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
	struct leveler_filter_data *filter = bzalloc(sizeof(*filter));
	if (!filter)
		return NULL;
	lvb_state_init(&filter->state);
	lvb_atomic_init(&filter->bypass, 0U);
	lvb_atomic_init(&filter->target_lufs, float_bits(-18.0f));
	lvb_atomic_init(&filter->max_boost_db, float_bits(18.0f));
	lvb_atomic_init(&filter->max_reduction_db, float_bits(18.0f));
	lvb_atomic_init(&filter->attack_ms, float_bits(180.0f));
	lvb_atomic_init(&filter->release_ms, float_bits(1800.0f));
	lvb_atomic_init(&filter->noise_floor_db, float_bits(-46.0f));
	lvb_atomic_init(&filter->peak_ceiling_db, float_bits(-1.0f));
	filter->filter_source = source;
	audio_t *audio = obs_get_audio();
	if (audio) {
		filter->channels = audio_output_get_channels(audio);
		filter->sample_rate = (float)audio_output_get_sample_rate(audio);
	}
	if (filter->channels > LVB_MAX_CHANNELS || filter->channels > MAX_AV_PLANES)
		filter->channels = LVB_MAX_CHANNELS < MAX_AV_PLANES ? LVB_MAX_CHANNELS : MAX_AV_PLANES;
	if (!leveler_register_instance(filter)) {
		bfree(filter);
		return NULL;
	}
	leveler_update(filter, settings);
	return filter;
}

static void leveler_filter_add(void *opaque, obs_source_t *source)
{
	struct leveler_filter_data *filter = opaque;
	if (!filter || !source)
		return;
	const char *name = obs_source_get_name(source);
	if (!name || name[0] == '\0')
		return;
	INSTANCES_LOCK();
	snprintf(filter->parent_source_name, sizeof(filter->parent_source_name), "%s", name);
	INSTANCES_UNLOCK();
}

static void leveler_destroy(void *opaque)
{
	struct leveler_filter_data *filter = opaque;
	if (!filter)
		return;
	leveler_unregister_instance(filter);
	bfree(filter);
}

static struct obs_audio_data *leveler_filter_audio(void *opaque, struct obs_audio_data *audio)
{
	struct leveler_filter_data *filter = opaque;
	if (!filter || !audio || filter->channels == 0 || filter->sample_rate <= 0.0f)
		return audio;

	const struct lvb_settings settings = leveler_settings_snapshot(filter);
	float *planes[MAX_AV_PLANES] = {0};
	for (size_t channel = 0; channel < filter->channels && channel < MAX_AV_PLANES; channel++) {
		if (audio->data[channel])
			planes[channel] = (float *)audio->data[channel];
	}

	lvb_process(&filter->state, &settings, filter->channels, planes, audio->frames, filter->sample_rate);
	struct lvb_stats stats;
	lvb_get_stats(&filter->state, &stats);
	lvb_telemetry_publish(filter->telemetry_slot, filter->instance_id, &stats);
	return audio;
}

static obs_properties_t *leveler_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *property;
	property = obs_properties_add_float_slider(properties, SETTING_TARGET_LUFS, obs_module_text("TargetLoudness"),
						   -36.0, -6.0, 0.5);
	obs_property_float_set_suffix(property, " LUFS");
	set_property_help(property, "TargetLoudnessHelp");
	property = obs_properties_add_float_slider(properties, SETTING_CEILING, obs_module_text("PeakCeiling"), -24.0,
						   0.0, 0.1);
	obs_property_float_set_suffix(property, " dBTP est.");
	set_property_help(property, "PeakCeilingHelp");
	property = obs_properties_add_bool(properties, SETTING_BYPASS, obs_module_text("Bypass"));
	set_property_help(property, "BypassHelp");

	obs_properties_t *advanced = obs_properties_create();
	property = obs_properties_add_float_slider(advanced, SETTING_MAX_BOOST, obs_module_text("MaximumCompensation"),
						   0.0, 36.0, 0.5);
	obs_property_float_set_suffix(property, " dB");
	set_property_help(property, "MaximumCompensationHelp");
	property = obs_properties_add_float_slider(advanced, SETTING_MAX_REDUCTION, obs_module_text("MaximumReduction"),
						   0.0, 36.0, 0.5);
	obs_property_float_set_suffix(property, " dB");
	set_property_help(property, "MaximumReductionHelp");
	property = obs_properties_add_float_slider(advanced, SETTING_NOISE_FLOOR, obs_module_text("ActivityFloor"),
						   -100.0, -6.0, 1.0);
	obs_property_float_set_suffix(property, " dBFS");
	set_property_help(property, "ActivityFloorHelp");
	property = obs_properties_add_int_slider(advanced, SETTING_ATTACK, obs_module_text("GainAttack"), 10, 3000, 10);
	obs_property_int_set_suffix(property, " ms");
	set_property_help(property, "GainAttackHelp");
	property = obs_properties_add_int_slider(advanced, SETTING_RELEASE, obs_module_text("GainRecovery"), 50, 10000,
						 50);
	obs_property_int_set_suffix(property, " ms");
	set_property_help(property, "GainRecoveryHelp");
	obs_properties_add_group(properties, "advanced", obs_module_text("Advanced"), OBS_GROUP_NORMAL, advanced);
	return properties;
}

struct obs_source_info live_volume_balancer_filter = {
	.id = "live_volume_balancer_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = leveler_name,
	.create = leveler_create,
	.destroy = leveler_destroy,
	.filter_add = leveler_filter_add,
	.update = leveler_update,
	.filter_audio = leveler_filter_audio,
	.get_defaults = leveler_defaults,
	.get_properties = leveler_properties,
};
