// src/reporter-template.h - Template recognition and management for Kanami Reporter filter.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "reporter-types.h"

bool reporter_valid_roi(struct reporter_roi roi);

char *reporter_template_path(const char *dir, int state_id);

void reporter_template_free(struct template_data *model);

bool reporter_template_save(const char *path,
			    const struct template_data *model);

bool reporter_template_load(const char *path,
			    struct template_data *model);

void reporter_to_grayscale(const uint8_t *bgra, uint8_t *gray);

double reporter_template_score(const struct template_data *model,
			       const uint8_t *gray);
                
void reporter_save_roi_preview(const struct frame_data *frame,
			       struct reporter_roi roi);
