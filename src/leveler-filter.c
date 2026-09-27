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
#include <stdio.h>
#include <stdint.h>
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
#define SETTING_MODE "mode"
#define SETTING_TARGET_LUFS "target_lufs"
#define SETTING_MAX_BOOST "max_boost_db"
#define SETTING_MAX_REDUCTION "max_reduction_db"
#define SETTING_ATTACK "attack_ms"
#define SETTING_RELEASE "release_ms"
#define SETTING_NOISE_FLOOR "noise_floor_db"
#define SETTING_CEILING "peak_ceiling_db"
#define SETTING_TRIM_WORSHIP "worship_trim_db"
#define SETTING_TRIM_SERMON "sermon_trim_db"
#define SETTING_TRIM_CUSTOM "custom_trim_db"
#define SETTING_TRIM_AUTO "auto_assist_trim_db"
#define SETTING_TRIM_ACOUSTIC "worship_acoustic_trim_db"

#define LVB_MAX_FILTER_INSTANCES 64

struct leveler_filter_data {
	struct lvb_state state;
	lvb_atomic_uint32_t bypass;
	lvb_atomic_uint32_t mode;
	lvb_atomic_uint32_t target_lufs;
	lvb_atomic_uint32_t max_boost_db;
	lvb_atomic_uint32_t max_reduction_db;
	lvb_atomic_uint32_t attack_ms;
	lvb_atomic_uint32_t release_ms;
	lvb_atomic_uint32_t noise_floor_db;
	lvb_atomic_uint32_t peak_ceiling_db;
	lvb_atomic_uint32_t mode_trim_db[LVB_MODE_COUNT];
	lvb_atomic_uint32_t calibration_start_requested;
	lvb_atomic_uint32_t calibration_reset_requested;
	lvb_atomic_uint32_t calibration_action_pending;
	obs_source_t *source;
	uint32_t instance_id;
	size_t channels;
	float sample_rate;
};

static struct leveler_filter_data *filter_instances[LVB_MAX_FILTER_INSTANCES];
static uint32_t next_instance_id = 1;
static lvb_atomic_uint32_t selected_instance_id;
static lvb_atomic_uint32_t published_instance_id;
/* Audio callbacks try this once; UI readers use it to copy a complete snapshot. */
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

static bool lvb_atomic_consume_if_equal(lvb_atomic_uint32_t *value, uint32_t expected)
{
	return (uint32_t)InterlockedCompareExchange(value, 0, (LONG)expected) == expected;
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

static bool lvb_atomic_consume_if_equal(lvb_atomic_uint32_t *value, uint32_t expected)
{
	uint32_t actual = expected;
	return atomic_compare_exchange_strong_explicit(value, &actual, 0U, memory_order_acq_rel, memory_order_acquire);
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

static enum lvb_mode mode_from_string(const char *mode)
{
	if (!mode)
		return LVB_MODE_WORSHIP;
	if (strcmp(mode, "sermon") == 0)
		return LVB_MODE_SERMON;
	if (strcmp(mode, "worship_acoustic") == 0)
		return LVB_MODE_WORSHIP_ACOUSTIC;
	if (strcmp(mode, "custom") == 0)
		return LVB_MODE_CUSTOM;
	if (strcmp(mode, "auto_assist") == 0)
		return LVB_MODE_AUTO_ASSIST;
	return LVB_MODE_WORSHIP;
}

static const char *trim_setting_for_mode(uint32_t mode)
{
	switch (mode) {
	case LVB_MODE_SERMON:
		return SETTING_TRIM_SERMON;
	case LVB_MODE_CUSTOM:
		return SETTING_TRIM_CUSTOM;
	case LVB_MODE_AUTO_ASSIST:
		return SETTING_TRIM_AUTO;
	case LVB_MODE_WORSHIP_ACOUSTIC:
		return SETTING_TRIM_ACOUSTIC;
	case LVB_MODE_WORSHIP:
	default:
		return SETTING_TRIM_WORSHIP;
	}
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
			if (lvb_atomic_load(&selected_instance_id) == 0U)
				lvb_atomic_store(&selected_instance_id, filter->instance_id);
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
		lvb_atomic_wait_lock(&published_snapshot_lock);
		lvb_atomic_store(&published_instance_id, 0U);
		lvb_atomic_release_lock(&published_snapshot_lock);
	}
	INSTANCES_UNLOCK();
}

static struct lvb_settings leveler_settings_snapshot(const struct leveler_filter_data *filter)
{
	struct lvb_settings settings = {
		.target_lufs = bits_float(lvb_atomic_load(&filter->target_lufs)),
		.max_boost_db = bits_float(lvb_atomic_load(&filter->max_boost_db)),
		.max_reduction_db = bits_float(lvb_atomic_load(&filter->max_reduction_db)),
		.attack_ms = bits_float(lvb_atomic_load(&filter->attack_ms)),
		.release_ms = bits_float(lvb_atomic_load(&filter->release_ms)),
		.noise_floor_db = bits_float(lvb_atomic_load(&filter->noise_floor_db)),
		.peak_ceiling_db = bits_float(lvb_atomic_load(&filter->peak_ceiling_db)),
		.mode = (enum lvb_mode)lvb_atomic_load(&filter->mode),
		.bypass = lvb_atomic_load(&filter->bypass) != 0,
	};
	for (size_t i = 0; i < LVB_MODE_COUNT; i++)
		settings.mode_trim_db[i] = bits_float(lvb_atomic_load(&filter->mode_trim_db[i]));
	return settings;
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
	lvb_atomic_store(&published_stats[0], float_bits(stats->momentary_lufs));
	lvb_atomic_store(&published_stats[1], float_bits(stats->short_term_lufs));
	lvb_atomic_store(&published_stats[2], float_bits(stats->true_peak_dbtp));
	lvb_atomic_store(&published_stats[3], float_bits(stats->gain_reduction_db));
	lvb_atomic_store(&published_stats[4], float_bits(stats->voice_activity));
	lvb_atomic_store(&published_stats[5], float_bits(stats->calibration_progress));
	lvb_atomic_store(&published_stats[6], float_bits(stats->calibration_suggestion_db));
	lvb_atomic_store(&published_stats[7], float_bits(stats->gain_db));
	lvb_atomic_store(&published_stats[8], float_bits(stats->calibration_measured_lufs));
	const uint32_t flags = (stats->calibration_active ? 1U : 0U) | (stats->calibration_ready ? 2U : 0U) |
			       (stats->voice_active ? 4U : 0U) | ((uint32_t)stats->mode << 8U) |
			       ((uint32_t)stats->calibration_mode << 16U);
	lvb_atomic_store(&published_flags, flags);
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
	if (found) {
		lvb_atomic_store(&selected_instance_id, instance_id);
		lvb_atomic_wait_lock(&published_snapshot_lock);
		lvb_atomic_store(&published_instance_id, 0U);
		lvb_atomic_release_lock(&published_snapshot_lock);
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
	snapshot->source_available = false;
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		if (filter_instances[i] && filter_instances[i]->instance_id == snapshot->source_instance_id) {
			snapshot->source_available = true;
			snapshot->calibration_waiting =
				lvb_atomic_load(&filter_instances[i]->calibration_start_requested) != 0U;
			snapshot->calibration_action_pending =
				lvb_atomic_load(&filter_instances[i]->calibration_action_pending) != 0U;
			break;
		}
	}
	INSTANCES_UNLOCK();
	snapshot->stats_available = snapshot->source_available && published.instance_id == snapshot->source_instance_id;
	if (snapshot->stats_available) {
		snapshot->stats.momentary_lufs = bits_float(published.stats[0]);
		snapshot->stats.short_term_lufs = bits_float(published.stats[1]);
		snapshot->stats.true_peak_dbtp = bits_float(published.stats[2]);
		snapshot->stats.gain_reduction_db = bits_float(published.stats[3]);
		snapshot->stats.voice_activity = bits_float(published.stats[4]);
		snapshot->stats.calibration_progress = bits_float(published.stats[5]);
		snapshot->stats.calibration_suggestion_db = bits_float(published.stats[6]);
		snapshot->stats.gain_db = bits_float(published.stats[7]);
		snapshot->stats.calibration_measured_lufs = bits_float(published.stats[8]);
		snapshot->stats.calibration_active = (published.flags & 1U) != 0;
		snapshot->stats.calibration_ready = (published.flags & 2U) != 0;
		snapshot->stats.voice_active = (published.flags & 4U) != 0;
		snapshot->stats.mode = (enum lvb_mode)((published.flags >> 8U) & 0xffU);
		snapshot->stats.calibration_mode = (enum lvb_mode)((published.flags >> 16U) & 0xffU);
	}
}

void lvb_monitor_start_calibration(void)
{
	const uint32_t instance_id = lvb_atomic_load(&selected_instance_id);
	if (instance_id == 0U)
		return;
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		struct leveler_filter_data *filter = filter_instances[i];
		if (filter && filter->instance_id == instance_id &&
		    lvb_atomic_load(&filter->calibration_action_pending) == 0U) {
			lvb_atomic_store(&filter->calibration_start_requested, 1U);
			break;
		}
	}
	INSTANCES_UNLOCK();
}

static obs_source_t *leveler_get_instance_source(uint32_t instance_id, enum lvb_mode *mode)
{
	obs_source_t *source = NULL;
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		struct leveler_filter_data *filter = filter_instances[i];
		if (filter && filter->instance_id == instance_id && filter->source) {
			source = obs_source_get_ref(filter->source);
			if (mode)
				*mode = (enum lvb_mode)lvb_atomic_load(&filter->mode);
			break;
		}
	}
	INSTANCES_UNLOCK();
	return source;
}

static obs_source_t *leveler_reserve_calibration_apply(uint32_t instance_id)
{
	obs_source_t *source = NULL;
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		struct leveler_filter_data *filter = filter_instances[i];
		if (lvb_atomic_load(&selected_instance_id) == instance_id && filter &&
		    filter->instance_id == instance_id && filter->source &&
		    lvb_atomic_set_if_zero(&filter->calibration_action_pending)) {
			source = obs_source_get_ref(filter->source);
			break;
		}
	}
	INSTANCES_UNLOCK();
	return source;
}

static void leveler_request_calibration_reset(uint32_t instance_id)
{
	INSTANCES_LOCK();
	for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
		struct leveler_filter_data *filter = filter_instances[i];
		if (filter && filter->instance_id == instance_id) {
			lvb_atomic_store(&filter->calibration_reset_requested, 1U);
			break;
		}
	}
	INSTANCES_UNLOCK();
}

void lvb_monitor_apply_calibration(void)
{
	struct lvb_published_snapshot published;
	read_published_snapshot(&published);
	const uint32_t selected_id = lvb_atomic_load(&selected_instance_id);
	const uint32_t mode = (published.flags >> 16U) & 0xffU;
	const float suggestion = bits_float(published.stats[6]);
	if (selected_id == 0U || published.instance_id != selected_id || (published.flags & 2U) == 0 ||
	    mode >= LVB_MODE_COUNT)
		return;
	obs_source_t *source = leveler_reserve_calibration_apply(selected_id);
	if (!source)
		return;
	if (!isfinite(suggestion)) {
		INSTANCES_LOCK();
		for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
			struct leveler_filter_data *filter = filter_instances[i];
			if (filter && filter->instance_id == selected_id) {
				lvb_atomic_store(&filter->calibration_action_pending, 0U);
				break;
			}
		}
		INSTANCES_UNLOCK();
		obs_source_release(source);
		return;
	}
	obs_data_t *settings = obs_source_get_settings(source);
	const char *key = trim_setting_for_mode(mode);
	const double current_trim = obs_data_get_double(settings, key);
	obs_data_set_double(settings, key, fmax(-12.0, fmin(12.0, current_trim + suggestion)));
	obs_source_update(source, settings);
	obs_data_release(settings);
	obs_source_release(source);
	leveler_request_calibration_reset(selected_id);
}

void lvb_monitor_reset_calibration(void)
{
	const uint32_t selected_id = lvb_atomic_load(&selected_instance_id);
	enum lvb_mode mode = LVB_MODE_WORSHIP;
	obs_source_t *source = leveler_get_instance_source(selected_id, &mode);
	if (mode < LVB_MODE_CUSTOM || mode >= LVB_MODE_COUNT)
		mode = LVB_MODE_WORSHIP;
	if (source) {
		INSTANCES_LOCK();
		for (size_t i = 0; i < LVB_MAX_FILTER_INSTANCES; i++) {
			struct leveler_filter_data *filter = filter_instances[i];
			if (filter && filter->instance_id == selected_id) {
				lvb_atomic_store(&filter->calibration_start_requested, 0U);
				lvb_atomic_store(&filter->calibration_action_pending, 1U);
				break;
			}
		}
		INSTANCES_UNLOCK();
		obs_data_t *settings = obs_source_get_settings(source);
		obs_data_set_double(settings, trim_setting_for_mode(mode), 0.0);
		obs_source_update(source, settings);
		obs_data_release(settings);
		obs_source_release(source);
		leveler_request_calibration_reset(selected_id);
	}
}

static const char *leveler_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LiveVolumeBalancer");
}

static void leveler_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, SETTING_BYPASS, false);
	obs_data_set_default_string(settings, SETTING_MODE, "worship");
	obs_data_set_default_double(settings, SETTING_TARGET_LUFS, -18.0);
	obs_data_set_default_double(settings, SETTING_MAX_BOOST, 12.0);
	obs_data_set_default_double(settings, SETTING_MAX_REDUCTION, 18.0);
	obs_data_set_default_int(settings, SETTING_ATTACK, 60);
	obs_data_set_default_int(settings, SETTING_RELEASE, 800);
	obs_data_set_default_double(settings, SETTING_NOISE_FLOOR, -60.0);
	obs_data_set_default_double(settings, SETTING_CEILING, -1.0);
	obs_data_set_default_double(settings, SETTING_TRIM_WORSHIP, 0.0);
	obs_data_set_default_double(settings, SETTING_TRIM_SERMON, 0.0);
	obs_data_set_default_double(settings, SETTING_TRIM_CUSTOM, 0.0);
	obs_data_set_default_double(settings, SETTING_TRIM_AUTO, 0.0);
	obs_data_set_default_double(settings, SETTING_TRIM_ACOUSTIC, 0.0);
}

static void leveler_update(void *opaque, obs_data_t *settings)
{
	struct leveler_filter_data *filter = opaque;
	if (!filter || !settings)
		return;
	lvb_atomic_store(&filter->bypass, obs_data_get_bool(settings, SETTING_BYPASS) ? 1U : 0U);
	lvb_atomic_store(&filter->mode, (uint32_t)mode_from_string(obs_data_get_string(settings, SETTING_MODE)));
	lvb_atomic_store(&filter->target_lufs, float_bits((float)obs_data_get_double(settings, SETTING_TARGET_LUFS)));
	lvb_atomic_store(&filter->max_boost_db, float_bits((float)obs_data_get_double(settings, SETTING_MAX_BOOST)));
	lvb_atomic_store(&filter->max_reduction_db,
			 float_bits((float)obs_data_get_double(settings, SETTING_MAX_REDUCTION)));
	lvb_atomic_store(&filter->attack_ms, float_bits((float)obs_data_get_int(settings, SETTING_ATTACK)));
	lvb_atomic_store(&filter->release_ms, float_bits((float)obs_data_get_int(settings, SETTING_RELEASE)));
	lvb_atomic_store(&filter->noise_floor_db,
			 float_bits((float)obs_data_get_double(settings, SETTING_NOISE_FLOOR)));
	lvb_atomic_store(&filter->peak_ceiling_db, float_bits((float)obs_data_get_double(settings, SETTING_CEILING)));
	lvb_atomic_store(&filter->mode_trim_db[LVB_MODE_WORSHIP],
			 float_bits((float)obs_data_get_double(settings, SETTING_TRIM_WORSHIP)));
	lvb_atomic_store(&filter->mode_trim_db[LVB_MODE_SERMON],
			 float_bits((float)obs_data_get_double(settings, SETTING_TRIM_SERMON)));
	lvb_atomic_store(&filter->mode_trim_db[LVB_MODE_CUSTOM],
			 float_bits((float)obs_data_get_double(settings, SETTING_TRIM_CUSTOM)));
	lvb_atomic_store(&filter->mode_trim_db[LVB_MODE_AUTO_ASSIST],
			 float_bits((float)obs_data_get_double(settings, SETTING_TRIM_AUTO)));
	lvb_atomic_store(&filter->mode_trim_db[LVB_MODE_WORSHIP_ACOUSTIC],
			 float_bits((float)obs_data_get_double(settings, SETTING_TRIM_ACOUSTIC)));
}

static void *leveler_create(obs_data_t *settings, obs_source_t *source)
{
	struct leveler_filter_data *filter = bzalloc(sizeof(*filter));
	if (!filter)
		return NULL;
	lvb_state_init(&filter->state);
	lvb_atomic_init(&filter->bypass, 0U);
	lvb_atomic_init(&filter->mode, LVB_MODE_WORSHIP);
	lvb_atomic_init(&filter->target_lufs, float_bits(-18.0f));
	lvb_atomic_init(&filter->max_boost_db, float_bits(12.0f));
	lvb_atomic_init(&filter->max_reduction_db, float_bits(18.0f));
	lvb_atomic_init(&filter->attack_ms, float_bits(60.0f));
	lvb_atomic_init(&filter->release_ms, float_bits(800.0f));
	lvb_atomic_init(&filter->noise_floor_db, float_bits(-60.0f));
	lvb_atomic_init(&filter->peak_ceiling_db, float_bits(-1.0f));
	for (size_t i = 0; i < LVB_MODE_COUNT; i++)
		lvb_atomic_init(&filter->mode_trim_db[i], float_bits(0.0f));
	lvb_atomic_init(&filter->calibration_start_requested, 0U);
	lvb_atomic_init(&filter->calibration_reset_requested, 0U);
	lvb_atomic_init(&filter->calibration_action_pending, 0U);

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
	if (filter->instance_id != 0U) {
		if (lvb_atomic_consume_if_equal(&filter->calibration_reset_requested, 1U)) {
			lvb_calibration_reset(&filter->state);
			lvb_atomic_store(&filter->calibration_action_pending, 0U);
		}
		if (lvb_atomic_consume_if_equal(&filter->calibration_start_requested, 1U))
			lvb_calibration_start(&filter->state, &settings);
	}

	float *planes[MAX_AV_PLANES] = {0};
	for (size_t channel = 0; channel < filter->channels; channel++)
		planes[channel] = (float *)audio->data[channel];

	lvb_process(&filter->state, &settings, filter->channels, planes, audio->frames, filter->sample_rate);
	struct lvb_stats stats;
	lvb_get_stats(&filter->state, &stats);
	publish_stats(filter->instance_id, &stats);
	return audio;
}

static obs_properties_t *leveler_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *property;
	obs_properties_add_bool(properties, SETTING_BYPASS, obs_module_text("Bypass"));

	property = obs_properties_add_list(properties, SETTING_MODE, obs_module_text("OperatingMode"),
					   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(property, obs_module_text("ModeWorship"), "worship");
	obs_property_list_add_string(property, obs_module_text("ModeWorshipAcoustic"), "worship_acoustic");
	obs_property_list_add_string(property, obs_module_text("ModeSermon"), "sermon");
	obs_property_list_add_string(property, obs_module_text("ModeCustom"), "custom");
	obs_property_list_add_string(property, obs_module_text("ModeAutoAssist"), "auto_assist");

	property = obs_properties_add_float_slider(properties, SETTING_TARGET_LUFS, obs_module_text("CustomTarget"),
						   -30.0, -9.0, 0.5);
	obs_property_float_set_suffix(property, " LUFS");
	property = obs_properties_add_float_slider(properties, SETTING_MAX_BOOST, obs_module_text("MaximumBoost"), 0.0,
						   18.0, 0.5);
	obs_property_float_set_suffix(property, " dB");
	property = obs_properties_add_float_slider(properties, SETTING_MAX_REDUCTION,
						   obs_module_text("MaximumReduction"), 0.0, 24.0, 0.5);
	obs_property_float_set_suffix(property, " dB");
	property = obs_properties_add_int_slider(properties, SETTING_ATTACK, obs_module_text("Attack"), 20, 1000, 10);
	obs_property_int_set_suffix(property, " ms");
	property =
		obs_properties_add_int_slider(properties, SETTING_RELEASE, obs_module_text("Release"), 100, 5000, 50);
	obs_property_int_set_suffix(property, " ms");
	property = obs_properties_add_float_slider(properties, SETTING_NOISE_FLOOR, obs_module_text("NoiseFloor"),
						   -80.0, -24.0, 1.0);
	obs_property_float_set_suffix(property, " dBFS");
	property = obs_properties_add_float_slider(properties, SETTING_CEILING, obs_module_text("PeakCeiling"), -12.0,
						   0.0, 0.1);
	obs_property_float_set_suffix(property, " dBTP est.");
	obs_properties_add_text(properties, "mode_note", obs_module_text("ModeNote"), OBS_TEXT_INFO);
	obs_properties_add_text(properties, "dock_note", obs_module_text("DockNote"), OBS_TEXT_INFO);
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
