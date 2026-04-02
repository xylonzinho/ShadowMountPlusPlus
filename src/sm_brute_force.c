#include "sm_brute_force.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sm_log.h"
#include "sm_mount_defs.h"

// Stage A: Fast-track candidates (high probability)
static const uint16_t STAGE_A_IMAGE_TYPES[] = {0, 5, 7};
static const uint16_t STAGE_A_RAW_FLAGS[] = {0x9, 0x8};
static const uint32_t STAGE_A_SECTOR_SIZES[] = {4096, 32768};
static const char *STAGE_A_FSTYPES[] = {"pfs", "ppr_pfs"};

// Stage B: Expanded candidates (when Stage A fails)
static const uint16_t STAGE_B_IMAGE_TYPES[] = {1, 2, 3, 4, 6, 8, 9, 10, 11, 12, 0xA, 0xB, 0xC};
static const uint16_t STAGE_B_RAW_FLAGS[] = {0xD, 0xC};
static const uint32_t STAGE_B_SECTOR_SIZES[] = {65536, 16384, 8192};
static const char *STAGE_B_FSTYPES[] = {"transaction_pfs"};

#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof(arr[0]))

void brute_attempt_state_init(brute_attempt_state_t *state,
                              uint32_t max_attempts,
                              uint32_t max_seconds_per_image) {
  if (!state)
    return;
  memset(state, 0, sizeof(*state));
  state->max_attempts = max_attempts > 0 ? max_attempts : 20;
  state->max_seconds = max_seconds_per_image > 0 ? max_seconds_per_image : 60;
  state->start_time = time(NULL);
  state->should_stop = false;
}

bool brute_should_continue(const brute_attempt_state_t *state) {
  if (!state || state->should_stop)
    return false;

  if (state->total_attempts >= state->max_attempts) {
    log_debug("  [IMG][BRUTE] reached max attempts limit (%u)", state->max_attempts);
    return false;
  }

  time_t now = time(NULL);
  uint32_t elapsed = (uint32_t)(now - state->start_time);
  if (elapsed >= state->max_seconds) {
    log_debug("  [IMG][BRUTE] reached max time limit (%u seconds)", state->max_seconds);
    return false;
  }

  return true;
}

bool brute_record_attempt(brute_attempt_state_t *state,
                          brute_force_result_t result) {
  if (!state)
    return false;

  (void)result;

  state->total_attempts++;
  return brute_should_continue(state);
}

int brute_generate_pfs_candidates(const char *image_path,
                                  image_fs_type_t fs_type,
                                  bool mount_read_only,
                                  int stage,
                                  mount_profile_t *candidates,
                                  int max_count) {
  if (!image_path || !candidates || max_count <= 0)
    return 0;

  (void)fs_type;

  const uint16_t *image_types = NULL;
  size_t image_types_count = 0;
  const uint16_t *raw_flags = NULL;
  size_t raw_flags_count = 0;
  const uint32_t *sector_sizes = NULL;
  size_t sector_sizes_count = 0;
  const char **fstypes = NULL;
  size_t fstypes_count = 0;

  if (stage == 0) {
    // Stage A: Fast track
    image_types = STAGE_A_IMAGE_TYPES;
    image_types_count = ARRAY_COUNT(STAGE_A_IMAGE_TYPES);
    raw_flags = STAGE_A_RAW_FLAGS;
    raw_flags_count = ARRAY_COUNT(STAGE_A_RAW_FLAGS);
    sector_sizes = STAGE_A_SECTOR_SIZES;
    sector_sizes_count = ARRAY_COUNT(STAGE_A_SECTOR_SIZES);
    fstypes = STAGE_A_FSTYPES;
    fstypes_count = ARRAY_COUNT(STAGE_A_FSTYPES);
  } else {
    // Stage B: Expanded set
    image_types = STAGE_B_IMAGE_TYPES;
    image_types_count = ARRAY_COUNT(STAGE_B_IMAGE_TYPES);
    raw_flags = STAGE_B_RAW_FLAGS;
    raw_flags_count = ARRAY_COUNT(STAGE_B_RAW_FLAGS);
    sector_sizes = STAGE_B_SECTOR_SIZES;
    sector_sizes_count = ARRAY_COUNT(STAGE_B_SECTOR_SIZES);
    fstypes = STAGE_B_FSTYPES;
    fstypes_count = ARRAY_COUNT(STAGE_B_FSTYPES);
  }

  int candidate_count = 0;

  // Generate all combinations in priority order
  for (size_t i_img = 0; i_img < image_types_count && candidate_count < max_count; i_img++) {
    for (size_t i_raw = 0; i_raw < raw_flags_count && candidate_count < max_count; i_raw++) {
      for (size_t i_sec = 0; i_sec < sector_sizes_count && candidate_count < max_count; i_sec++) {
        for (size_t i_fs = 0; i_fs < fstypes_count && candidate_count < max_count; i_fs++) {
          mount_profile_t *profile = &candidates[candidate_count];
          memset(profile, 0, sizeof(*profile));

          profile->io_version = LVD_ATTACH_IO_VERSION_V0;
          profile->image_type = image_types[i_img];
          profile->raw_flags = raw_flags[i_raw];
          profile->sector_size = sector_sizes[i_sec];
          profile->secondary_unit = sector_sizes[i_sec];
          profile->fstype = fstypes[i_fs];
          profile->budgetid = DEVPFS_BUDGET_GAME;
          profile->mkeymode = DEVPFS_MKEYMODE_SD;
          profile->sigverify = (PFS_MOUNT_SIGVERIFY != 0) ? 1u : 0u;
          profile->playgo = (PFS_MOUNT_PLAYGO != 0) ? 1u : 0u;
          profile->disc = (PFS_MOUNT_DISC != 0) ? 1u : 0u;
          profile->mount_read_only = mount_read_only;

          // Normalize raw_flags
          if ((profile->raw_flags & 0x800Eu) != 0u) {
            uint32_t raw = (uint32_t)profile->raw_flags;
            uint32_t len = (raw & 0xFFFF8000u) + ((raw & 2u) << 6) +
                           (8u * (raw & 1u)) + (2u * ((raw >> 2) & 1u)) +
                           (2u * (raw & 8u)) + 4u;
            profile->normalized_flags = (uint16_t)len;
          } else {
            profile->normalized_flags = (uint16_t)(8u * ((uint32_t)profile->raw_flags & 1u) + 4u);
          }

          profile->label = NULL;

          candidate_count++;
        }
      }
    }
  }

  return candidate_count;
}

void brute_log_attempt(const char *image_path,
                       uint32_t attempt_index,
                       uint32_t total_attempts,
                       const mount_profile_t *profile,
                       brute_force_result_t result,
                       int errno_value) {
  if (!image_path || !profile)
    return;

  const char *result_str = "UNKNOWN";
  switch (result) {
  case BRUTE_RESULT_SUCCESS:
    result_str = "OK";
    break;
  case BRUTE_RESULT_ATTACH_FAILED:
    result_str = "ATTACH_FAILED";
    break;
  case BRUTE_RESULT_NMOUNT_FAILED:
    result_str = "NMOUNT_FAILED";
    break;
  case BRUTE_RESULT_VALIDATION_FAILED:
    result_str = "VALIDATION_FAILED";
    break;
  case BRUTE_RESULT_CLEANUP_FAILED:
    result_str = "CLEANUP_FAILED";
    break;
  case BRUTE_RESULT_TIMEOUT:
    result_str = "TIMEOUT";
    break;
  case BRUTE_RESULT_ABORT:
    result_str = "ABORT";
    break;
  }

  char profile_buf[160];
  mount_profile_format_compact(profile, profile_buf, sizeof(profile_buf));

  log_debug(
      "  [IMG][BRUTE] attempt=%u/%u result=%s errno=%d profile=(img=%u "
      "raw=0x%x flags=0x%x sec=%u fstype=%s)",
      attempt_index, total_attempts, result_str, errno_value, profile->image_type,
      profile->raw_flags, profile->normalized_flags, profile->sector_size,
      profile->fstype);
}

void brute_log_success(const char *image_path,
                       const mount_profile_t *profile) {
  if (!image_path || !profile)
    return;

  char profile_buf[160];
  mount_profile_format_compact(profile, profile_buf, sizeof(profile_buf));

  log_debug("  [IMG][BRUTE] profile selected and cached: %s", profile_buf);
}

void brute_log_exhausted(const char *image_path) {
  if (!image_path)
    return;

  log_debug("  [IMG][BRUTE] all profiles failed, moving to next image");
}
