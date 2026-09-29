// src/reporter-properties.h - Property definitions for Kanami Reporter filter.

#pragma once

#include <obs-module.h>

void reporter_defaults(obs_data_t *settings);

obs_properties_t *reporter_properties(void *private_data);