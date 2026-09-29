// src/reporter-capture.h - Frame capture and processing for Kanami Reporter filter.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "reporter-context.h"

void reporter_capture_frame(struct kanami_reporter_data *data,
			    uint64_t now,
			    bool enabled,
			    double threshold);
