#include "sm_bench.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm_limits.h"
#include "sm_log.h"
#include "sm_mount_cache.h"
#include "sm_paths.h"
#include "sm_time.h"

// ---------------------------------------------------------------------------
// Internal constants

#define BENCH_INI_PATH    "/data/shadowmount/benchmarking.ini"
#define PROBE_INI_PREFIX  "/data/shadowmount/pfs_probe_"
#define PROBE_INI_SUFFIX  ".ini"

#define BENCH_LINE_LEN    768
#define BENCH_READ_BUF    65536u   // 64 KB read chunk for file bench
#define BENCH_MAX_DIR_DEPTH 8      // recursion limit for dir walk
#define BENCH_MAX_READ_BYTES (256u * 1024u * 1024u) // hard cap 256 MB per file

// Result serialization field separator
#define RS ","

// ---------------------------------------------------------------------------
// Helpers

static uint32_t us_to_ms(uint64_t us) {
  return (uint32_t)(us / 1000u);
}

static uint64_t elapsed_ms_since(uint64_t start_us) {
  uint64_t now = monotonic_time_us();
  if (now <= start_us)
    return 0;
  return (now - start_us) / 1000u;
}

// Write contents atomically via a temp file + rename.
// buf is not necessarily NUL-terminated up to len.
static bool atomic_write_file(const char *path, const char *buf, size_t len) {
  char tmp[MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;

  size_t written = 0;
  while (written < len) {
    ssize_t w = write(fd, buf + written, len - written);
    if (w < 0) {
      close(fd);
      (void)unlink(tmp);
      return false;
    }
    written += (size_t)w;
  }
  close(fd);
  if (rename(tmp, path) != 0) {
    (void)unlink(tmp);
    return false;
  }
  return true;
}

// Read entire file into a malloc'd buffer; caller must free.  Returns NULL on
// failure.  *len_out is set to file size.
static char *slurp_file(const char *path, size_t *len_out) {
  *len_out = 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return NULL;
  if (st.st_size <= 0 || st.st_size > 8 * 1024 * 1024)
    return NULL;  // refuse >8 MB for the bench file to keep memory bounded

  char *buf = malloc((size_t)st.st_size + 1);
  if (!buf)
    return NULL;

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    free(buf);
    return NULL;
  }

  ssize_t r = read(fd, buf, (size_t)st.st_size);
  close(fd);
  if (r < 0) {
    free(buf);
    return NULL;
  }
  buf[r] = '\0';
  *len_out = (size_t)r;
  return buf;
}

// Trim leading/trailing ASCII whitespace in-place; return pointer into s.
static char *bench_trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                   e[-1] == '\n'))
    e--;
  *e = '\0';
  return s;
}

// Sanitize image_basename for use in a filename: replace '/' and '\0' with '_'.
static void safe_basename(const char *in, char *out, size_t out_size) {
  size_t i;
  for (i = 0; i < out_size - 1 && in[i]; i++) {
    char c = in[i];
    out[i] = (c == '/' || c == '\\') ? '_' : c;
  }
  out[i] = '\0';
}

// ---------------------------------------------------------------------------
// Result serialization

// Format bench_result_t into a single-line string (no newline).
static void format_bench_result(const bench_result_t *r,
                                char *buf, size_t buf_size) {
  snprintf(buf, buf_size,
           "mount_ok=%u,mount_ms=%u,"
           "dl_ok=%u,dl_ms=%u,dl_f=%u,dl_d=%u,"
           "eb_ok=%u,eb_ms=%u,eb_b=%llu,eb_gap=%u,"
           "pr_ok=%u,pr_ms=%u,pr_b=%llu,pr_gap=%u,"
           "slow=%u,fail=%u,score=%u",
           r->mount_ok ? 1u : 0u, r->mount_ms,
           r->dirlist_ok ? 1u : 0u, r->dirlist_ms,
           r->dirlist_files, r->dirlist_dirs,
           r->eboot_ok ? 1u : 0u, r->eboot_read_ms,
           (unsigned long long)r->eboot_bytes, r->eboot_max_gap_ms,
           r->param_ok ? 1u : 0u, r->param_read_ms,
           (unsigned long long)r->param_bytes, r->param_max_gap_ms,
           r->slow_reads, r->any_failed ? 1u : 0u,
           r->score_ms);
}

// Parse a bench_result string (produced by format_bench_result) back into r.
static bool parse_bench_result(const char *str, bench_result_t *r) {
  if (!str || !r)
    return false;
  memset(r, 0, sizeof(*r));

  char buf[BENCH_LINE_LEN];
  (void)strlcpy(buf, str, sizeof(buf));

  char *p = buf;
  char *token, *saveptr = NULL;
  for (token = strtok_r(p, ",", &saveptr); token;
       token = strtok_r(NULL, ",", &saveptr)) {
    char *eq = strchr(token, '=');
    if (!eq)
      continue;
    *eq = '\0';
    const char *k = bench_trim(token);
    const char *v = eq + 1;
    unsigned long long ull;
    unsigned long ul;

    if (strcmp(k, "mount_ok") == 0)       { ul = strtoul(v, NULL, 10); r->mount_ok = (ul != 0); }
    else if (strcmp(k, "mount_ms") == 0)  { r->mount_ms = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "dl_ok") == 0)     { ul = strtoul(v, NULL, 10); r->dirlist_ok = (ul != 0); }
    else if (strcmp(k, "dl_ms") == 0)     { r->dirlist_ms = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "dl_f") == 0)      { r->dirlist_files = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "dl_d") == 0)      { r->dirlist_dirs = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "eb_ok") == 0)     { ul = strtoul(v, NULL, 10); r->eboot_ok = (ul != 0); }
    else if (strcmp(k, "eb_ms") == 0)     { r->eboot_read_ms = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "eb_b") == 0)      { ull = strtoull(v, NULL, 10); r->eboot_bytes = ull; }
    else if (strcmp(k, "eb_gap") == 0)    { r->eboot_max_gap_ms = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "pr_ok") == 0)     { ul = strtoul(v, NULL, 10); r->param_ok = (ul != 0); }
    else if (strcmp(k, "pr_ms") == 0)     { r->param_read_ms = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "pr_b") == 0)      { ull = strtoull(v, NULL, 10); r->param_bytes = ull; }
    else if (strcmp(k, "pr_gap") == 0)    { r->param_max_gap_ms = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "slow") == 0)      { r->slow_reads = (uint32_t)strtoul(v, NULL, 10); }
    else if (strcmp(k, "fail") == 0)      { ul = strtoul(v, NULL, 10); r->any_failed = (ul != 0); }
    else if (strcmp(k, "score") == 0)     { r->score_ms = (uint32_t)strtoul(v, NULL, 10); }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Probe persistence

void bench_save_probe(const char *image_basename,
                      const mount_profile_t *profiles, int count) {
  if (!image_basename || !profiles || count <= 0)
    return;

  char safe[MAX_PATH];
  safe_basename(image_basename, safe, sizeof(safe));

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s%s%s", PROBE_INI_PREFIX, safe, PROBE_INI_SUFFIX);

  // Build file content
  char *content = malloc(count * 512 + 128);
  if (!content)
    return;

  int pos = 0;
  pos += snprintf(content + pos, (size_t)(count * 512 + 128 - pos),
                  "# ShadowMount PFS probe results - auto-generated\n"
                  "profile_count=%d\n", count);

  for (int i = 0; i < count; i++) {
    char profile_str[256];
    format_profile_for_cache(&profiles[i], profile_str, sizeof(profile_str));
    pos += snprintf(content + pos, (size_t)(count * 512 + 128 - pos),
                    "profile_%d=%s\n", i, profile_str);
  }

  if (!atomic_write_file(path, content, (size_t)pos))
    log_debug("  [BENCH] failed to save probe file: %s", path);
  else
    log_debug("  [BENCH] saved %d probe profiles to: %s", count, path);

  free(content);
}

int bench_load_probe(const char *image_basename,
                     mount_profile_t *profiles_out, int max_count) {
  if (!image_basename || !profiles_out || max_count <= 0)
    return 0;

  char safe[MAX_PATH];
  safe_basename(image_basename, safe, sizeof(safe));

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s%s%s", PROBE_INI_PREFIX, safe, PROBE_INI_SUFFIX);

  size_t len = 0;
  char *content = slurp_file(path, &len);
  if (!content)
    return 0;

  int loaded = 0;
  char *line, *saveptr = NULL;
  for (line = strtok_r(content, "\n", &saveptr); line && loaded < max_count;
       line = strtok_r(NULL, "\n", &saveptr)) {
    line = bench_trim(line);
    if (line[0] == '#' || line[0] == '\0')
      continue;

    // Look for profile_N=<str>
    const char *prefix = "profile_";
    if (strncmp(line, prefix, strlen(prefix)) != 0)
      continue;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;

    const char *profile_str = eq + 1;
    if (parse_profile_from_cache(profile_str, &profiles_out[loaded]))
      loaded++;
  }

  free(content);
  log_debug("  [BENCH] loaded %d probe profiles from: %s", loaded, path);
  return loaded;
}

// ---------------------------------------------------------------------------
// Benchmark execution

// Recursive directory walk; increments *files and *dirs.
// depth_remaining prevents infinite recursion.
static void walk_dir(const char *path, uint32_t *files, uint32_t *dirs,
                     int depth_remaining) {
  if (depth_remaining <= 0)
    return;

  DIR *d = opendir(path);
  if (!d)
    return;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;

    char child[MAX_PATH];
    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

    bool is_dir = false;
    if (ent->d_type == DT_DIR) {
      is_dir = true;
    } else if (ent->d_type == DT_UNKNOWN) {
      struct stat st;
      if (stat(child, &st) == 0)
        is_dir = S_ISDIR(st.st_mode);
    }

    if (is_dir) {
      (*dirs)++;
      walk_dir(child, files, dirs, depth_remaining - 1);
    } else {
      (*files)++;
    }
  }
  closedir(d);
}

// Search for a file by name (case-insensitive) under root, depth-limited.
// Writes found path into out (size out_size).  Returns true if found.
static bool find_file_ci(const char *root, const char *name,
                         int depth_remaining, char *out, size_t out_size) {
  if (depth_remaining <= 0)
    return false;

  DIR *d = opendir(root);
  if (!d)
    return false;

  bool found = false;
  struct dirent *ent;
  while (!found && (ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;

    char child[MAX_PATH];
    snprintf(child, sizeof(child), "%s/%s", root, ent->d_name);

    bool is_dir = false;
    if (ent->d_type == DT_DIR) {
      is_dir = true;
    } else if (ent->d_type == DT_UNKNOWN) {
      struct stat st;
      if (stat(child, &st) == 0)
        is_dir = S_ISDIR(st.st_mode);
    }

    if (!is_dir && strcasecmp(ent->d_name, name) == 0) {
      (void)strlcpy(out, child, out_size);
      found = true;
    } else if (is_dir) {
      found = find_file_ci(child, name, depth_remaining - 1, out, out_size);
    }
  }
  closedir(d);
  return found;
}

// Read a file measuring timing and stall detection.
// Returns bytes read; fills *total_ms, *max_gap_ms, *slow_reads.
static uint64_t bench_read_file(const char *path,
                                uint32_t min_read_bytes,
                                uint32_t delay_threshold_ms,
                                uint32_t *total_ms_out,
                                uint32_t *max_gap_ms_out,
                                uint32_t *slow_reads_out) {
  *total_ms_out = 0;
  *max_gap_ms_out = 0;
  *slow_reads_out = 0;

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;

  char *buf = malloc(BENCH_READ_BUF);
  if (!buf) {
    close(fd);
    return 0;
  }

  uint64_t total_bytes = 0;
  uint32_t max_cap = min_read_bytes;
  if (max_cap < BENCH_READ_BUF)
    max_cap = BENCH_READ_BUF;
  if (max_cap > BENCH_MAX_READ_BYTES)
    max_cap = BENCH_MAX_READ_BYTES;

  uint64_t start_us = monotonic_time_us();
  uint64_t prev_us = start_us;

  while (total_bytes < (uint64_t)max_cap) {
    uint64_t chunk_start = monotonic_time_us();
    ssize_t r = read(fd, buf, BENCH_READ_BUF);
    if (r <= 0)
      break;
    total_bytes += (uint64_t)r;

    uint64_t chunk_end = monotonic_time_us();
    uint32_t chunk_ms = us_to_ms(chunk_end - chunk_start);

    // Gap between end of previous read and start of this one
    uint32_t gap_ms = us_to_ms(chunk_start - prev_us);
    prev_us = chunk_end;

    if (gap_ms > *max_gap_ms_out)
      *max_gap_ms_out = gap_ms;
    if (chunk_ms > delay_threshold_ms || gap_ms > delay_threshold_ms)
      (*slow_reads_out)++;
  }

  uint64_t end_us = monotonic_time_us();
  *total_ms_out = us_to_ms(end_us - start_us);

  free(buf);
  close(fd);
  (void)elapsed_ms_since; // suppress unused warning
  return total_bytes;
}

bool bench_run_mounted(const char *mount_point,
                       const runtime_config_t *cfg,
                       bench_result_t *result_out) {
  if (!mount_point || !cfg || !result_out)
    return false;

  uint32_t min_read = cfg->pfs_bench_min_read_bytes;
  if (min_read < BENCH_READ_BUF)
    min_read = BENCH_READ_BUF;

  uint32_t delay_thresh = cfg->pfs_bench_delay_threshold_ms;
  if (delay_thresh == 0)
    delay_thresh = 500u;

  result_out->mount_ok = true;  // caller guarantees this

  // --- Directory walk ---
  uint64_t dl_start = monotonic_time_us();
  result_out->dirlist_files = 0;
  result_out->dirlist_dirs = 0;
  walk_dir(mount_point, &result_out->dirlist_files,
           &result_out->dirlist_dirs, BENCH_MAX_DIR_DEPTH);
  result_out->dirlist_ms = us_to_ms(monotonic_time_us() - dl_start);
  result_out->dirlist_ok = (result_out->dirlist_files > 0 ||
                            result_out->dirlist_dirs > 0);

  log_debug("  [BENCH] dirlist: ok=%d ms=%u files=%u dirs=%u",
            result_out->dirlist_ok ? 1 : 0,
            result_out->dirlist_ms,
            result_out->dirlist_files,
            result_out->dirlist_dirs);

  // --- eboot.bin read ---
  char eboot_path[MAX_PATH];
  eboot_path[0] = '\0';
  (void)find_file_ci(mount_point, "eboot.bin", 4, eboot_path, sizeof(eboot_path));

  if (eboot_path[0] != '\0') {
    result_out->eboot_bytes = bench_read_file(
        eboot_path, min_read, delay_thresh,
        &result_out->eboot_read_ms,
        &result_out->eboot_max_gap_ms,
        &result_out->slow_reads);
    result_out->eboot_ok = (result_out->eboot_bytes > 0);
    log_debug("  [BENCH] eboot.bin: ok=%d ms=%u bytes=%llu max_gap=%u slow=%u",
              result_out->eboot_ok ? 1 : 0,
              result_out->eboot_read_ms,
              (unsigned long long)result_out->eboot_bytes,
              result_out->eboot_max_gap_ms,
              result_out->slow_reads);
  } else {
    log_debug("  [BENCH] eboot.bin: not found under %s", mount_point);
  }

  // --- param.json read (sce_sys/param.json) ---
  char param_path[MAX_PATH];
  param_path[0] = '\0';
  (void)find_file_ci(mount_point, "param.json", 4, param_path, sizeof(param_path));

  uint32_t param_slow = 0;
  if (param_path[0] != '\0') {
    result_out->param_bytes = bench_read_file(
        param_path, min_read, delay_thresh,
        &result_out->param_read_ms,
        &result_out->param_max_gap_ms,
        &param_slow);
    result_out->slow_reads += param_slow;
    result_out->param_ok = (result_out->param_bytes > 0);
    log_debug("  [BENCH] param.json: ok=%d ms=%u bytes=%llu max_gap=%u",
              result_out->param_ok ? 1 : 0,
              result_out->param_read_ms,
              (unsigned long long)result_out->param_bytes,
              result_out->param_max_gap_ms);
  } else {
    log_debug("  [BENCH] param.json: not found under %s", mount_point);
  }

  result_out->any_failed = (!result_out->dirlist_ok && !result_out->eboot_ok);

  // Compute composite score: sum of key latencies weighted toward actual reads.
  // Failures add a large penalty so they sort to the end.
  if (result_out->dirlist_ok || result_out->eboot_ok) {
    uint32_t score = result_out->mount_ms +
                     result_out->dirlist_ms +
                     result_out->eboot_read_ms +
                     result_out->param_read_ms +
                     result_out->eboot_max_gap_ms * 2u +
                     result_out->param_max_gap_ms * 2u +
                     result_out->slow_reads * 500u;
    result_out->score_ms = (score == 0) ? 1u : score;
  } else {
    result_out->score_ms = 0;  // indicates failure
  }

  return result_out->dirlist_ok || result_out->eboot_ok;
}

// ---------------------------------------------------------------------------
// Benchmark persistence

// A parsed benchmarking.ini is stored in memory as a flat list of sections.
// Each section has a name (the image basename) and a list of key=value pairs.
// Since we rewrite the whole file on each save, we keep it simple.

// Build the path to benchmarking.ini
static const char *bench_ini_path(void) {
  return BENCH_INI_PATH;
}

// Parse the INI into a malloc'd buffer of lines, grouped by section.
// Returns the raw file content; caller frees.  Out-parameters point into it.
// We locate the section for image_basename and find specific keys.

typedef struct {
  char key[64];
  char value[BENCH_LINE_LEN];
} bench_kv_t;

typedef struct {
  char name[MAX_PATH];      // section name (image basename)
  bench_kv_t *kvs;          // dynamically allocated
  int kv_count;
  int kv_cap;
} bench_section_t;

#define BENCH_MAX_SECTIONS 64
#define BENCH_MAX_KVS_PER_SECTION 256

static void bench_section_free(bench_section_t *s) {
  free(s->kvs);
  s->kvs = NULL;
  s->kv_count = 0;
  s->kv_cap = 0;
}

static bool bench_section_add_kv(bench_section_t *s,
                                  const char *k, const char *v) {
  if (s->kv_count >= BENCH_MAX_KVS_PER_SECTION)
    return false;

  if (s->kv_count >= s->kv_cap) {
    int new_cap = s->kv_cap ? s->kv_cap * 2 : 16;
    if (new_cap > BENCH_MAX_KVS_PER_SECTION)
      new_cap = BENCH_MAX_KVS_PER_SECTION;
    bench_kv_t *nk = realloc(s->kvs, (size_t)new_cap * sizeof(bench_kv_t));
    if (!nk)
      return false;
    s->kvs = nk;
    s->kv_cap = new_cap;
  }

  (void)strlcpy(s->kvs[s->kv_count].key, k, sizeof(s->kvs[0].key));
  (void)strlcpy(s->kvs[s->kv_count].value, v, sizeof(s->kvs[0].value));
  s->kv_count++;
  return true;
}

static const char *bench_section_get(const bench_section_t *s, const char *k) {
  for (int i = 0; i < s->kv_count; i++) {
    if (strcasecmp(s->kvs[i].key, k) == 0)
      return s->kvs[i].value;
  }
  return NULL;
}

static void bench_section_set(bench_section_t *s, const char *k, const char *v) {
  for (int i = 0; i < s->kv_count; i++) {
    if (strcasecmp(s->kvs[i].key, k) == 0) {
      (void)strlcpy(s->kvs[i].value, v, sizeof(s->kvs[i].value));
      return;
    }
  }
  bench_section_add_kv(s, k, v);
}

// Parse benchmarking.ini into sections array.
// sections must be BENCH_MAX_SECTIONS elements; returns number of sections.
static int bench_parse_ini(const char *content, bench_section_t *sections,
                            int max_sections) {
  if (!content || !sections || max_sections <= 0)
    return 0;

  char *buf = strdup(content);
  if (!buf)
    return 0;

  int count = 0;
  int cur = -1;  // current section index

  char *line, *saveptr = NULL;
  for (line = strtok_r(buf, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    line = bench_trim(line);
    if (line[0] == '#' || line[0] == ';' || line[0] == '\0')
      continue;

    if (line[0] == '[') {
      // Section header
      char *end = strchr(line, ']');
      if (!end)
        continue;
      *end = '\0';
      const char *name = line + 1;

      if (count >= max_sections) {
        cur = -1;
        continue;
      }
      memset(&sections[count], 0, sizeof(sections[0]));
      (void)strlcpy(sections[count].name, name, sizeof(sections[0].name));
      cur = count;
      count++;
      continue;
    }

    if (cur < 0)
      continue;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *k = bench_trim(line);
    char *v = bench_trim(eq + 1);
    bench_section_add_kv(&sections[cur], k, v);
  }

  free(buf);
  return count;
}

// Serialize sections back to a buffer.  Returns malloc'd string; caller frees.
static char *bench_serialize_ini(const bench_section_t *sections, int count) {
  // Estimate size
  size_t sz = 128;
  for (int i = 0; i < count; i++) {
    sz += 4 + strlen(sections[i].name) + 4;
    for (int j = 0; j < sections[i].kv_count; j++)
      sz += strlen(sections[i].kvs[j].key) + 3 +
            strlen(sections[i].kvs[j].value) + 2;
  }

  char *buf = malloc(sz);
  if (!buf)
    return NULL;

  int pos = 0;
  pos += snprintf(buf + pos, sz - (size_t)pos,
                  "# ShadowMount PFS bench state - auto-generated\n");

  for (int i = 0; i < count; i++) {
    pos += snprintf(buf + pos, sz - (size_t)pos,
                    "\n[%s]\n", sections[i].name);
    for (int j = 0; j < sections[i].kv_count; j++) {
      pos += snprintf(buf + pos, sz - (size_t)pos,
                      "%s=%s\n",
                      sections[i].kvs[j].key,
                      sections[i].kvs[j].value);
    }
  }
  return buf;
}

// Find section index for image_basename, or -1.
static int bench_find_section(const bench_section_t *sections, int count,
                               const char *name) {
  for (int i = 0; i < count; i++) {
    if (strcasecmp(sections[i].name, name) == 0)
      return i;
  }
  return -1;
}

int bench_load_results(const char *image_basename,
                       bench_result_t *results_out, int max_count,
                       int *next_to_bench_out, bool *bench_complete_out) {
  if (next_to_bench_out)
    *next_to_bench_out = 0;
  if (bench_complete_out)
    *bench_complete_out = false;
  if (!image_basename || !results_out || max_count <= 0)
    return 0;

  size_t len = 0;
  char *content = slurp_file(bench_ini_path(), &len);
  if (!content)
    return 0;

  bench_section_t sections[BENCH_MAX_SECTIONS];
  memset(sections, 0, sizeof(sections));
  int nsec = bench_parse_ini(content, sections, BENCH_MAX_SECTIONS);
  free(content);

  int sec_idx = bench_find_section(sections, nsec, image_basename);
  if (sec_idx < 0) {
    for (int i = 0; i < nsec; i++)
      bench_section_free(&sections[i]);
    return 0;
  }

  bench_section_t *sec = &sections[sec_idx];

  const char *v;

  if (next_to_bench_out) {
    v = bench_section_get(sec, "bench_next");
    if (v)
      *next_to_bench_out = (int)strtol(v, NULL, 10);
  }
  if (bench_complete_out) {
    v = bench_section_get(sec, "bench_done");
    if (v)
      *bench_complete_out = (strtol(v, NULL, 10) != 0);
  }

  // Load results
  int loaded = 0;
  for (int i = 0; i < max_count; i++) {
    char key[32];
    snprintf(key, sizeof(key), "result_%d", i);
    v = bench_section_get(sec, key);
    if (!v)
      break;
    if (parse_bench_result(v, &results_out[loaded])) {
      // Load the profile from profile_N
      char pk[32];
      snprintf(pk, sizeof(pk), "profile_%d", i);
      const char *pv = bench_section_get(sec, pk);
      if (pv)
        (void)parse_profile_from_cache(pv, &results_out[loaded].profile);
      loaded++;
    }
  }

  for (int i = 0; i < nsec; i++)
    bench_section_free(&sections[i]);

  return loaded;
}

bool bench_save_result(const char *image_basename,
                       int profile_idx, const bench_result_t *result,
                       int total_count, bool bench_complete, int best_idx) {
  if (!image_basename || !result)
    return false;

  // Load existing content
  size_t len = 0;
  char *content = slurp_file(bench_ini_path(), &len);

  bench_section_t sections[BENCH_MAX_SECTIONS];
  memset(sections, 0, sizeof(sections));
  int nsec = 0;

  if (content) {
    nsec = bench_parse_ini(content, sections, BENCH_MAX_SECTIONS);
    free(content);
    content = NULL;
  }

  // Find or create section for this image
  int sec_idx = bench_find_section(sections, nsec, image_basename);
  if (sec_idx < 0) {
    if (nsec >= BENCH_MAX_SECTIONS) {
      for (int i = 0; i < nsec; i++)
        bench_section_free(&sections[i]);
      return false;
    }
    memset(&sections[nsec], 0, sizeof(sections[0]));
    (void)strlcpy(sections[nsec].name, image_basename,
                  sizeof(sections[0].name));
    sec_idx = nsec;
    nsec++;
  }

  bench_section_t *sec = &sections[sec_idx];

  // Store result
  char result_str[BENCH_LINE_LEN];
  format_bench_result(result, result_str, sizeof(result_str));
  char key[32];
  snprintf(key, sizeof(key), "result_%d", profile_idx);
  bench_section_set(sec, key, result_str);

  // Store corresponding profile
  char pk[32];
  snprintf(pk, sizeof(pk), "profile_%d", profile_idx);
  char profile_str[256];
  format_profile_for_cache(&result->profile, profile_str, sizeof(profile_str));
  bench_section_set(sec, pk, profile_str);

  // Update meta-keys
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%d", total_count);
  bench_section_set(sec, "bench_total", tmp);
  snprintf(tmp, sizeof(tmp), "%d", bench_complete ? total_count : profile_idx + 1);
  bench_section_set(sec, "bench_next", tmp);
  snprintf(tmp, sizeof(tmp), "%d", bench_complete ? 1 : 0);
  bench_section_set(sec, "bench_done", tmp);
  snprintf(tmp, sizeof(tmp), "%d", best_idx);
  bench_section_set(sec, "best", tmp);

  // Serialize and write
  char *new_content = bench_serialize_ini(sections, nsec);
  bool ok = false;
  if (new_content) {
    ok = atomic_write_file(bench_ini_path(), new_content, strlen(new_content));
    free(new_content);
  }

  for (int i = 0; i < nsec; i++)
    bench_section_free(&sections[i]);

  return ok;
}

// ---------------------------------------------------------------------------
// Analysis

int bench_find_best(const bench_result_t *results, int count) {
  int best = -1;
  uint32_t best_score = 0;

  for (int i = 0; i < count; i++) {
    if (!results[i].mount_ok || results[i].score_ms == 0)
      continue;
    if (best < 0 || results[i].score_ms < best_score) {
      best = i;
      best_score = results[i].score_ms;
    }
  }
  return best;
}

void bench_log_report(const char *image_basename,
                      const bench_result_t *results, int count, int best_idx) {
  if (!image_basename || !results || count <= 0)
    return;

  log_debug("  [BENCH] ===== report for %s =====", image_basename);
  log_debug("  [BENCH] profiles tested: %d  best_idx: %d", count, best_idx);

  for (int i = 0; i < count; i++) {
    const bench_result_t *r = &results[i];
    char profile_buf[160];
    mount_profile_format_compact(&r->profile, profile_buf, sizeof(profile_buf));
    log_debug(
        "  [BENCH] [%d%s] profile=(%s) score=%u mount_ms=%u "
        "dirlist_ms=%u(f=%u,d=%u) eboot_ms=%u(%lluB,gap=%u) "
        "param_ms=%u(%lluB,gap=%u) slow=%u fail=%d",
        i, (i == best_idx) ? "*" : " ",
        profile_buf, r->score_ms, r->mount_ms,
        r->dirlist_ms, r->dirlist_files, r->dirlist_dirs,
        r->eboot_read_ms, (unsigned long long)r->eboot_bytes, r->eboot_max_gap_ms,
        r->param_read_ms, (unsigned long long)r->param_bytes, r->param_max_gap_ms,
        r->slow_reads, r->any_failed ? 1 : 0);
  }

  if (best_idx >= 0 && best_idx < count) {
    char best_prof[160];
    mount_profile_format_compact(&results[best_idx].profile, best_prof, sizeof(best_prof));
    log_debug("  [BENCH] best profile for %s: (%s) score=%u",
              image_basename, best_prof, results[best_idx].score_ms);
  } else {
    log_debug("  [BENCH] no valid profiles found for %s", image_basename);
  }
}
