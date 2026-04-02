#ifndef SM_BRUTE_FORCE_H
#define SM_BRUTE_FORCE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "sm_mount_profile.h"
#include "sm_types.h"

// Brute-force mount attempt result
typedef enum {
  BRUTE_RESULT_SUCCESS = 0,
  BRUTE_RESULT_ATTACH_FAILED,
  BRUTE_RESULT_NMOUNT_FAILED,
  BRUTE_RESULT_VALIDATION_FAILED,
  BRUTE_RESULT_CLEANUP_FAILED,
  BRUTE_RESULT_TIMEOUT,
  BRUTE_RESULT_ABORT,
} brute_force_result_t;

// Per-image attempt tracking
typedef struct {
  uint32_t total_attempts;
  uint32_t max_attempts;
  uint32_t max_seconds;
  time_t start_time;
  bool should_stop;
} brute_attempt_state_t;

// Candidate list generator for PFS/special mounts
// Generates up to max_count profile candidates in priority order.
// Pass stage=0 for Stage A (fast path), stage=1 for Stage B (expanded).
typedef int (*profile_generator_fn)(const char *image_path,
                                    image_fs_type_t fs_type,
                                    bool mount_read_only,
                                    int stage,
                                    mount_profile_t *candidates,
                                    int max_count);

// Initialize brute-force attempt tracking
void brute_attempt_state_init(brute_attempt_state_t *state,
                              uint32_t max_attempts,
                              uint32_t max_seconds_per_image);

// Check if we should continue attempting (time/count limits)
bool brute_should_continue(const brute_attempt_state_t *state);

// Record one attempt and check limits
bool brute_record_attempt(brute_attempt_state_t *state,
                          brute_force_result_t result);

// Generate PFS brute-force candidate profiles (Stage A + B)
int brute_generate_pfs_candidates(const char *image_path,
                                  image_fs_type_t fs_type,
                                  bool mount_read_only,
                                  int stage,
                                  mount_profile_t *candidates,
                                  int max_count);

// Log one attempt with result
void brute_log_attempt(const char *image_path,
                       uint32_t attempt_index,
                       uint32_t total_attempts,
                       const mount_profile_t *profile,
                       brute_force_result_t result,
                       int errno_value);

// Log success/cache message
void brute_log_success(const char *image_path,
                       const mount_profile_t *profile);

// Log exhaustion message
void brute_log_exhausted(const char *image_path);

#endif
