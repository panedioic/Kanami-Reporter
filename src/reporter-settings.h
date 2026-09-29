// src/reporter-settings.h - Settings management for Kanami Reporter filter.

#pragma once

#include <obs-module.h>

#include "reporter-types.h"

int reporter_setting_int(obs_data_t *settings, const char *name,
			 int minimum, int maximum);

/*
 * 同步“当前事件”的ROI：
 * 切换事件时保存旧值并恢复该事件自己的坐标。
 */
void reporter_settings_sync_event_roi(obs_data_t *settings,
				      int *selected_cue,
				      struct reporter_roi *selected_roi);

bool reporter_settings_event_changed(obs_properties_t *props,
				     obs_property_t *property,
				     obs_data_t *settings);
