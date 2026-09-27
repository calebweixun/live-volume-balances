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
	bool source_available;
	bool stats_available;
	bool calibration_waiting;
	bool calibration_action_pending;
};

struct lvb_monitor_source {
	uint32_t instance_id;
	char name[LVB_MONITOR_SOURCE_NAME_LENGTH];
};

void lvb_monitor_read(struct lvb_monitor_snapshot *snapshot);
size_t lvb_monitor_list_sources(struct lvb_monitor_source *sources, size_t capacity);
void lvb_monitor_select_source(uint32_t instance_id);
void lvb_monitor_start_calibration(void);
void lvb_monitor_apply_calibration(void);
void lvb_monitor_reset_calibration(void);

void lvb_monitor_dock_load(void);
void lvb_monitor_dock_unload(void);

#ifdef __cplusplus
}
#endif
