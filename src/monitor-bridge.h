/*
 * Live Volume Balancer frontend bridge
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "leveler-dsp.h"

#define LVB_MONITOR_MAX_SOURCES 64
#define LVB_MONITOR_SOURCE_NAME_LENGTH 128

#ifdef __cplusplus
extern "C" {
#endif

struct lvb_monitor_snapshot {
	struct lvb_stats stats;
	uint32_t source_instance_id;
	uint32_t sequence;
	bool source_available;
	bool stats_available;
};

struct lvb_monitor_source {
	uint32_t instance_id;
	size_t telemetry_slot;
	char name[LVB_MONITOR_SOURCE_NAME_LENGTH];
};

void lvb_monitor_read_source(size_t telemetry_slot, uint32_t instance_id, struct lvb_monitor_snapshot *snapshot);
size_t lvb_monitor_list_sources(struct lvb_monitor_source *sources, size_t capacity);
bool lvb_monitor_open_source_properties(uint32_t instance_id);

void lvb_monitor_dock_load(void);
void lvb_monitor_dock_unload(void);

#ifdef __cplusplus
}
#endif
