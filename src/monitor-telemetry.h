/*
 * Live Volume Balancer per-filter telemetry
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "leveler-dsp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LVB_TELEMETRY_MAX_SLOTS 64

struct lvb_telemetry_snapshot {
	struct lvb_stats stats;
	uint32_t instance_id;
	uint32_t sequence;
	bool available;
	bool stats_available;
};

#ifdef __cplusplus
extern "C" {
#endif

void lvb_telemetry_init(void);
bool lvb_telemetry_register_slot(size_t slot, uint32_t instance_id);
bool lvb_telemetry_unregister_slot(size_t slot, uint32_t instance_id);
bool lvb_telemetry_publish(size_t slot, uint32_t instance_id, const struct lvb_stats *stats);
bool lvb_telemetry_read(size_t slot, uint32_t instance_id, struct lvb_telemetry_snapshot *snapshot);

#ifdef __cplusplus
}
#endif
