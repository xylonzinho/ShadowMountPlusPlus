#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_install.h"
#include "sm_types.h"
#include "sm_game_cache.h"
#include "sm_log.h"
#include "sm_filesystem.h"
#include "sm_limits.h"
#include "sm_path_utils.h"
#include "sm_appdb.h"
#include "sm_title_state.h"
#include "sm_image_cache.h"
#include "sm_paths.h"

static bool write_link_file(const char *path, const char *value) {
  FILE *f = fopen(path, "w");
  if (!f) {
    log_debug("  [LINK] open failed for %s: %s", path, strerror(errno));
    return false;
  }

  int saved_errno = 0;
  if (fprintf(f, "%s", value) < 0)
    saved_errno = errno;
  if (fflush(f) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (fclose(f) != 0 && saved_errno == 0)
    saved_errno = errno;

  if (saved_errno != 0) {
    errno = saved_errno;
    log_debug("  [LINK] write failed for %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

static bool is_appmeta_file(const char *name) {
  if (!name)
    return false;
  if (strcasecmp(name, "param.json") == 0 || strcasecmp(name, "param.sfo") == 0)
    return true;

  const char *ext = strrchr(name, '.');
  if (!ext)
    return false;

  return (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".dds") == 0 ||
          strcasecmp(ext, ".at9") == 0);
}

static bool copy_sce_sys_to_appmeta(const char *src_sce_sys,
                                    const char *user_appmeta_dir) {
  DIR *d = opendir(src_sce_sys);
  if (!d)
    return false;

  bool ok = true;
  struct dirent *e;
  char src_path[MAX_PATH];
  char dst_path[MAX_PATH];
  struct stat st;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (!is_appmeta_file(e->d_name))
      continue;

    snprintf(src_path, sizeof(src_path), "%s/%s", src_sce_sys, e->d_name);
    if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    snprintf(dst_path, sizeof(dst_path), "%s/%s", user_appmeta_dir, e->d_name);
    if (copy_file(src_path, dst_path) != 0) {
      ok = false;
      break;
    }
  }

  if (closedir(d) != 0)
    ok = false;
  return ok;
}

static bool validate_install_source(const char *src_path, const char *title_id) {
  char src_eboot[MAX_PATH];
  char src_param_json[MAX_PATH];
  char src_param_sfo[MAX_PATH];

  log_debug("  [STEP][VAL] begin title=%s src=%s", title_id, src_path);

  snprintf(src_eboot, sizeof(src_eboot), "%s/eboot.bin", src_path);
  snprintf(src_param_json, sizeof(src_param_json), "%s/sce_sys/param.json", src_path);
  snprintf(src_param_sfo, sizeof(src_param_sfo), "%s/sce_sys/param.sfo", src_path);

  log_debug("  [STEP][VAL] checking eboot: %s", src_eboot);

  if (!path_exists(src_eboot)) {
    log_debug("  [REG] missing source eboot.bin: %s", src_eboot);
    return false;
  }

  log_debug("  [STEP][VAL] eboot present");
  log_debug("  [STEP][VAL] checking metadata: %s | %s", src_param_json,
            src_param_sfo);

  if (!path_exists(src_param_json) && !path_exists(src_param_sfo)) {
    log_debug("  [REG] missing source param metadata: %s or %s",
              src_param_json, src_param_sfo);
    return false;
  }

  log_debug("  [STEP][VAL] metadata present");
  log_debug("  [STEP][VAL] source validation complete");
  return true;
}

static bool resolve_install_source_root(const char *src_path,
                                        const char *title_id,
                                        char *resolved_src,
                                        size_t resolved_src_size) {
  const char *suffixes[] = {"", "/app0", "/APP0", "/image0", "/Image0"};
  char candidate[MAX_PATH];

  if (!src_path || !title_id || !resolved_src || resolved_src_size == 0)
    return false;

  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    char candidate_eboot[MAX_PATH];
    char candidate_param_json[MAX_PATH];
    char candidate_param_sfo[MAX_PATH];

    snprintf(candidate, sizeof(candidate), "%s%s", src_path, suffixes[i]);
    snprintf(candidate_eboot, sizeof(candidate_eboot), "%s/eboot.bin",
             candidate);
    snprintf(candidate_param_json, sizeof(candidate_param_json),
             "%s/sce_sys/param.json", candidate);
    snprintf(candidate_param_sfo, sizeof(candidate_param_sfo),
             "%s/sce_sys/param.sfo", candidate);

    if (path_exists(candidate_eboot) &&
        (path_exists(candidate_param_json) || path_exists(candidate_param_sfo))) {
      snprintf(resolved_src, resolved_src_size, "%s", candidate);
      return true;
    }
  }

  snprintf(candidate, sizeof(candidate), "%s/%s", src_path, title_id);
  {
    char candidate_eboot[MAX_PATH];
    char candidate_param_json[MAX_PATH];
    char candidate_param_sfo[MAX_PATH];

    snprintf(candidate_eboot, sizeof(candidate_eboot), "%s/eboot.bin",
             candidate);
    snprintf(candidate_param_json, sizeof(candidate_param_json),
             "%s/sce_sys/param.json", candidate);
    snprintf(candidate_param_sfo, sizeof(candidate_param_sfo),
             "%s/sce_sys/param.sfo", candidate);

    if (path_exists(candidate_eboot) &&
        (path_exists(candidate_param_json) || path_exists(candidate_param_sfo))) {
      snprintf(resolved_src, resolved_src_size, "%s", candidate);
      return true;
    }
  }

  return false;
}

static bool register_title(const char *src_path, const char *title_id,
                           const char *title_name, bool metadata_restaged,
                           bool has_src_snd0) {
  char src_snd0[MAX_PATH];
  log_debug("  [STEP][REG] begin title=%s metadata_restaged=%d has_src_snd0=%d",
            title_id, metadata_restaged ? 1 : 0, has_src_snd0 ? 1 : 0);
  if (!metadata_restaged) {
    snprintf(src_snd0, sizeof(src_snd0), "%s/sce_sys/snd0.at9", src_path);
    log_debug("  [STEP][REG] checking snd0 source: %s", src_snd0);
    has_src_snd0 = path_exists(src_snd0);
    log_debug("  [STEP][REG] snd0 present=%d", has_src_snd0 ? 1 : 0);
  }

  log_debug("  [REG] begin title=%s base=%s", title_id, APP_BASE "/");
  log_debug("  [STEP][REG] mark register attempted");
  mark_register_attempted(title_id);
  log_debug("  [STEP][REG] calling sceAppInstUtilAppInstallTitleDir");
  int res = sceAppInstUtilAppInstallTitleDir(title_id, APP_BASE "/", 0);
  log_debug("  [REG] result title=%s code=0x%08X", title_id, (uint32_t)res);
  log_debug("  [STEP][REG] sleeping after install API");
  sceKernelUsleep(200000);

  if (res == 0) {
    log_debug("  [STEP][REG] install result path: NEW");
    invalidate_app_db_title_cache();
    log_debug("  [REG] Installed NEW!");
    notify_system_info("Installed: %s", title_id);
    if (has_src_snd0) {
      log_debug("  [STEP][REG] updating snd0info for new install");
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
    }
    log_debug("  [STEP][REG] completed NEW path");
    return true;
  }

  if ((uint32_t)res == 0x80990002u) {
    log_debug("  [STEP][REG] install result path: RESTORED");
    invalidate_app_db_title_cache();
    log_debug("  [REG] Restored.");
    if (has_src_snd0) {
      log_debug("  [STEP][REG] updating snd0info for restored install");
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
    }
    log_debug("  [STEP][REG] completed RESTORED path");
    return true;
  }

  log_debug("  [STEP][REG] install result path: FAIL");
  log_debug("  [REG] FAIL: 0x%x", res);
  notify_system("Register failed: %s (%s)\ncode=0x%08X", title_name, title_id,
                (uint32_t)res);
  return false;
}

// --- Install/Remount Action ---
static bool mount_and_install(const char *src_path, const char *title_id,
                              const char *title_name, bool is_remount,
                              bool should_register) {
  char user_appmeta_dir[MAX_PATH];
  char user_app_dir[MAX_PATH];
  char user_sce_sys[MAX_PATH];
  char src_sce_sys[MAX_PATH];
  char src_snd0[MAX_PATH];
  char image_source_path[MAX_PATH];
  bool has_image_source = false;
  bool appmeta_missing = false;
  bool metadata_restaged = false;
  bool restage_staging = false;
  bool restage_appmeta = false;
  bool has_src_snd0 = false;
  bool image_mount_source = is_under_image_mount_base(src_path);
  char resolved_src_path[MAX_PATH];
  const char *effective_src_path = src_path;

  log_debug("  [STEP][FLOW] begin title=%s remount=%d should_register=%d src=%s",
            title_id, is_remount ? 1 : 0, should_register ? 1 : 0, src_path);

  snprintf(user_appmeta_dir, sizeof(user_appmeta_dir), "%s/%s", APPMETA_BASE,
           title_id);
  snprintf(user_app_dir, sizeof(user_app_dir), "%s/%s", APP_BASE, title_id);

  log_debug("  [STEP][FLOW] target dirs app=%s appmeta=%s", user_app_dir,
            user_appmeta_dir);

  log_debug("  [STEP][VAL] resolving source root from %s", src_path);
  if (!resolve_install_source_root(src_path, title_id, resolved_src_path,
                                   sizeof(resolved_src_path))) {
    log_debug("  [STEP][VAL] no valid install root found for %s under %s",
              title_id, src_path);
    log_debug("  [REG] skipping install due to invalid source root");
    return false;
  }
  effective_src_path = resolved_src_path;
  if (strcmp(effective_src_path, src_path) != 0) {
    log_debug("  [STEP][VAL] source root resolved: %s -> %s", src_path,
              effective_src_path);
  }

  if (!validate_install_source(effective_src_path, title_id)) {
    log_debug("  [STEP][VAL] validation reported issues, continuing flow anyway");
  } else {
    log_debug("  [STEP][FLOW] source validation passed");
  }

  if (image_mount_source) {
    log_debug("  [STEP][FLOW] image-backed source detected");
    has_image_source = resolve_image_source_from_mount_cache(
        src_path, image_source_path, sizeof(image_source_path));
    if (!has_image_source) {
      log_debug("  [LINK] image source lookup failed for %s: %s", title_id,
                src_path);
    } else {
      log_debug("  [STEP][FLOW] image source cache hit: %s", image_source_path);
    }
  } else {
    log_debug("  [STEP][FLOW] direct source (not image mount base)");
  }

  appmeta_missing = !has_appmeta_data(title_id);
  if (is_remount && appmeta_missing) {
    log_debug("  [REG] appmeta missing, restaging metadata only: %s", title_id);
  }

  restage_staging = (!is_remount || should_register);
  restage_appmeta = (!is_remount || appmeta_missing);

  log_debug("  [STEP][FLOW] flags appmeta_missing=%d restage_staging=%d restage_appmeta=%d",
            appmeta_missing ? 1 : 0, restage_staging ? 1 : 0,
            restage_appmeta ? 1 : 0);

  // COPY FILES
  if (restage_staging || restage_appmeta) {
    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", effective_src_path);
    snprintf(src_snd0, sizeof(src_snd0), "%s/snd0.at9", src_sce_sys);
    has_src_snd0 = path_exists(src_snd0);
    log_debug("  [STEP][COPY] source sce_sys=%s snd0_present=%d", src_sce_sys,
              has_src_snd0 ? 1 : 0);
  } else {
    log_debug("  [SPEED] Skipping file copy (Assets already exist)");
  }

  if (restage_staging) {
    log_debug("  [STEP][COPY] staging start");
    mkdir(APP_BASE, 0777);
    mkdir(user_app_dir, 0777);
    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_sce_sys, 0777);
    log_debug("  [STEP][COPY] copy app metadata files %s -> %s", src_sce_sys,
              user_sce_sys);
    if (!copy_sce_sys_to_appmeta(src_sce_sys, user_sce_sys)) {
      log_debug("  [COPY] Failed to copy sce_sys metadata staging: %s -> %s",
                src_sce_sys, user_sce_sys);
      return false;
    }

    char icon_src[MAX_PATH];
    char icon_dst[MAX_PATH];
    snprintf(icon_src, sizeof(icon_src), "%s/icon0.png", src_sce_sys);
    snprintf(icon_dst, sizeof(icon_dst), "%s/icon0.png", user_app_dir);
    log_debug("  [STEP][COPY] copy icon %s -> %s", icon_src, icon_dst);
    if (copy_file(icon_src, icon_dst) != 0) {
      log_debug("  [COPY] Failed to copy staged icon: %s -> %s", icon_src,
                icon_dst);
      return false;
    }
    log_debug("  [STEP][COPY] staging complete");
  }

  if (restage_appmeta) {
    log_debug("  [STEP][COPY] appmeta staging start");
    mkdir(APPMETA_BASE, 0777);
    mkdir(user_appmeta_dir, 0777);
    if (!copy_sce_sys_to_appmeta(src_sce_sys, user_appmeta_dir)) {
      log_debug("  [COPY] Failed to copy appmeta files: %s -> %s", src_sce_sys,
                user_appmeta_dir);
      return false;
    }
    metadata_restaged = true;
    log_debug("  [STEP][COPY] appmeta staging complete");
  }

  log_debug("  [STEP][MOUNT] mounting nullfs title=%s src=%s", title_id,
            effective_src_path);
  if (!mount_title_nullfs(title_id, effective_src_path)) {
    log_debug("  [LINK] nullfs mount failed: title=%s src=%s", title_id,
              effective_src_path);
    return false;
  }
  log_debug("  [STEP][MOUNT] nullfs mounted");

  // WRITE TRACKER
  char lnk_path[MAX_PATH];
  mkdir(APP_BASE, 0777);
  mkdir(user_app_dir, 0777);
  snprintf(lnk_path, sizeof(lnk_path), "%s/mount.lnk", user_app_dir);
  log_debug("  [STEP][LINK] writing mount link %s", lnk_path);
  if (!write_link_file(lnk_path, effective_src_path))
    return false;

  log_debug("  [LINK] mount.lnk created: %s -> %s", lnk_path,
            effective_src_path);

  char img_lnk_path[MAX_PATH];
  snprintf(img_lnk_path, sizeof(img_lnk_path), "%s/mount_img.lnk",
           user_app_dir);
  if (has_image_source) {
    log_debug("  [STEP][LINK] writing image link %s -> %s", img_lnk_path,
              image_source_path);
    if (!write_link_file(img_lnk_path, image_source_path))
      return false;
    log_debug("  [LINK] mount_img.lnk created: %s -> %s", img_lnk_path,
              image_source_path);
    if (!cache_image_source_mapping(image_source_path, effective_src_path)) {
      log_debug("  [LINK] image source cache update failed: %s -> %s",
                effective_src_path, image_source_path);
    }
  } else if (unlink(img_lnk_path) != 0 && errno != ENOENT) {
    log_debug("  [LINK] remove failed for %s: %s", img_lnk_path,
              strerror(errno));
  } else {
    log_debug("  [STEP][LINK] no image link required for this source");
  }

  if (!should_register) {
    log_debug("  [STEP][REG] skipped by should_register=0");
    if (metadata_restaged && has_src_snd0) {
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info force-updated after appmeta refresh rows=%d",
                  snd0_updates);
    }
    log_debug("  [REG] Skip (already present in app.db)");
    return true;
  }

  // REGISTER
  // Give shell/runtime a brief settle window after nullfs mount before install API.
  log_debug("  [STEP][REG] pre-install settle delay start");
  sceKernelUsleep(300000);
  log_debug("  [STEP][REG] pre-install settle delay done");
  if (!register_title(effective_src_path, title_id, title_name, metadata_restaged,
                      has_src_snd0)) {
    return false;
  }

  log_debug("  [STEP][FLOW] completed title=%s", title_id);
  return true;
}

// --- Execution (per discovered candidate) ---
void process_scan_candidates(const scan_candidate_t *candidates,
                             int candidate_count) {
  log_debug("  [STEP][SCAN] processing candidates count=%d", candidate_count);
  for (int i = 0; i < candidate_count; i++) {
    if (should_stop_requested())
      return;

    const scan_candidate_t *c = &candidates[i];
    log_debug("  [STEP][SCAN] candidate idx=%d title=%s in_app_db=%d installed=%d path=%s",
              i, c->title_id, c->in_app_db ? 1 : 0, c->installed ? 1 : 0,
              c->path);
    if (c->installed) {
      log_debug("  [ACTION] Remounting: %s", c->title_name);
    } else {
      log_debug("  [ACTION] Installing: %s (%s)", c->title_name, c->title_id);
    }

    if (mount_and_install(c->path, c->title_id, c->title_name, c->installed,
                          !c->in_app_db)) {
      clear_failed_mount_attempts(c->title_id);
      cache_game_entry(c->path, c->title_id, c->title_name);
    } else {
      uint8_t failed_attempts = bump_failed_mount_attempts(c->title_id);
      if (failed_attempts == MAX_FAILED_MOUNT_ATTEMPTS) {
        log_debug("  [RETRY] limit reached (%u/%u): %s (%s)",
                  (unsigned)failed_attempts,
                  (unsigned)MAX_FAILED_MOUNT_ATTEMPTS, c->title_name,
                  c->title_id);
      }
    }
  }
}
