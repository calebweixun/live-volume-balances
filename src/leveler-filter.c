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
#include <pthread.h>
#include <stdatomic.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "leveler-dsp.h"
#include "monitor-bridge.h"

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

#define LVB_MAX_FILTER_INSTANCES 64

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
	obs_source_t *source;
	uint32_t instance_id;
	size_t channels;
	float sample_rate;
};

static struct leveler_filter_data *filter_instances[LVB_MAX_FILTER_INSTANCES];
static uint32_t next_instance_id = 1;
static lvb_atomic_uint32_t selected_instance_id;
static lvb_atomic_uint32_t published_instance_id;
/* Audio callbacks try this once; UI readers copy a complete snapshot under it. */
static lvb_atomic_uint32_t published_snapshot_lock;
static lvb_atomic_uint32_t published_stats[9];
static lvb_atomic_uint32_t published_flags;

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

static bool lvb_atomic_set_if_zero(lvb_atomic_uint32_t *value)
{
	return InterlockedCompareExchange(value, 1, 0) == 0;
}

static void lvb_atomic_wait_lock(lvb_atomic_uint32_t *value)
{
	while (!lvb_atomic_set_if_zero(value))
		YieldProcessor();
}

static void lvb_atomic_release_lock(lvb_atomic_uint32_t *value)
{
	InterlockedExchange(value, 0);
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

static bool lvb_atomic_set_if_zero(lvb_atomic_uint32_t *value)
{
	uint32_t expected = 0U;
	return atomic_compare_exchange_strong_explicit(value, &expected, 1U, memory_order_acquire,
						       memory_order_relaxed);
}

static void lvb_atomic_wait_lock(lvb_atomic_uint32_t *value)
{
	while (!lvb_atomic_set_if_zero(value))
		atomic_signal_fence(memory_order_acq_rel);
}

static void lvb_atomic_release_lock(lvb_atomic_uint32_t *value)
{
	atomic_store_explicit(value, 0U, memory_order_release);
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

struct lvb_published_snapshot {
	uint32_t instance_id;
	uint32_t flags;
	uint32_t stats[9];
};

static void read_published_snapshot(struct lvb_published_snapshot *snapshot)
{
	lvb_atomic_wait_lock(&published_snapshot_lock);
	snapshot->instance_id = lvb_atomic_load(&published_instance_id);
	snapshot->flags = lvb_atomic_load(&published_flags);
	for (size_t i = 0; i < 9; i++)
		snapshot->stats[i] = lvb_atomic_load(&published_stats[i]);
	lvb_atomic_release_lock(&published_snapshot_lock);
}

static void clear_published_snapshot(void)
{
	lvb_atomic_wait_lock(&published_snapshot_lock);
	lvb_atomic_store(&published_instance_id, 0U);
	lvb_atomic_store(&published_flags, 0U);
	for (size_t i = 0; i < 9; i++)
		lvb_atomic_store(&published_stats[i], float_bits(-120.0f));
	lvb_atomic_release_lock(&published_snapshot_lock);
}

static void leveler_register_instance(struct leveler_filter_data *filter)
{
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (!filter_instances[i]) {
			filter->instance_id = next_instance_id++;
			if (next_instance_id == 0)
				next_instance_id = 1;
			filter_instances[i] = filter;
			if (lvb_atomic_load(&selected_instance_id) == 0U) {
				lvb_atomic_store(&selected_instance_id, filter->instance_id);
				clear_published_snapshot();
			}
			break;
		}
	}
	INSTANCES_UNLOCK();
}

static void leveler_unregister_instance(struct leveler_filter_data *filter)
{
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (filter_instances[i] == filter) {
			filter_instances[i] = NULL;
			break;
		}
	}
	if (lvb_atomic_load(&selected_instance_id) == filter->instance_id) {
		lvb_atomic_store(&selected_instance_id, 0U);
		for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
			if (filter_instances[i]) {
				lvb_atomic_store(&selected_instance_id, filter_instances[i]->instance_id);
				break;
			}
		}
		clear_published_snapshot();
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

static void publish_stats(uint32_t instance_id, const struct lvb_stats *stats)
{
	if (instance_id != lvb_atomic_load(&selected_instance_id) || !lvb_atomic_set_if_zero(&published_snapshot_lock))
		return;
	if (instance_id != lvb_atomic_load(&selected_instance_id)) {
		lvb_atomic_release_lock(&published_snapshot_lock);
		return;
	}
	lvb_atomic_store(&published_instance_id, 0U);
	lvb_atomic_store(&published_stats[0], float_bits(stats->input_momentary_lufs));
	lvb_atomic_store(&published_stats[1], float_bits(stats->input_short_term_lufs));
	lvb_atomic_store(&published_stats[2], float_bits(stats->output_momentary_lufs));
	lvb_atomic_store(&published_stats[3], float_bits(stats->output_short_term_lufs));
	lvb_atomic_store(&published_stats[4], float_bits(stats->true_peak_dbtp));
	lvb_atomic_store(&published_stats[5], float_bits(stats->peak_hold_dbtp));
	lvb_atomic_store(&published_stats[6], float_bits(stats->gain_db));
	lvb_atomic_store(&published_stats[7], float_bits(stats->target_lufs));
	lvb_atomic_store(&published_stats[8], float_bits(stats->peak_ceiling_dbtp));
	lvb_atomic_store(&published_flags, stats->activity_open ? 1U : 0U);
	if (instance_id == lvb_atomic_load(&selected_instance_id))
		lvb_atomic_store(&published_instance_id, instance_id);
	lvb_atomic_release_lock(&published_snapshot_lock);
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
			const char *name = filter->source ? obs_source_get_name(filter->source) : NULL;
			if (name && name[0] != '\0')
				snprintf(sources[count].name, sizeof(sources[count].name), "%s (#%u)", name,
					 filter->instance_id);
			else
				snprintf(sources[count].name, sizeof(sources[count].name), "Audio source %u",
					 filter->instance_id);
		}
		count++;
	}
	INSTANCES_UNLOCK();
	return count < capacity ? count : capacity;
}

void lvb_monitor_select_source(uint32_t instance_id)
{
	bool found = false;
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (filter_instances[i] && filter_instances[i]->instance_id == instance_id) {
			found = true;
			break;
		}
	}
	if (found && instance_id != lvb_atomic_load(&selected_instance_id)) {
		lvb_atomic_store(&selected_instance_id, instance_id);
		clear_published_snapshot();
	}
	INSTANCES_UNLOCK();
}

void lvb_monitor_read(struct lvb_monitor_snapshot *snapshot)
{
	if (!snapshot)
		return;
	memset(snapshot, 0, sizeof(*snapshot));
	struct lvb_published_snapshot published;
	read_published_snapshot(&published);
	snapshot->source_instance_id = lvb_atomic_load(&selected_instance_id);
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (filter_instances[i] && filter_instances[i]->instance_id == snapshot->source_instance_id) {
			snapshot->source_available = true;
			break;
		}
	}
	INSTANCES_UNLOCK();
	snapshot->stats_available = snapshot->source_available && published.instance_id == snapshot->source_instance_id;
	if (!snapshot->stats_available)
		return;
	snapshot->stats.input_momentary_lufs = bits_float(published.stats[0]);
	snapshot->stats.input_short_term_lufs = bits_float(published.stats[1]);
	snapshot->stats.output_momentary_lufs = bits_float(published.stats[2]);
	snapshot->stats.output_short_term_lufs = bits_float(published.stats[3]);
	snapshot->stats.true_peak_dbtp = bits_float(published.stats[4]);
	snapshot->stats.peak_hold_dbtp = bits_float(published.stats[5]);
	snapshot->stats.gain_db = bits_float(published.stats[6]);
	snapshot->stats.target_lufs = bits_float(published.stats[7]);
	snapshot->stats.peak_ceiling_dbtp = bits_float(published.stats[8]);
	snapshot->stats.activity_open = (published.flags & 1U) != 0;
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
	filter->source = source;
	audio_t *audio = obs_get_audio();
	if (audio) {
		filter->channels = audio_output_get_channels(audio);
		filter->sample_rate = (float)audio_output_get_sample_rate(audio);
	}
	if (filter->channels > LVB_MAX_CHANNELS || filter->channels > MAX_AV_PLANES)
		filter->channels = LVB_MAX_CHANNELS < MAX_AV_PLANES ? LVB_MAX_CHANNELS : MAX_AV_PLANES;
	leveler_register_instance(filter);
	leveler_update(filter, settings);
	return filter;
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
	publish_stats(filter->instance_id, &stats);
	return audio;
}

static obs_properties_t *leveler_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *property;
	property = obs_properties_add_float_slider(properties, SETTING_TARGET_LUFS, obs_module_text("TargetLoudness"),
						   -30.0, -9.0, 0.5);
	obs_property_float_set_suffix(property, " LUFS");
	set_property_help(property, "TargetLoudnessHelp");
	property = obs_properties_add_float_slider(properties, SETTING_CEILING, obs_module_text("PeakCeiling"), -12.0,
						   0.0, 0.1);
	obs_property_float_set_suffix(property, " dBTP est.");
	set_property_help(property, "PeakCeilingHelp");
	property = obs_properties_add_bool(properties, SETTING_BYPASS, obs_module_text("Bypass"));
	set_property_help(property, "BypassHelp");

	obs_properties_t *advanced = obs_properties_create();
	property = obs_properties_add_float_slider(advanced, SETTING_MAX_BOOST, obs_module_text("MaximumCompensation"),
						   0.0, 18.0, 0.5);
	obs_property_float_set_suffix(property, " dB");
	set_property_help(property, "MaximumCompensationHelp");
	property = obs_properties_add_float_slider(advanced, SETTING_MAX_REDUCTION, obs_module_text("MaximumReduction"),
						   0.0, 24.0, 0.5);
	obs_property_float_set_suffix(property, " dB");
	set_property_help(property, "MaximumReductionHelp");
	property = obs_properties_add_float_slider(advanced, SETTING_NOISE_FLOOR, obs_module_text("ActivityFloor"),
						   -80.0, -24.0, 1.0);
	obs_property_float_set_suffix(property, " dBFS");
	set_property_help(property, "ActivityFloorHelp");
	property = obs_properties_add_int_slider(advanced, SETTING_ATTACK, obs_module_text("GainAttack"), 20, 1000, 10);
	obs_property_int_set_suffix(property, " ms");
	set_property_help(property, "GainAttackHelp");
	property = obs_properties_add_int_slider(advanced, SETTING_RELEASE, obs_module_text("GainRecovery"), 100, 5000,
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
	.update = leveler_update,
	.filter_audio = leveler_filter_audio,
	.get_defaults = leveler_defaults,
	.get_properties = leveler_properties,
};
