#ifndef SM_MOUNT_CACHE_H
#define SM_MOUNT_CACHE_H

#include <stdbool.h>

#include "sm_mount_profile.h"
#include "sm_types.h"

// Lookup a cached/autotuned mount profile by image filename
// Returns true if found and populates profile_out
bool get_cached_mount_profile(const char *image_filename,
                              mount_profile_t *profile_out);

// Upsert a winning mount profile into autotune.ini by image filename
// Returns true if successfully cached
bool cache_mount_profile(const char *image_filename,
                         const mount_profile_t *profile);

// Format a mount profile into a compact INI-friendly string
// for storage in autotune.ini
void format_profile_for_cache(const mount_profile_t *profile,
                              char *buf, size_t buf_size);

// Parse a cached profile string back from autotune.ini
bool parse_profile_from_cache(const char *cached_str,
                              mount_profile_t *profile_out);

#endif
