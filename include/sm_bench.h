#ifndef SM_BENCH_H
#define SM_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#include "sm_mount_profile.h"
#include "sm_types.h"

// Maximum working profiles collected by one probe sweep
#define SM_PROBE_MAX_WINNERS 64

// Benchmark result for one profile run against one image
typedef struct {
  // Profile that was tested
  mount_profile_t profile;

  // Mount timing
  bool mount_ok;
  uint32_t mount_ms;

  // Recursive directory walk
  bool dirlist_ok;
  uint32_t dirlist_ms;
  uint32_t dirlist_files;
  uint32_t dirlist_dirs;

  // eboot.bin read (present in every PS5 game)
  bool eboot_ok;
  uint32_t eboot_read_ms;
  uint64_t eboot_bytes;
  uint32_t eboot_max_gap_ms;  // max stall between consecutive 64 KB reads

  // sce_sys/param.json read (main title config JSON)
  bool param_ok;
  uint32_t param_read_ms;
  uint64_t param_bytes;
  uint32_t param_max_gap_ms;

  // Count of individual reads that exceeded the delay threshold
  uint32_t slow_reads;
  bool any_failed;

  // Composite latency score in ms (lower = better; 0 = this profile failed)
  uint32_t score_ms;
} bench_result_t;

// ---------------------------------------------------------------------------
// Probe persistence
// Save all working profiles found by probe sweep to
//   /data/shadowmount/pfs_probe_{image_basename}.ini
void bench_save_probe(const char *image_basename,
                      const mount_profile_t *profiles, int count);

// Load working profiles from pfs_probe_{image_basename}.ini.
// Returns number of profiles loaded (capped at max_count).
int bench_load_probe(const char *image_basename,
                     mount_profile_t *profiles_out, int max_count);

// ---------------------------------------------------------------------------
// Benchmark execution
// Run a read-performance benchmark on an already-mounted PFS image.
// mount_point  : root of the mounted filesystem (e.g. /mnt/shadowmnt/...)
// cfg          : runtime config for thresholds / read sizes
// result_out   : filled on return (mount_ok is always left true by caller)
// Returns true if at least the directory walk succeeded.
bool bench_run_mounted(const char *mount_point,
                       const runtime_config_t *cfg,
                       bench_result_t *result_out);

// ---------------------------------------------------------------------------
// Benchmark persistence   (all images share one benchmarking.ini)
//
// Load existing bench results for image_basename from benchmarking.ini.
// Returns number of results loaded; sets *next_to_bench_out and
// *bench_complete_out.
int bench_load_results(const char *image_basename,
                       bench_result_t *results_out, int max_count,
                       int *next_to_bench_out, bool *bench_complete_out);

// Upsert one bench result into benchmarking.ini.
// Call with bench_complete=true and the best_idx once all profiles are done.
bool bench_save_result(const char *image_basename,
                       int profile_idx, const bench_result_t *result,
                       int total_count, bool bench_complete, int best_idx);

// ---------------------------------------------------------------------------
// Analysis
// Return the index of the best result (lowest score, requires mount_ok).
// Returns -1 if no valid results.
int bench_find_best(const bench_result_t *results, int count);

// Log a human-readable summary report for all profiles of one image.
void bench_log_report(const char *image_basename,
                      const bench_result_t *results, int count, int best_idx);

#endif
