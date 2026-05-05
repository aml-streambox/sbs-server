/*
 * SBS - StreamBox Broadcast System
 * UUID generation header
 */
#ifndef SBS_UUID_H
#define SBS_UUID_H

#include "sbs/types.h"

/**
 * Generate a UUID v4 string into `out`.
 * The buffer must be at least SBS_UUID_STRING_LEN bytes.
 * Uses /dev/urandom for randomness.
 */
void sbs_uuid_generate(sbs_uuid_t out);

/**
 * Validate a UUID string format (xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx).
 * Returns true if valid.
 */
bool sbs_uuid_validate(const char *uuid);

#endif /* SBS_UUID_H */
