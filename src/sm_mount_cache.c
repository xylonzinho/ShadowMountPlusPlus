#include "sm_mount_cache.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm_hash.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_mount_defs.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

// Autotune file path: /data/shadowmount/autotune.ini
#define AUTOTUNE_INI_PATH "/data/shadowmount/autotune.ini"

// Line buffer for parsing
#define AUTOTUNE_LINE_SIZE 512
#define AUTOTUNE_MAX_ENTRIES 256

typedef struct {
  char filename[MAX_PATH];
  char profile_str[256];
} autotune_cache_entry_t;

// In-memory cache of parsed autotune entries
static autotune_cache_entry_t g_autotune_cache[AUTOTUNE_MAX_ENTRIES];
static int g_autotune_cache_count = 0;
static bool g_autotune_cache_loaded = false;

// Load autotune.ini into memory
static bool load_autotune_cache(void) {
  if (g_autotune_cache_loaded)
    return true;

  g_autotune_cache_count = 0;
  memset(g_autotune_cache, 0, sizeof(g_autotune_cache));

  FILE *fp = fopen(AUTOTUNE_INI_PATH, "r");
  if (!fp) {
    // File may not exist yet, that's OK
    g_autotune_cache_loaded = true;
    return true;
  }

  char line[AUTOTUNE_LINE_SIZE];
  while (fgets(line, sizeof(line), fp) != NULL && g_autotune_cache_count < AUTOTUNE_MAX_ENTRIES) {
    // Trim trailing newline
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
      line[len - 1] = '\0';

    // Skip empty lines and comments
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
      continue;

    // Look for mount_profile=<filename>:<profile_str>
    const char *prefix = "mount_profile=";
    if (strncmp(line, prefix, strlen(prefix)) != 0)
      continue;

    const char *value = line + strlen(prefix);
    const char *colon = strchr(value, ':');
    if (!colon)
      continue;

    size_t filename_len = (size_t)(colon - value);
    if (filename_len == 0 || filename_len >= MAX_PATH)
      continue;

    const char *profile_str = colon + 1;
    if (!profile_str || profile_str[0] == '\0')
      continue;

    // Store in cache
    memcpy(g_autotune_cache[g_autotune_cache_count].filename, value, filename_len);
    g_autotune_cache[g_autotune_cache_count].filename[filename_len] = '\0';
    (void)strlcpy(g_autotune_cache[g_autotune_cache_count].profile_str, profile_str,
                  sizeof(g_autotune_cache[g_autotune_cache_count].profile_str));
    g_autotune_cache_count++;
  }

  fclose(fp);
  g_autotune_cache_loaded = true;
  return true;
}

bool get_cached_mount_profile(const char *image_filename,
                              mount_profile_t *profile_out) {
  if (!image_filename || !profile_out)
    return false;

  if (!load_autotune_cache())
    return false;

  // Search cache for matching filename
  for (int i = 0; i < g_autotune_cache_count; i++) {
    if (strcmp(g_autotune_cache[i].filename, image_filename) == 0) {
      if (parse_profile_from_cache(g_autotune_cache[i].profile_str, profile_out)) {
        log_debug("  [IMG][CACHE] found cached profile for %s", image_filename);
        return true;
      }
      break;
    }
  }

  return false;
}

void format_profile_for_cache(const mount_profile_t *profile,
                              char *buf, size_t buf_size) {
  if (!profile || !buf || buf_size == 0)
    return;

  snprintf(buf, buf_size,
           "v2:%u:0x%x:0x%x:%u:%u:%s:%s:%s:%u:%u:%u:%u:%u:%d",
           profile->image_type, profile->raw_flags,
           profile->normalized_flags, profile->sector_size,
           profile->secondary_unit, profile->fstype, profile->budgetid,
           profile->mkeymode, profile->sigverify, profile->playgo,
           profile->disc, profile->include_ekpfs ? 1u : 0u,
           profile->supports_noatime ? 1u : 0u,
           profile->mount_read_only ? 1 : 0);
}

bool parse_profile_from_cache(const char *cached_str,
                              mount_profile_t *profile_out) {
  if (!cached_str || !profile_out)
    return false;

  char buf[256];
  (void)strlcpy(buf, cached_str, sizeof(buf));

  memset(profile_out, 0, sizeof(*profile_out));

  // Parse format:
  // v2:image_type:raw_flags:norm_flags:sector_size:sec2:fstype:budgetid:mkeymode:sig:playgo:disc:ekpfs:noatime:ro
  // v1:image_type:raw_flags:norm_flags:sector_size:sec2:fstype:budgetid:mkeymode:sig:playgo:disc:ekpfs:ro
  // v0:image_type:raw_flags:raw_flags:norm_flags:sector_size:fstype:budgetid:mkeymode:sig:playgo:disc:ro
  char *saveptr = NULL;
  char *token = NULL;

  // Version
  token = strtok_r(buf, ":", &saveptr);
  if (!token)
    return false;

  bool is_v2 = (strcmp(token, "v2") == 0);
  bool is_v1 = (strcmp(token, "v1") == 0);
  bool is_v0 = (strcmp(token, "v0") == 0);
  if (!is_v2 && !is_v1 && !is_v0)
    return false;

  // image_type
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  profile_out->image_type = (uint16_t)strtoul(token, NULL, 10);

  if (is_v2 || is_v1) {
    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->raw_flags = (uint16_t)strtoul(token, NULL, 0);

    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->normalized_flags = (uint16_t)strtoul(token, NULL, 0);

    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->sector_size = (uint32_t)strtoul(token, NULL, 10);

    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->secondary_unit = (uint32_t)strtoul(token, NULL, 10);
  } else {
    // raw_flags
    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->raw_flags = (uint16_t)strtoul(token, NULL, 10);

    // Skip stored raw_flags (redundant)
    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;

    // normalized_flags
    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->normalized_flags = (uint16_t)strtoul(token, NULL, 16);

    // sector_size
    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->sector_size = (uint32_t)strtoul(token, NULL, 10);
    profile_out->secondary_unit = profile_out->sector_size;
  }

  // fstype
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  // Allocate static string (short-lived in this context)
  if (strcmp(token, "pfs") == 0) {
    profile_out->fstype = "pfs";
  } else if (strcmp(token, "ppr_pfs") == 0) {
    profile_out->fstype = "ppr_pfs";
  } else if (strcmp(token, "transaction_pfs") == 0) {
    profile_out->fstype = "transaction_pfs";
  } else {
    return false;
  }

  // budgetid
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  if (strcmp(token, DEVPFS_BUDGET_GAME) == 0) {
    profile_out->budgetid = DEVPFS_BUDGET_GAME;
  } else if (strcmp(token, DEVPFS_BUDGET_SYSTEM) == 0) {
    profile_out->budgetid = DEVPFS_BUDGET_SYSTEM;
  } else {
    return false;
  }

  // mkeymode
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  if (strcmp(token, DEVPFS_MKEYMODE_SD) == 0) {
    profile_out->mkeymode = DEVPFS_MKEYMODE_SD;
  } else if (strcmp(token, DEVPFS_MKEYMODE_GD) == 0) {
    profile_out->mkeymode = DEVPFS_MKEYMODE_GD;
  } else if (strcmp(token, DEVPFS_MKEYMODE_AC) == 0) {
    profile_out->mkeymode = DEVPFS_MKEYMODE_AC;
  } else {
    return false;
  }

  // sigverify
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  profile_out->sigverify = (uint8_t)strtoul(token, NULL, 10);

  // playgo
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  profile_out->playgo = (uint8_t)strtoul(token, NULL, 10);

  // disc
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  profile_out->disc = (uint8_t)strtoul(token, NULL, 10);

  if (is_v2 || is_v1) {
    token = strtok_r(NULL, ":", &saveptr);
    if (!token)
      return false;
    profile_out->include_ekpfs = ((uint32_t)strtoul(token, NULL, 10) != 0);

    if (is_v2) {
      token = strtok_r(NULL, ":", &saveptr);
      if (!token)
        return false;
      profile_out->supports_noatime =
          ((uint32_t)strtoul(token, NULL, 10) != 0);
    } else {
      profile_out->supports_noatime = true;
    }
  } else {
    profile_out->include_ekpfs = true;
    profile_out->supports_noatime = true;
  }

  // mount_read_only
  token = strtok_r(NULL, ":", &saveptr);
  if (!token)
    return false;
  profile_out->mount_read_only = ((uint32_t)strtoul(token, NULL, 10) != 0);

  profile_out->io_version = LVD_ATTACH_IO_VERSION_V0;
  profile_out->label = "cached";

  return true;
}

static bool upsert_cache_profile_line(const char *image_filename,
                                     const char *profile_str) {
  char temp_path[MAX_PATH];
  int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", AUTOTUNE_INI_PATH);
  if (written <= 0 || (size_t)written >= sizeof(temp_path))
    return false;

  FILE *in = fopen(AUTOTUNE_INI_PATH, "r");
  FILE *out = fopen(temp_path, "w");
  if (!out) {
    log_debug("  [IMG][CACHE] temp open failed: %s (%s)", temp_path,
              strerror(errno));
    if (in)
      fclose(in);
    return false;
  }

  const char *prefix = "mount_profile=";
  bool replaced = false;
  if (in) {
    char line[AUTOTUNE_LINE_SIZE];
    while (fgets(line, sizeof(line), in) != NULL) {
      char original[AUTOTUNE_LINE_SIZE];
      (void)strlcpy(original, line, sizeof(original));

      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n')
        line[len - 1] = '\0';

      bool is_target = false;
      if (strncmp(line, prefix, strlen(prefix)) == 0) {
        const char *value = line + strlen(prefix);
        const char *colon = strchr(value, ':');
        if (colon) {
          size_t filename_len = (size_t)(colon - value);
          if (filename_len == strlen(image_filename) &&
              strncmp(value, image_filename, filename_len) == 0) {
            is_target = true;
          }
        }
      }

      if (is_target) {
        if (!replaced) {
          if (fprintf(out, "mount_profile=%s:%s\n", image_filename,
                      profile_str) < 0) {
            goto write_failed;
          }
          replaced = true;
        }
      } else {
        if (fputs(original, out) == EOF)
          goto write_failed;
      }
    }

    fclose(in);
    in = NULL;
  }

  if (!replaced &&
      fprintf(out, "mount_profile=%s:%s\n", image_filename, profile_str) < 0) {
    goto write_failed;
  }

  if (fclose(out) != 0) {
    out = NULL;
    unlink(temp_path);
    return false;
  }
  out = NULL;

  if (rename(temp_path, AUTOTUNE_INI_PATH) != 0) {
    unlink(temp_path);
    return false;
  }

  return true;

write_failed:
  if (in)
    fclose(in);
  fclose(out);
  unlink(temp_path);
  return false;
}

bool cache_mount_profile(const char *image_filename,
                         const mount_profile_t *profile) {
  if (!image_filename || !profile)
    return false;

  if (!load_autotune_cache())
    return false;

  // Check if already cached
  int existing_index = -1;
  for (int i = 0; i < g_autotune_cache_count; i++) {
    if (strcmp(g_autotune_cache[i].filename, image_filename) == 0) {
      existing_index = i;
      break;
    }
  }

  // Format profile for caching
  char profile_str[256];
  format_profile_for_cache(profile, profile_str, sizeof(profile_str));

  if (!upsert_cache_profile_line(image_filename, profile_str)) {
    mkdir("/data/shadowmount", 0777);
    if (!upsert_cache_profile_line(image_filename, profile_str)) {
      log_debug("  [IMG][CACHE] failed to upsert %s", AUTOTUNE_INI_PATH);
      return false;
    }
  }

  if (existing_index >= 0) {
    (void)strlcpy(g_autotune_cache[existing_index].profile_str, profile_str,
                  sizeof(g_autotune_cache[existing_index].profile_str));
  } else if (g_autotune_cache_count < AUTOTUNE_MAX_ENTRIES) {
    (void)strlcpy(g_autotune_cache[g_autotune_cache_count].filename,
                  image_filename,
                  sizeof(g_autotune_cache[g_autotune_cache_count].filename));
    (void)strlcpy(g_autotune_cache[g_autotune_cache_count].profile_str,
                  profile_str,
                  sizeof(g_autotune_cache[g_autotune_cache_count].profile_str));
    g_autotune_cache_count++;
  }

  log_debug("  [IMG][CACHE] cached profile for %s: %s", image_filename, profile_str);
  return true;
}
