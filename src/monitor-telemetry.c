/*
 * Live Volume Balancer per-filter telemetry
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "monitor-telemetry.h"

#include <string.h>

#ifdef _MSC_VER
#include <windows.h>
typedef volatile LONG lvb_atomic_uint32_t;
#else
#include <stdatomic.h>
typedef _Atomic(uint32_t) lvb_atomic_uint32_t;
#endif

#define LVB_TELEMETRY_STATS_COUNT 19

struct lvb_telemetry_slot {
	lvb_atomic_uint32_t owner_id;
	lvb_atomic_uint32_t lock;
	lvb_atomic_uint32_t sequence;
	lvb_atomic_uint32_t flags;
	lvb_atomic_uint32_t stats[LVB_TELEMETRY_STATS_COUNT];
};

static struct lvb_telemetry_slot telemetry_slots[LVB_TELEMETRY_MAX_SLOTS];

#ifdef _MSC_VER
static void atomic_init_u32(lvb_atomic_uint32_t *value, uint32_t initial)
{
	*value = (LONG)initial;
}

static void atomic_store_u32(lvb_atomic_uint32_t *value, uint32_t next)
{
	InterlockedExchange(value, (LONG)next);
}

static uint32_t atomic_load_u32(const lvb_atomic_uint32_t *value)
{
	return (uint32_t)InterlockedCompareExchange((volatile LONG *)value, 0, 0);
}

static bool atomic_try_lock(lvb_atomic_uint32_t *value)
{
	return InterlockedCompareExchange(value, 1, 0) == 0;
}
#else
static void atomic_init_u32(lvb_atomic_uint32_t *value, uint32_t initial)
{
	atomic_init(value, initial);
}

static void atomic_store_u32(lvb_atomic_uint32_t *value, uint32_t next)
{
	atomic_store_explicit(value, next, memory_order_release);
}

static uint32_t atomic_load_u32(const lvb_atomic_uint32_t *value)
{
	return atomic_load_explicit(value, memory_order_acquire);
}

static bool atomic_try_lock(lvb_atomic_uint32_t *value)
{
	uint32_t expected = 0U;
	return atomic_compare_exchange_strong_explicit(value, &expected, 1U, memory_order_acquire,
						       memory_order_relaxed);
}
#endif

static void atomic_wait_lock(lvb_atomic_uint32_t *value)
{
	while (!atomic_try_lock(value)) {
#ifdef _MSC_VER
		YieldProcessor();
#else
		atomic_signal_fence(memory_order_acq_rel);
#endif
	}
}

static void atomic_release_lock(lvb_atomic_uint32_t *value)
{
	atomic_store_u32(value, 0U);
}

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

static void clear_slot_values(struct lvb_telemetry_slot *slot)
{
	atomic_store_u32(&slot->sequence, 0U);
	atomic_store_u32(&slot->flags, 0U);
	for (size_t i = 0; i < LVB_TELEMETRY_STATS_COUNT; i++)
		atomic_store_u32(&slot->stats[i], float_bits(-120.0f));
	atomic_store_u32(&slot->stats[11], float_bits(0.0f));
}

void lvb_telemetry_init(void)
{
	for (size_t slot_index = 0; slot_index < LVB_TELEMETRY_MAX_SLOTS; slot_index++) {
		struct lvb_telemetry_slot *slot = &telemetry_slots[slot_index];
		atomic_init_u32(&slot->owner_id, 0U);
		atomic_init_u32(&slot->lock, 0U);
		atomic_init_u32(&slot->sequence, 0U);
		atomic_init_u32(&slot->flags, 0U);
		for (size_t i = 0; i < LVB_TELEMETRY_STATS_COUNT; i++)
			atomic_init_u32(&slot->stats[i], float_bits(-120.0f));
		atomic_store_u32(&slot->stats[11], float_bits(0.0f));
	}
}

bool lvb_telemetry_register_slot(size_t slot_index, uint32_t instance_id)
{
	if (slot_index >= LVB_TELEMETRY_MAX_SLOTS || instance_id == 0U)
		return false;

	struct lvb_telemetry_slot *slot = &telemetry_slots[slot_index];
	atomic_wait_lock(&slot->lock);
	atomic_store_u32(&slot->owner_id, 0U);
	clear_slot_values(slot);
	atomic_store_u32(&slot->owner_id, instance_id);
	atomic_release_lock(&slot->lock);
	return true;
}

bool lvb_telemetry_unregister_slot(size_t slot_index, uint32_t instance_id)
{
	if (slot_index >= LVB_TELEMETRY_MAX_SLOTS || instance_id == 0U)
		return false;

	struct lvb_telemetry_slot *slot = &telemetry_slots[slot_index];
	atomic_wait_lock(&slot->lock);
	const bool matches = atomic_load_u32(&slot->owner_id) == instance_id;
	if (matches) {
		atomic_store_u32(&slot->owner_id, 0U);
		clear_slot_values(slot);
	}
	atomic_release_lock(&slot->lock);
	return matches;
}

bool lvb_telemetry_publish(size_t slot_index, uint32_t instance_id, const struct lvb_stats *stats)
{
	if (slot_index >= LVB_TELEMETRY_MAX_SLOTS || instance_id == 0U || !stats)
		return false;

	struct lvb_telemetry_slot *slot = &telemetry_slots[slot_index];
	if (atomic_load_u32(&slot->owner_id) != instance_id || !atomic_try_lock(&slot->lock))
		return false;
	if (atomic_load_u32(&slot->owner_id) != instance_id) {
		atomic_release_lock(&slot->lock);
		return false;
	}

	const float values[LVB_TELEMETRY_STATS_COUNT] = {
		stats->input_momentary_lufs,
		stats->input_short_term_lufs,
		stats->output_momentary_lufs,
		stats->output_short_term_lufs,
		stats->true_peak_dbtp,
		stats->peak_hold_dbtp,
		stats->gain_db,
		stats->target_lufs,
		stats->peak_ceiling_dbtp,
		stats->input_fast_rms_dbfs,
		stats->output_fast_rms_dbfs,
		stats->sample_rate_hz,
		stats->max_boost_db,
		stats->max_reduction_db,
		stats->noise_floor_dbfs,
		stats->attack_ms,
		stats->recovery_ms,
		stats->fader_smoothness,
		stats->quiet_attenuation_db,
	};
	for (size_t i = 0; i < LVB_TELEMETRY_STATS_COUNT; i++)
		atomic_store_u32(&slot->stats[i], float_bits(values[i]));
	const uint32_t flags = (stats->activity_open ? 1U : 0U) | (stats->bypass ? 2U : 0U);
	atomic_store_u32(&slot->flags, flags);
	uint32_t sequence = atomic_load_u32(&slot->sequence) + 1U;
	if (sequence == 0U)
		sequence = 1U;
	atomic_store_u32(&slot->sequence, sequence);
	atomic_release_lock(&slot->lock);
	return true;
}

bool lvb_telemetry_read(size_t slot_index, uint32_t instance_id, struct lvb_telemetry_snapshot *snapshot)
{
	if (slot_index >= LVB_TELEMETRY_MAX_SLOTS || instance_id == 0U || !snapshot)
		return false;

	memset(snapshot, 0, sizeof(*snapshot));
	struct lvb_telemetry_slot *slot = &telemetry_slots[slot_index];
	if (atomic_load_u32(&slot->owner_id) != instance_id)
		return false;

	atomic_wait_lock(&slot->lock);
	if (atomic_load_u32(&slot->owner_id) != instance_id) {
		atomic_release_lock(&slot->lock);
		return false;
	}

	snapshot->instance_id = instance_id;
	snapshot->sequence = atomic_load_u32(&slot->sequence);
	snapshot->available = true;
	snapshot->stats_available = snapshot->sequence != 0U;
	float *values[] = {
		&snapshot->stats.input_momentary_lufs,
		&snapshot->stats.input_short_term_lufs,
		&snapshot->stats.output_momentary_lufs,
		&snapshot->stats.output_short_term_lufs,
		&snapshot->stats.true_peak_dbtp,
		&snapshot->stats.peak_hold_dbtp,
		&snapshot->stats.gain_db,
		&snapshot->stats.target_lufs,
		&snapshot->stats.peak_ceiling_dbtp,
		&snapshot->stats.input_fast_rms_dbfs,
		&snapshot->stats.output_fast_rms_dbfs,
		&snapshot->stats.sample_rate_hz,
		&snapshot->stats.max_boost_db,
		&snapshot->stats.max_reduction_db,
		&snapshot->stats.noise_floor_dbfs,
		&snapshot->stats.attack_ms,
		&snapshot->stats.recovery_ms,
		&snapshot->stats.fader_smoothness,
		&snapshot->stats.quiet_attenuation_db,
	};
	for (size_t i = 0; i < LVB_TELEMETRY_STATS_COUNT; i++)
		*values[i] = bits_float(atomic_load_u32(&slot->stats[i]));
	const uint32_t flags = atomic_load_u32(&slot->flags);
	snapshot->stats.activity_open = (flags & 1U) != 0U;
	snapshot->stats.bypass = (flags & 2U) != 0U;
	atomic_release_lock(&slot->lock);
	return true;
}
