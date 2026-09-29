// src/reporter-statemachine.h - State machine for Kanami Reporter filter.

#pragma once

#include <stdint.h>

void reporter_state_reset(void);

void reporter_state_process_frame(void *argument);

const char *reporter_state_get_current(void);

void reporter_state_set_target_score(int target);
