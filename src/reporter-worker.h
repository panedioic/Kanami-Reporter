// src/reporter-worker.h - Worker thread for Kanami Reporter filter.

#pragma once

#include "reporter-context.h"

unsigned int __stdcall reporter_worker(void *argument);
