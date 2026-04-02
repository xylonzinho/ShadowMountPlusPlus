#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_image.h"
#include "sm_hash.h"
#include "sm_image_cache.h"
#include "sm_game_cache.h"
#include "sm_log.h"
#include "sm_config_mount.h"
#include "sm_limits.h"
#include "sm_mount_defs.h"
#include "sm_mount_device.h"
#include "sm_filesystem.h"
#include "sm_path_state.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_mount_profile.h"
#include "sm_brute_force.h"
#include "sm_mount_cache.h"
#include "sm_bench.h"
#include "sm_time.h"

static uint32_t get_lvd_sector_size_fallback(image_fs_type_t fs_type) {
  const runtime_config_t *cfg = runtime_config();
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return cfg->lvd_sector_ufs;
  case IMAGE_FS_ZFS:
    return cfg->lvd_sector_zfs;
  case IMAGE_FS_PFS:
    return cfg->lvd_sector_pfs;
  case IMAGE_FS_EXFAT:
  default:
    return cfg->lvd_sector_exfat;
  }
}

static uint32_t get_image_sector_size_override_or_default(
    const char *path, uint32_t fallback) {
  uint32_t override = 0;
  const char *filename = get_filename_component(path);
  if (filename && filename[0] != '\0' &&
      get_image_sector_size_override(filename, &override)) {
    return override;
  }
  return fallback;
}

static uint32_t get_lvd_sector_size(const char *path, image_fs_type_t fs_type) {
  uint32_t fallback = get_image_sector_size_override_or_default(
      path, get_lvd_sector_size_fallback(fs_type));
  struct statfs sfs;
  if (statfs(path, &sfs) != 0)
    return fallback;

  uint64_t fs_cluster_size = (uint64_t)sfs.f_bsize;
  if (fs_cluster_size == 0)
    fs_cluster_size = (uint64_t)sfs.f_iosize;
  if (fs_cluster_size == 0 || fs_cluster_size >= (uint64_t)fallback)
    return fallback;

  return (uint32_t)fs_cluster_size;
}

static uint32_t get_lvd_secondary_unit(const char *path,
                                       image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_EXFAT)
    return LVD_SECONDARY_UNIT_SINGLE_IMAGE;
  return get_lvd_sector_size(path, fs_type);
}

static uint32_t get_md_sector_size(image_fs_type_t fs_type) {
  const runtime_config_t *cfg = runtime_config();
  uint32_t fallback = 0;
  switch (fs_type) {
  case IMAGE_FS_UFS:
    fallback = cfg->md_sector_ufs;
    break;
  case IMAGE_FS_ZFS:
    fallback = cfg->md_sector_zfs;
    break;
  case IMAGE_FS_EXFAT:
  default:
    fallback = cfg->md_sector_exfat;
    break;
  }
  return fallback;
}

static uint32_t get_md_sector_size_for_path(const char *path,
                                            image_fs_type_t fs_type) {
  return get_image_sector_size_override_or_default(path,
                                                   get_md_sector_size(fs_type));
}

static unsigned int get_md_attach_options(bool mount_read_only) {
  unsigned int options = MD_AUTOUNIT | MD_ASYNC;
  if (mount_read_only)
    options |= MD_READONLY;
  return options;
}

static uint16_t get_lvd_attach_raw_flags(image_fs_type_t fs_type,
                                         bool mount_read_only) {
    if (fs_type == IMAGE_FS_UFS) {
    return mount_read_only ? LVD_ATTACH_RAW_FLAGS_DD_RO
                           : LVD_ATTACH_RAW_FLAGS_DD_RW;
  }
  return mount_read_only ? LVD_ATTACH_RAW_FLAGS_SINGLE_RO
                         : LVD_ATTACH_RAW_FLAGS_SINGLE_RW;
}

static unsigned int get_nmount_flags(image_fs_type_t fs_type,
                                     bool mount_read_only,
                                     const char **mount_mode_out) {
  if (fs_type == IMAGE_FS_UFS) {
    if (mount_mode_out)
      *mount_mode_out = mount_read_only ? "dd_ro" : "dd_rw";
    return mount_read_only ? UFS_NMOUNT_FLAG_RO : UFS_NMOUNT_FLAG_RW;
  }
  if (mount_mode_out)
    *mount_mode_out = mount_read_only ? "rdonly" : "rw";
  return mount_read_only ? MNT_RDONLY : 0;
}

static uint16_t normalize_lvd_raw_flags(uint16_t raw_flags) {
  if ((raw_flags & 0x800Eu) != 0u) {
    uint32_t raw = (uint32_t)raw_flags;
    uint32_t len = (raw & 0xFFFF8000u) + ((raw & 2u) << 6) +
                   (8u * (raw & 1u)) + (2u * ((raw >> 2) & 1u)) +
                   (2u * (raw & 8u)) + 4u;
    return (uint16_t)len;
  }
  return (uint16_t)(8u * ((uint32_t)raw_flags & 1u) + 4u);
}

static uint16_t get_lvd_image_type(image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_UFS)
    return LVD_ATTACH_IMAGE_TYPE_UFS_DOWNLOAD_DATA;
  if (fs_type == IMAGE_FS_PFS)
    return LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA;
  if (fs_type == IMAGE_FS_ZFS)
    return LVD_ATTACH_IMAGE_TYPE_ZFS;
  return LVD_ATTACH_IMAGE_TYPE_SINGLE;
}

static uint16_t get_lvd_source_type(const char *path) {
  if (strncmp(path, "/dev/sbram0", strlen("/dev/sbram0")) == 0)
    return LVD_ENTRY_TYPE_SPECIAL;

  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
      return LVD_ENTRY_TYPE_SPECIAL;
  }
  return LVD_ENTRY_TYPE_FILE;
}

// --- Image Path and Naming Helpers ---
static image_fs_type_t detect_image_fs_type(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot)
    return IMAGE_FS_UNKNOWN;
  if (strcasecmp(dot, ".ffpkg") == 0)
    return IMAGE_FS_UFS;
  if (strcasecmp(dot, ".exfat") == 0)
    return IMAGE_FS_EXFAT;
  if (strcasecmp(dot, ".ffpfs") == 0)
    return IMAGE_FS_PFS;
  if (strcasecmp(dot, ".ffzfs") == 0)
    return IMAGE_FS_ZFS;
  return IMAGE_FS_UNKNOWN;
}

bool is_supported_image_file_name(const char *name) {
  return detect_image_fs_type(name) != IMAGE_FS_UNKNOWN;
}

static const char *image_fs_name(image_fs_type_t fs_type) {
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return "ufs";
  case IMAGE_FS_EXFAT:
    return "exfatfs";
  case IMAGE_FS_PFS:
    return "pfs";
  case IMAGE_FS_ZFS:
    return "zfs";
  default:
    return "unknown";
  }
}

void log_fs_stats(const char *tag, const char *path,
                  const char *type_hint) {
  struct statfs sfs;
  if (statfs(path, &sfs) != 0) {
    log_debug("  [%s] FS stats read failed for %s: %s", tag, path,
              strerror(errno));
    return;
  }

  const char *type_name = type_hint;
  if (sfs.f_fstypename[0] != '\0')
    type_name = sfs.f_fstypename;
  if (!type_name)
    type_name = "unknown";

  uint64_t bsize = (uint64_t)sfs.f_bsize;
  uint64_t iosize = (uint64_t)sfs.f_iosize;
  uint64_t blocks = (uint64_t)sfs.f_blocks;
  uint64_t bfree = (uint64_t)sfs.f_bfree;
  uint64_t bavail = (uint64_t)sfs.f_bavail;
  uint64_t files = (uint64_t)sfs.f_files;
  uint64_t ffree = (uint64_t)sfs.f_ffree;
  uint64_t total_bytes = blocks * bsize;
  uint64_t free_bytes = bfree * bsize;
  uint64_t avail_bytes = bavail * bsize;

  log_debug("  [%s] FS stats: path=%s type=%s bsize=%llu iosize=%llu "
            "blocks=%llu bfree=%llu bavail=%llu files=%llu ffree=%llu "
            "flags=0x%lX total=%lluB free=%lluB avail=%lluB",
            tag, path, type_name, (unsigned long long)bsize,
            (unsigned long long)iosize, (unsigned long long)blocks,
            (unsigned long long)bfree, (unsigned long long)bavail,
            (unsigned long long)files, (unsigned long long)ffree,
            (unsigned long)sfs.f_flags, (unsigned long long)total_bytes,
            (unsigned long long)free_bytes, (unsigned long long)avail_bytes);
}

static void strip_extension(const char *filename, char *out, size_t out_size) {
  const char *dot = strrchr(filename, '.');
  size_t len = dot ? (size_t)(dot - filename) : strlen(filename);
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, filename, len);
  out[len] = '\0';
}

static void build_image_mount_point(const char *file_path,
                                    char mount_point[MAX_PATH]) {
  const char *filename = get_filename_component(file_path);
  char base_name[MAX_PATH];
  char mount_name[MAX_PATH];
  strip_extension(filename, base_name, sizeof(base_name));

  size_t base_len = strlen(base_name);
  size_t max_base_len = sizeof(mount_name) - 1u - 9u;
  if (base_len > max_base_len)
    base_len = max_base_len;
  memcpy(mount_name, base_name, base_len);
  mount_name[base_len] = '\0';
  snprintf(mount_name + base_len, sizeof(mount_name) - base_len, "_%08x",
           sm_fnv1a32(file_path));

  snprintf(mount_point, MAX_PATH, "%s/%s", IMAGE_MOUNT_BASE, mount_name);
}

typedef bool (*image_attach_fn)(const char *file_path, image_fs_type_t fs_type,
                                bool mount_read_only, off_t file_size,
                                int *unit_id_out, char *devname_out,
                                size_t devname_size);

typedef struct {
  attach_backend_t id;
  image_attach_fn attach;
} image_backend_ops_t;

static bool attach_md_backend(const char *file_path, image_fs_type_t fs_type,
                              bool mount_read_only, off_t file_size,
                              int *unit_id_out, char *devname_out,
                              size_t devname_size) {
  int md_fd = open(MD_CTRL_PATH, O_RDWR);
  if (md_fd < 0) {
    log_debug("  [IMG][%s] open %s failed: %s",
              attach_backend_name(ATTACH_BACKEND_MD), MD_CTRL_PATH,
              strerror(errno));
    return false;
  }

  struct md_ioctl req;
  memset(&req, 0, sizeof(req));
  req.md_version = MDIOVERSION;
  req.md_type = MD_VNODE;
  req.md_file = (char *)file_path;
  req.md_mediasize = file_size;
  req.md_sectorsize = get_md_sector_size_for_path(file_path, fs_type);
  req.md_options = get_md_attach_options(mount_read_only);

  int last_errno = 0;
  log_debug("  [IMG][%s] attach try: options=0x%x",
            attach_backend_name(ATTACH_BACKEND_MD), req.md_options);
  int ret = ioctl(md_fd, MDIOCATTACH, &req);
  if (ret != 0)
    last_errno = errno;
  close(md_fd);

  if (ret != 0) {
    errno = last_errno;
    log_debug("  [IMG][%s] attach failed: %s (ret: 0x%x)",
              attach_backend_name(ATTACH_BACKEND_MD), strerror(errno), ret);
    return false;
  }

  int unit_id = (int)req.md_unit;
  if (unit_id < 0) {
    log_debug("  [IMG][%s] attach returned invalid unit: %d",
              attach_backend_name(ATTACH_BACKEND_MD), unit_id);
    return false;
  }

  snprintf(devname_out, devname_size, "/dev/md%d", unit_id);
  if (!wait_for_dev_node_state(devname_out, true)) {
    log_debug("  [IMG][%s] device node did not appear: %s",
              attach_backend_name(ATTACH_BACKEND_MD), devname_out);
    (void)detach_attached_unit(ATTACH_BACKEND_MD, unit_id);
    return false;
  }

  log_debug("  [IMG][%s] attach returned unit=%d",
            attach_backend_name(ATTACH_BACKEND_MD), unit_id);
  *unit_id_out = unit_id;
  return true;
}

static bool attach_lvd_backend(const char *file_path, image_fs_type_t fs_type,
                               bool mount_read_only, off_t file_size,
                               int *unit_id_out, char *devname_out,
                               size_t devname_size) {
  int lvd_fd = open(LVD_CTRL_PATH, O_RDWR);
  if (lvd_fd < 0) {
    log_debug("  [IMG][%s] open %s failed: %s",
              attach_backend_name(ATTACH_BACKEND_LVD), LVD_CTRL_PATH,
              strerror(errno));
    return false;
  }

  lvd_ioctl_layer_v0_t layers[LVD_ATTACH_LAYER_COUNT];
  memset(layers, 0, sizeof(layers));
  layers[0].source_type = get_lvd_source_type(file_path);
  layers[0].flags = LVD_ENTRY_FLAG_NO_BITMAP;
  layers[0].path = file_path;
  layers[0].offset = 0;
  layers[0].size = (uint64_t)file_size;

  uint32_t sector_size = get_lvd_sector_size(file_path, fs_type);
  uint32_t secondary_unit = get_lvd_secondary_unit(file_path, fs_type);
  uint16_t raw_flags = get_lvd_attach_raw_flags(fs_type, mount_read_only);
  uint16_t normalized_flags = normalize_lvd_raw_flags(raw_flags);

  lvd_ioctl_attach_v0_t req;
  memset(&req, 0, sizeof(req));
  req.io_version = LVD_ATTACH_IO_VERSION_V0;
  req.image_type = get_lvd_image_type(fs_type);
  req.layer_count = LVD_ATTACH_LAYER_COUNT;
  req.device_size = (uint64_t)file_size;
  req.layers_ptr = layers;
  req.sector_size = sector_size;
  req.secondary_unit = secondary_unit;
  req.flags = normalized_flags;
  req.device_id = -1;

  int last_errno = 0;
  log_debug("  [IMG][%s] attach try: ver=%u sec=%u sec2=%u raw=0x%x "
            "flags=0x%x img=%u",
            attach_backend_name(ATTACH_BACKEND_LVD), req.io_version,
            req.sector_size, req.secondary_unit, raw_flags, req.flags,
            req.image_type);
  int ret = ioctl(lvd_fd, SCE_LVD_IOC_ATTACH_V0, &req);
  if (ret != 0)
    last_errno = errno;
  close(lvd_fd);
  int unit_id = req.device_id;

  if (ret != 0) {
    errno = last_errno;
    log_debug("  [IMG][%s] attach failed: %s (ret: 0x%x)",
              attach_backend_name(ATTACH_BACKEND_LVD), strerror(errno), ret);
    return false;
  }

  if (unit_id < 0) {
    log_debug("  [IMG][%s] attach returned invalid unit: %d",
              attach_backend_name(ATTACH_BACKEND_LVD), unit_id);
    return false;
  }
  log_debug("  [IMG][%s] attach returned unit=%d",
            attach_backend_name(ATTACH_BACKEND_LVD), unit_id);

  snprintf(devname_out, devname_size, "/dev/lvd%d", unit_id);
  if (!wait_for_dev_node_state(devname_out, true)) {
    log_debug("  [IMG][%s] device node did not appear: %s",
              attach_backend_name(ATTACH_BACKEND_LVD), devname_out);
    (void)detach_attached_unit(ATTACH_BACKEND_LVD, unit_id);
    return false;
  }

  *unit_id_out = unit_id;
  return true;
}

static const image_backend_ops_t *get_image_backend_ops(attach_backend_t backend) {
  static const image_backend_ops_t md_ops = {
      .id = ATTACH_BACKEND_MD, .attach = attach_md_backend};
  static const image_backend_ops_t lvd_ops = {
      .id = ATTACH_BACKEND_LVD, .attach = attach_lvd_backend};
  if (backend == ATTACH_BACKEND_MD)
    return &md_ops;
  if (backend == ATTACH_BACKEND_LVD)
    return &lvd_ops;
  return NULL;
}

static bool is_cached_image_path(const char *file_path) {
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (get_image_cache_entry(k, &cached_entry) &&
        strcmp(cached_entry.path, file_path) == 0) {
      return true;
    }
  }
  return false;
}

static bool directory_has_visible_entries(const char *path) {
  DIR *dir = opendir(path);
  if (!dir)
    return false;

  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    found = true;
    break;
  }
  closedir(dir);
  return found;
}

static bool is_image_mount_root_accessible(const char *mount_point,
                                           int *error_out) {
  if (error_out)
    *error_out = 0;

  DIR *dir = opendir(mount_point);
  if (!dir) {
    if (error_out)
      *error_out = errno;
    return false;
  }
  closedir(dir);
  return true;
}

static bool reject_mounted_image_io(const char *file_path,
                                    attach_backend_t attach_backend, int unit_id,
                                    const char *devname,
                                    const char *mount_point, int io_err,
                                    const char *stage) {
  sm_error_set("IMG", io_err, file_path, "Mounted image is unreadable or damaged");
  log_debug("  [IMG][%s] unreadable or damaged mount (%s -> %s, %s): %s",
            attach_backend_name(attach_backend), devname, mount_point, stage,
            strerror(io_err));
  (void)unmount_image(file_path, unit_id, attach_backend);
  errno = io_err;
  return false;
}

static bool prepare_image_mount_retry(const image_cache_entry_t *cached_entry,
                                      char mount_point[MAX_PATH],
                                      char source_path[MAX_PATH]) {
  build_image_mount_point(cached_entry->path, mount_point);
  (void)strlcpy(source_path, cached_entry->path, MAX_PATH);

  if (is_active_image_mount_point(mount_point)) {
    int root_err = 0;
    if (is_image_mount_root_accessible(mount_point, &root_err))
      return false;

    log_debug("  [IMG][%s] mount unreadable, retrying: %s -> %s: %s",
              attach_backend_name(cached_entry->backend), source_path,
              mount_point, strerror(root_err));
    if (!unmount_image(source_path, cached_entry->unit_id, cached_entry->backend))
      return false;
  } else {
    log_debug("  [IMG][%s] mount lost, retrying: %s -> %s",
              attach_backend_name(cached_entry->backend), source_path,
              mount_point);
    clear_cached_game(mount_point);
  }

  clear_missing_param_entry(mount_point);
  return true;
}

static bool reuse_existing_image_mount(const char *file_path,
                                       const char *mount_point,
                                       bool *cache_failed_out) {
  if (cache_failed_out)
    *cache_failed_out = false;

  struct stat mount_st;
  if (stat(mount_point, &mount_st) != 0 || !S_ISDIR(mount_st.st_mode))
    return false;

  attach_backend_t existing_backend = ATTACH_BACKEND_NONE;
  int existing_unit = -1;
  if (resolve_device_from_mount(mount_point, &existing_backend, &existing_unit)) {
    int root_err = 0;
    if (!is_image_mount_root_accessible(mount_point, &root_err)) {
      log_debug("  [IMG][%s] Existing mount unreadable, reattaching: %s -> %s: %s",
                attach_backend_name(existing_backend), file_path, mount_point,
                strerror(root_err));
      if (!unmount_image(file_path, existing_unit, existing_backend)) {
        if (cache_failed_out)
          *cache_failed_out = true;
        return false;
      }
      return false;
    }
    if (!cache_image_mount(file_path, mount_point, existing_unit,
                           existing_backend)) {
      sm_error_set("IMG", ENOSPC, file_path,
                   "Image cache full (%u entries), cannot track mount %s",
                   (unsigned)MAX_IMAGE_MOUNTS, mount_point);
      log_debug("  [IMG] image cache full, refusing unmanaged mount reuse: %s",
                mount_point);
      errno = ENOSPC;
      if (cache_failed_out)
        *cache_failed_out = true;
      return false;
    }
    log_debug("  [IMG][%s] Already mounted: %s",
              attach_backend_name(existing_backend), mount_point);
    return true;
  }

  if (!directory_has_visible_entries(mount_point))
    return false;

  log_debug("  [IMG] Mount point exists and is non-empty but is not an active "
            "mount, reattaching: %s", mount_point);
  return false;
}

static bool stat_image_file(const char *file_path, struct stat *st_out) {
  if (stat(file_path, st_out) != 0) {
    log_debug("  [IMG] stat failed for %s: %s", file_path, strerror(errno));
    return false;
  }
  if (st_out->st_size < 0) {
    log_debug("  [IMG] invalid file size for %s: %lld", file_path,
              (long long)st_out->st_size);
    errno = EINVAL;
    return false;
  }
  return true;
}

static void ensure_mount_dirs(const char *mount_point) {
  mkdir(IMAGE_MOUNT_BASE, 0777);
  mkdir(mount_point, 0777);
}

static attach_backend_t select_image_backend(const runtime_config_t *cfg,
                                             image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_EXFAT)
    return cfg->exfat_backend;
  if (fs_type == IMAGE_FS_UFS)
    return cfg->ufs_backend;
  if (fs_type == IMAGE_FS_ZFS)
    return cfg->zfs_backend;
  return ATTACH_BACKEND_LVD;
}

static bool attach_image_device(const char *file_path, image_fs_type_t fs_type,
                                bool mount_read_only, off_t file_size,
                                attach_backend_t attach_backend, int *unit_id_out,
                                char *devname_out, size_t devname_size) {
  const image_backend_ops_t *backend_ops = get_image_backend_ops(attach_backend);
  if (!backend_ops) {
    log_debug("  [IMG] unsupported attach backend for %s", file_path);
    errno = EINVAL;
    return false;
  }

  if (!backend_ops->attach(file_path, fs_type, mount_read_only, file_size,
                           unit_id_out, devname_out, devname_size)) {
    return false;
  }

  log_debug("  [IMG][%s] Attached as %s", attach_backend_name(attach_backend),
            devname_out);
  return true;
}

static bool perform_image_nmount(const char *file_path, image_fs_type_t fs_type,
                                 attach_backend_t attach_backend, int unit_id,
                                 const char *devname, const char *mount_point,
                                 bool mount_read_only, bool force_mount) {
  struct iovec *iov = NULL;
  unsigned int iovlen = 0;
  char mount_errmsg[256];
  memset(mount_errmsg, 0, sizeof(mount_errmsg));
  const char *sigverify = PFS_MOUNT_SIGVERIFY ? "1" : "0";
  const char *playgo = PFS_MOUNT_PLAYGO ? "1" : "0";
  const char *disc = PFS_MOUNT_DISC ? "1" : "0";
  const char *ekpfs_key = PFS_ZERO_EKPFS_KEY_HEX;

  struct iovec iov_ufs[] = {
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("ufs"), IOVEC_ENTRY("from"),
      IOVEC_ENTRY(devname),      IOVEC_ENTRY("fspath"),
      IOVEC_ENTRY(mount_point),  IOVEC_ENTRY("budgetid"),
      IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  struct iovec iov_exfat[] = {
      IOVEC_ENTRY("from"),       IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),     IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("exfatfs"),
      IOVEC_ENTRY("budgetid"),   IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("large"),      IOVEC_ENTRY("yes"),
      IOVEC_ENTRY("timezone"),   IOVEC_ENTRY("static"),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("ignoreacl"),  IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  struct iovec iov_pfs[] = {
      IOVEC_ENTRY("from"),       IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),     IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("pfs"),
      IOVEC_ENTRY("sigverify"),  IOVEC_ENTRY(sigverify),
      IOVEC_ENTRY("mkeymode"),   IOVEC_ENTRY(PFS_MOUNT_MKEYMODE),
      IOVEC_ENTRY("budgetid"),   IOVEC_ENTRY(PFS_MOUNT_BUDGET_ID),
      IOVEC_ENTRY("playgo"),     IOVEC_ENTRY(playgo),
      IOVEC_ENTRY("disc"),       IOVEC_ENTRY(disc),
      IOVEC_ENTRY("ekpfs"),      IOVEC_ENTRY(ekpfs_key),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  struct iovec iov_zfs[] = {
      IOVEC_ENTRY("from"),       IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),     IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("zfs"),
      IOVEC_ENTRY("budgetid"),   IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  if (fs_type == IMAGE_FS_UFS) {
    iov = iov_ufs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_ufs) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_EXFAT) {
    iov = iov_exfat;
    iovlen = (unsigned int)IOVEC_SIZE(iov_exfat) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_PFS) {
    log_debug("  [IMG][%s] PFS ro=%d budgetid=%s mkeymode=%s "
              "sigverify=%s playgo=%s disc=%s ekpfs=zero",
              attach_backend_name(attach_backend), mount_read_only ? 1 : 0,
              PFS_MOUNT_BUDGET_ID, PFS_MOUNT_MKEYMODE, sigverify, playgo, disc);
    iov = iov_pfs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_pfs) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_ZFS) {
    iov = iov_zfs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_zfs) - (force_mount ? 0u : 2u);
  } else {
    log_debug("  [IMG][%s] unsupported fstype=%s",
              attach_backend_name(attach_backend), image_fs_name(fs_type));
    (void)detach_attached_unit(attach_backend, unit_id);
    errno = EINVAL;
    return false;
  }

  const char *mount_mode = NULL;
  unsigned int mount_flags =
      get_nmount_flags(fs_type, mount_read_only, &mount_mode);
  if (nmount(iov, iovlen, (int)mount_flags) == 0)
    return true;

  int mount_errno = errno;
  if (mount_errmsg[0] != '\0') {
    sm_error_set("IMG", mount_errno, file_path, "%s", mount_errmsg);
    log_debug("  [IMG][%s] nmount %s errmsg: %s",
              attach_backend_name(attach_backend), mount_mode, mount_errmsg);
  }
  log_debug("  [IMG][%s] nmount %s failed: %s",
            attach_backend_name(attach_backend), mount_mode,
            strerror(mount_errno));
  (void)detach_attached_unit(attach_backend, unit_id);
  errno = mount_errno;
  return false;
}

static bool validate_mounted_image(const char *file_path, image_fs_type_t fs_type,
                                   attach_backend_t attach_backend, int unit_id,
                                   const char *devname,
                                   const char *mount_point) {
  struct statfs mounted_sfs;
  if (statfs(mount_point, &mounted_sfs) != 0) {
    return reject_mounted_image_io(file_path, attach_backend, unit_id, devname,
                                   mount_point, errno, "statfs");
  }

  uint32_t min_device_sector =
      (attach_backend == ATTACH_BACKEND_MD) ? get_md_sector_size_for_path(file_path, fs_type)
                                            : get_lvd_sector_size(file_path,
                                                                  fs_type);
  uint64_t fs_block_size = (uint64_t)mounted_sfs.f_bsize;
  if (fs_block_size < (uint64_t)min_device_sector) {
    uint32_t tuned_sector_size = 0;
    bool autotuned =
        fs_block_size <= UINT32_MAX &&
        upsert_image_sector_size_autotune(get_filename_component(file_path),
                                          (uint32_t)fs_block_size,
                                          &tuned_sector_size);
    sm_error_set("IMG", EINVAL, file_path,
                 "Filesystem cluster size (%llu) is smaller than device "
                 "sector size (%u) for %s",
                 (unsigned long long)fs_block_size, min_device_sector, devname);
    log_debug("  [IMG][%s] %s", attach_backend_name(attach_backend),
              sm_last_error()->message);
    if (autotuned) {
      log_debug("  [CFG] image sector autotuned: %s=%u",
                get_filename_component(file_path), tuned_sector_size);
      notify_system("Image mount rejected:\n%s\nCluster size is too small "
                    "(%llu < %u).\nSaved image_sector override: %u.\nTry "
                    "mounting again.",
                    file_path, (unsigned long long)fs_block_size,
                    min_device_sector, tuned_sector_size);
    } else {
      notify_system("Image mount rejected:\n%s\nCluster size is too small "
                    "(%llu < %u).\nUse a larger cluster size in the image or "
                    "lower sector size in config.",
                    file_path, (unsigned long long)fs_block_size,
                    min_device_sector);
    }
    sm_error_mark_notified();
    (void)unmount_image(file_path, unit_id, attach_backend);
    errno = EINVAL;
    return false;
  }

  int root_err = 0;
  if (!is_image_mount_root_accessible(mount_point, &root_err)) {
    return reject_mounted_image_io(file_path, attach_backend, unit_id, devname,
                                   mount_point, root_err, "root access");
  }

  return true;
}

// --- Brute-Force Mount Strategy (PFS two-stage solver) ---
typedef struct {
  uint16_t image_type;
  uint16_t raw_flags;
  uint16_t normalized_flags;
  uint32_t sector_size;
  uint32_t secondary_unit;
} pfs_attach_tuple_t;

typedef struct {
  const char *fstype;
  const char *budgetid;
  const char *mkeymode;
  uint8_t sigverify;
  uint8_t playgo;
  uint8_t disc;
  bool include_ekpfs;
  bool supports_noatime;
  uint8_t key_level;
} pfs_nmount_profile_t;

typedef struct {
  const uint16_t *image_types;
  size_t image_type_count;
  const uint16_t *raw_flags;
  size_t raw_flag_count;
  const uint32_t *sector_sizes;
  size_t sector_size_count;
  bool force_secondary_65536;
  const char *label;
} pfs_attach_pass_t;

typedef struct {
  const runtime_config_t *cfg;
  const char *file_path;
  image_fs_type_t fs_type;
  off_t file_size;
  const char *mount_point;
  bool mount_read_only;
  bool force_mount;
  time_t start_time;
  uint32_t attempt_idx;
  bool limit_logged;
  uint32_t attach_einval_count;
  uint32_t nmount_einval_count;
  uint32_t nmount_semantic_count;
  uint32_t other_fail_count;
} pfs_bruteforce_state_t;

typedef struct {
  char path[MAX_PATH];
  time_t cooldown_until;
  bool cooldown_logged;
  bool valid;
} pfs_cooldown_entry_t;

#define PFS_COOLDOWN_CAPACITY 64
static pfs_cooldown_entry_t g_pfs_cooldowns[PFS_COOLDOWN_CAPACITY];
static time_t g_pfs_global_attempt_window = 0;
static uint32_t g_pfs_global_attempts = 0;

static pfs_cooldown_entry_t *find_or_create_pfs_cooldown(const char *path) {
  for (int i = 0; i < PFS_COOLDOWN_CAPACITY; i++) {
    if (!g_pfs_cooldowns[i].valid)
      continue;
    if (strcmp(g_pfs_cooldowns[i].path, path) == 0)
      return &g_pfs_cooldowns[i];
  }
  for (int i = 0; i < PFS_COOLDOWN_CAPACITY; i++) {
    if (g_pfs_cooldowns[i].valid)
      continue;
    memset(&g_pfs_cooldowns[i], 0, sizeof(g_pfs_cooldowns[i]));
    g_pfs_cooldowns[i].valid = true;
    (void)strlcpy(g_pfs_cooldowns[i].path, path, sizeof(g_pfs_cooldowns[i].path));
    return &g_pfs_cooldowns[i];
  }
  return &g_pfs_cooldowns[0];
}

static bool is_pfs_cooldown_active(const char *path, time_t *remaining_out) {
  if (remaining_out)
    *remaining_out = 0;
  for (int i = 0; i < PFS_COOLDOWN_CAPACITY; i++) {
    if (!g_pfs_cooldowns[i].valid)
      continue;
    if (strcmp(g_pfs_cooldowns[i].path, path) != 0)
      continue;
    time_t now = time(NULL);
    if (g_pfs_cooldowns[i].cooldown_until <= now) {
      g_pfs_cooldowns[i].cooldown_logged = false;
      return false;
    }
    if (remaining_out)
      *remaining_out = g_pfs_cooldowns[i].cooldown_until - now;
    if (!g_pfs_cooldowns[i].cooldown_logged) {
      log_debug("  [IMG][BRUTE] cooldown active (%lds), skip heavy search: %s",
                (long)(g_pfs_cooldowns[i].cooldown_until - now), path);
      g_pfs_cooldowns[i].cooldown_logged = true;
    }
    return true;
  }
  return false;
}

static void set_pfs_cooldown(const char *path, uint32_t seconds) {
  pfs_cooldown_entry_t *entry = find_or_create_pfs_cooldown(path);
  if (!entry)
    return;
  entry->cooldown_until = time(NULL) + (time_t)seconds;
  entry->cooldown_logged = false;
}

static bool stage_a_attach_tuple(const char *file_path, off_t file_size,
                                 const pfs_attach_tuple_t *tuple,
                                 int *unit_id_out, char *devname_out,
                                 size_t devname_size, int *errno_out) {
  if (errno_out)
    *errno_out = 0;

  lvd_ioctl_layer_v0_t layers[LVD_ATTACH_LAYER_COUNT];
  memset(layers, 0, sizeof(layers));
  layers[0].source_type = get_lvd_source_type(file_path);
  layers[0].flags = LVD_ENTRY_FLAG_NO_BITMAP;
  layers[0].path = file_path;
  layers[0].offset = 0;
  layers[0].size = (uint64_t)file_size;

  lvd_ioctl_attach_v0_t req;
  memset(&req, 0, sizeof(req));
  req.io_version = LVD_ATTACH_IO_VERSION_V0;
  req.image_type = tuple->image_type;
  req.layer_count = LVD_ATTACH_LAYER_COUNT;
  req.device_size = (uint64_t)file_size;
  req.layers_ptr = layers;
  req.sector_size = tuple->sector_size;
  req.secondary_unit = tuple->secondary_unit;
  req.flags = tuple->normalized_flags;
  req.device_id = -1;

  int fd = open(LVD_CTRL_PATH, O_RDWR);
  if (fd < 0) {
    if (errno_out)
      *errno_out = errno;
    return false;
  }

  int ret = ioctl(fd, SCE_LVD_IOC_ATTACH_V0, &req);
  int saved_errno = (ret == 0) ? 0 : errno;
  close(fd);
  if (ret != 0 || req.device_id < 0) {
    if (errno_out)
      *errno_out = (saved_errno != 0) ? saved_errno : EINVAL;
    return false;
  }

  snprintf(devname_out, devname_size, "/dev/lvd%d", req.device_id);
  if (!wait_for_dev_node_state(devname_out, true)) {
    (void)detach_attached_unit(ATTACH_BACKEND_LVD, req.device_id);
    if (errno_out)
      *errno_out = ETIMEDOUT;
    return false;
  }

  *unit_id_out = req.device_id;
  return true;
}

static bool stage_b_nmount_profile(const char *mount_point, const char *devname,
                                   bool mount_read_only, bool force_mount,
                                   const pfs_nmount_profile_t *np,
                                   bool include_noatime,
                                   char *mount_errmsg, size_t errmsg_size,
                                   int *errno_out) {
  if (errno_out)
    *errno_out = 0;
  if (mount_errmsg && errmsg_size > 0)
    mount_errmsg[0] = '\0';

  struct iovec iov[48];
  unsigned int iovlen = 0;

  // Mandatory keys.
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY("from");
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY(devname);
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY("fspath");
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY(mount_point);
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY("fstype");
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY(np->fstype);

  // Stage B profile keys.
  if (np->key_level >= 1) {
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("budgetid");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(np->budgetid);
  }
  if (np->key_level >= 2) {
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("mkeymode");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(np->mkeymode);
  }
  if (np->key_level >= 3 || strcmp(np->fstype, "pfs") == 0) {
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("sigverify");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(np->sigverify ? "1" : "0");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("playgo");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(np->playgo ? "1" : "0");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("disc");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(np->disc ? "1" : "0");
  }
  if (strcmp(np->fstype, "pfs") == 0 && np->include_ekpfs) {
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("ekpfs");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(PFS_ZERO_EKPFS_KEY_HEX);
  }

  iov[iovlen++] = (struct iovec)IOVEC_ENTRY("async");
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY(NULL);
  if (include_noatime) {
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("noatime");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(NULL);
  }
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY("automounted");
  iov[iovlen++] = (struct iovec)IOVEC_ENTRY(NULL);

  iov[iovlen++] = (struct iovec)IOVEC_ENTRY("errmsg");
  iov[iovlen].iov_base = (void *)mount_errmsg;
  iov[iovlen].iov_len = errmsg_size;
  iovlen++;

  if (force_mount) {
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY("force");
    iov[iovlen++] = (struct iovec)IOVEC_ENTRY(NULL);
  }

  if (nmount(iov, iovlen, mount_read_only ? MNT_RDONLY : 0) == 0)
    return true;

  if (errno_out)
    *errno_out = errno;
  return false;
}

static bool pfs_bruteforce_limits_reached(pfs_bruteforce_state_t *state) {
  time_t now = time(NULL);
  bool reached =
      state->attempt_idx >= state->cfg->pfs_bruteforce_max_attempts ||
      (uint32_t)(now - state->start_time) >=
          state->cfg->pfs_bruteforce_max_seconds_per_image ||
      g_pfs_global_attempts >=
          state->cfg->pfs_bruteforce_max_global_attempts_per_scan;

  if (!reached || state->limit_logged)
    return reached;

  log_debug("  [IMG][BRUTE] limits reached: attempts=%u elapsed=%us global=%u",
            state->attempt_idx, (unsigned)(now - state->start_time),
            g_pfs_global_attempts);
  state->limit_logged = true;
  return true;
}

static void pfs_bruteforce_sleep(const pfs_bruteforce_state_t *state) {
  if (state->cfg->pfs_bruteforce_sleep_ms > 0)
    sceKernelUsleep(state->cfg->pfs_bruteforce_sleep_ms * 1000u);
}

static void fill_mount_profile_from_tuple(mount_profile_t *profile,
                                          const pfs_attach_tuple_t *tuple,
                                          const pfs_nmount_profile_t *np,
                                          bool supports_noatime,
                                          bool mount_read_only) {
  memset(profile, 0, sizeof(*profile));
  profile->io_version = LVD_ATTACH_IO_VERSION_V0;
  profile->image_type = tuple->image_type;
  profile->raw_flags = tuple->raw_flags;
  profile->normalized_flags = tuple->normalized_flags;
  profile->sector_size = tuple->sector_size;
  profile->secondary_unit = tuple->secondary_unit;
  profile->fstype = np->fstype;
  profile->budgetid = np->budgetid;
  profile->mkeymode = np->mkeymode;
  profile->sigverify = np->sigverify;
  profile->playgo = np->playgo;
  profile->disc = np->disc;
  profile->include_ekpfs = np->include_ekpfs;
  profile->supports_noatime = supports_noatime;
  profile->mount_read_only = mount_read_only;
}

static void count_attach_failure(pfs_bruteforce_state_t *state, int err) {
  if (err == EINVAL)
    state->attach_einval_count++;
  else
    state->other_fail_count++;
}

static void count_nmount_failure(pfs_bruteforce_state_t *state, int err) {
  if (err == EINVAL)
    state->nmount_einval_count++;
  else if (err == EOPNOTSUPP)
    state->nmount_semantic_count++;
  else
    state->other_fail_count++;
}

static bool pfs_try_nmount_profile(pfs_bruteforce_state_t *state,
                                   const pfs_attach_tuple_t *tuple,
                                   const pfs_nmount_profile_t *np,
                                   int *unit_id_io, char *devname,
                                   size_t devname_size,
                                   mount_profile_t *winner_out) {
  bool include_noatime = np->supports_noatime;
  char errmsg[256];
  int nmount_err = 0;
  bool ok = stage_b_nmount_profile(state->mount_point, devname,
                                   state->mount_read_only,
                                   state->force_mount, np, include_noatime,
                                   errmsg, sizeof(errmsg), &nmount_err);
  g_pfs_global_attempts++;
  log_debug("  [IMG][BRUTE] stage=B idx=%u tuple=(img=%u raw=0x%x sec=%u sec2=%u) opts=(fstype=%s budget=%s mkey=%s sig=%u playgo=%u disc=%u ekpfs=%d noatime=%d) result=%s errno=%d",
            state->attempt_idx, tuple->image_type, tuple->raw_flags,
            tuple->sector_size, tuple->secondary_unit, np->fstype,
            np->budgetid ? np->budgetid : "-",
            np->mkeymode ? np->mkeymode : "-", np->sigverify, np->playgo,
            np->disc, np->include_ekpfs ? 1 : 0, include_noatime ? 1 : 0,
            ok ? "NMOUNT_OK" : "NMOUNT_FAIL", nmount_err);
  state->attempt_idx++;

  if (!ok && nmount_err == EINVAL && include_noatime) {
    int retry_err = 0;
    bool retry_ok = stage_b_nmount_profile(state->mount_point, devname,
                                           state->mount_read_only,
                                           state->force_mount, np, false,
                                           errmsg, sizeof(errmsg), &retry_err);
    g_pfs_global_attempts++;
    log_debug("  [IMG][BRUTE] stage=B idx=%u retry=(drop-noatime) tuple=(img=%u raw=0x%x sec=%u sec2=%u) opts=(fstype=%s budget=%s mkey=%s sig=%u playgo=%u disc=%u ekpfs=%d noatime=0) result=%s errno=%d",
              state->attempt_idx, tuple->image_type, tuple->raw_flags,
              tuple->sector_size, tuple->secondary_unit, np->fstype,
              np->budgetid ? np->budgetid : "-",
              np->mkeymode ? np->mkeymode : "-", np->sigverify, np->playgo,
              np->disc, np->include_ekpfs ? 1 : 0,
              retry_ok ? "NMOUNT_OK" : "NMOUNT_FAIL", retry_err);
    state->attempt_idx++;

    ok = retry_ok;
    nmount_err = retry_err;
    include_noatime = false;
  }

  if (ok && validate_mounted_image(state->file_path, state->fs_type,
                                   ATTACH_BACKEND_LVD, *unit_id_io, devname,
                                   state->mount_point)) {
    fill_mount_profile_from_tuple(winner_out, tuple, np,
                                  include_noatime,
                                  state->mount_read_only);
    return true;
  }

  if (ok)
    (void)unmount_image(state->file_path, *unit_id_io, ATTACH_BACKEND_LVD);
  else
    count_nmount_failure(state, nmount_err);

  if (*unit_id_io >= 0)
    (void)detach_attached_unit(ATTACH_BACKEND_LVD, *unit_id_io);
  *unit_id_io = -1;
  if (devname_size > 0)
    devname[0] = '\0';
  return false;
}

static bool pfs_try_attached_tuple_profiles(pfs_bruteforce_state_t *state,
                                            const pfs_attach_tuple_t *tuple,
                                            int *unit_id_io, char *devname,
                                            size_t devname_size,
                                            mount_profile_t *winner_out) {
  static const pfs_nmount_profile_t k_pfs_primary_profiles[] = {
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_GAME,
       .mkeymode = DEVPFS_MKEYMODE_GD,
       .sigverify = 0,
       .playgo = 0,
       .disc = 0,
       .include_ekpfs = true,
        .supports_noatime = true,
       .key_level = 3},
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_GAME,
       .mkeymode = DEVPFS_MKEYMODE_SD,
       .sigverify = 0,
       .playgo = 0,
       .disc = 0,
       .include_ekpfs = true,
      .supports_noatime = true,
       .key_level = 3},
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_GAME,
       .mkeymode = DEVPFS_MKEYMODE_AC,
       .sigverify = 0,
       .playgo = 0,
       .disc = 0,
       .include_ekpfs = true,
      .supports_noatime = true,
       .key_level = 3},
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_SYSTEM,
       .mkeymode = DEVPFS_MKEYMODE_GD,
       .sigverify = 0,
       .playgo = 0,
       .disc = 0,
       .include_ekpfs = true,
      .supports_noatime = true,
       .key_level = 3},
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_GAME,
       .mkeymode = DEVPFS_MKEYMODE_GD,
       .sigverify = 0,
       .playgo = 0,
       .disc = 0,
       .include_ekpfs = false,
      .supports_noatime = true,
       .key_level = 3},
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_GAME,
       .mkeymode = DEVPFS_MKEYMODE_GD,
       .sigverify = 1,
       .playgo = 0,
       .disc = 0,
       .include_ekpfs = true,
      .supports_noatime = true,
       .key_level = 3},
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_GAME,
       .mkeymode = DEVPFS_MKEYMODE_GD,
       .sigverify = 0,
       .playgo = 1,
       .disc = 0,
       .include_ekpfs = true,
      .supports_noatime = true,
       .key_level = 3},
      {.fstype = "pfs",
       .budgetid = DEVPFS_BUDGET_GAME,
       .mkeymode = DEVPFS_MKEYMODE_GD,
       .sigverify = 0,
       .playgo = 0,
       .disc = 1,
       .include_ekpfs = true,
      .supports_noatime = true,
       .key_level = 3},
  };
  static const char *k_fallback_fstypes[] = {"ppr_pfs", "transaction_pfs"};

  for (size_t i = 0; i < sizeof(k_pfs_primary_profiles) / sizeof(k_pfs_primary_profiles[0]); i++) {
    if (pfs_bruteforce_limits_reached(state))
      return false;

    if (pfs_try_nmount_profile(state, tuple, &k_pfs_primary_profiles[i],
                               unit_id_io, devname, devname_size,
                               winner_out)) {
      return true;
    }

    if (pfs_bruteforce_limits_reached(state))
      return false;

    int attach_err = 0;
    if (!stage_a_attach_tuple(state->file_path, state->file_size, tuple,
                              unit_id_io, devname, devname_size,
                              &attach_err)) {
      count_attach_failure(state, attach_err);
      return false;
    }
    pfs_bruteforce_sleep(state);
  }

  for (size_t i = 0; i < sizeof(k_fallback_fstypes) / sizeof(k_fallback_fstypes[0]); i++) {
    uint8_t key_level = 0;
    while (key_level <= 3) {
      if (pfs_bruteforce_limits_reached(state))
        return false;

      pfs_nmount_profile_t np = {
          .fstype = k_fallback_fstypes[i],
          .budgetid = DEVPFS_BUDGET_GAME,
          .mkeymode = DEVPFS_MKEYMODE_GD,
          .sigverify = 0,
          .playgo = 0,
          .disc = 0,
          .include_ekpfs = false,
            .supports_noatime = true,
          .key_level = key_level,
      };
      int unit_before_attempt = *unit_id_io;
      if (pfs_try_nmount_profile(state, tuple, &np, unit_id_io, devname,
                                 devname_size, winner_out)) {
        return true;
      }

      if (pfs_bruteforce_limits_reached(state))
        return false;

      int attach_err = 0;
      if (!stage_a_attach_tuple(state->file_path, state->file_size, tuple,
                                unit_id_io, devname, devname_size,
                                &attach_err)) {
        count_attach_failure(state, attach_err);
        return false;
      }
      pfs_bruteforce_sleep(state);

      if (unit_before_attempt < 0)
        break;
      key_level++;
    }
  }

  return false;
}

static bool pfs_try_attach_pass(pfs_bruteforce_state_t *state,
                                const pfs_attach_pass_t *pass,
                                mount_profile_t *winner_out, int *unit_id_out,
                                char *devname_out, size_t devname_size) {
  for (size_t i = 0; i < pass->image_type_count; i++) {
    for (size_t r = 0; r < pass->raw_flag_count; r++) {
      for (size_t s = 0; s < pass->sector_size_count; s++) {
        if (pfs_bruteforce_limits_reached(state))
          return false;

        uint32_t sec = pass->sector_sizes[s];
        uint32_t sec2 = sec;
        if (pass->force_secondary_65536) {
          if (sec == 65536u)
            continue;
          sec2 = 65536u;
        }

        pfs_attach_tuple_t tuple = {
            .image_type = pass->image_types[i],
            .raw_flags = pass->raw_flags[r],
            .normalized_flags = normalize_lvd_raw_flags(pass->raw_flags[r]),
            .sector_size = sec,
            .secondary_unit = sec2,
        };

        int attach_err = 0;
        int temp_unit = -1;
        char temp_dev[64];
        bool ok = stage_a_attach_tuple(state->file_path, state->file_size,
                                       &tuple, &temp_unit, temp_dev,
                                       sizeof(temp_dev), &attach_err);
        g_pfs_global_attempts++;
        log_debug("  [IMG][BRUTE] stage=A pass=%s idx=%u tuple=(img=%u raw=0x%x flags=0x%x sec=%u sec2=%u) result=%s errno=%d",
                  pass->label, state->attempt_idx, tuple.image_type,
                  tuple.raw_flags, tuple.normalized_flags, tuple.sector_size,
                  tuple.secondary_unit, ok ? "ATTACH_OK" : "ATTACH_FAIL",
                  attach_err);
        state->attempt_idx++;

        if (!ok) {
          count_attach_failure(state, attach_err);
          pfs_bruteforce_sleep(state);
          continue;
        }

        if (pfs_try_attached_tuple_profiles(state, &tuple, &temp_unit,
                                            temp_dev, sizeof(temp_dev),
                                            winner_out)) {
          *unit_id_out = temp_unit;
          (void)strlcpy(devname_out, temp_dev, devname_size);
          return true;
        }

        if (temp_unit >= 0)
          (void)detach_attached_unit(ATTACH_BACKEND_LVD, temp_unit);
        pfs_bruteforce_sleep(state);
      }
    }
  }

  return false;
}

static bool mount_profile_equals(const mount_profile_t *a,
                                 const mount_profile_t *b) {
  if (!a || !b)
    return false;
  if (a->image_type != b->image_type ||
      a->raw_flags != b->raw_flags ||
      a->normalized_flags != b->normalized_flags ||
      a->sector_size != b->sector_size ||
      a->secondary_unit != b->secondary_unit ||
      a->sigverify != b->sigverify ||
      a->playgo != b->playgo ||
      a->disc != b->disc ||
      a->include_ekpfs != b->include_ekpfs ||
      a->supports_noatime != b->supports_noatime ||
      a->mount_read_only != b->mount_read_only)
    return false;

  const char *a_fstype = a->fstype ? a->fstype : "";
  const char *b_fstype = b->fstype ? b->fstype : "";
  const char *a_budget = a->budgetid ? a->budgetid : "";
  const char *b_budget = b->budgetid ? b->budgetid : "";
  const char *a_mkey = a->mkeymode ? a->mkeymode : "";
  const char *b_mkey = b->mkeymode ? b->mkeymode : "";

  return strcmp(a_fstype, b_fstype) == 0 &&
         strcmp(a_budget, b_budget) == 0 &&
         strcmp(a_mkey, b_mkey) == 0;
}

static bool append_unique_profile(mount_profile_t *profiles, int *count,
                                  int max_count,
                                  const mount_profile_t *candidate) {
  if (!profiles || !count || !candidate || *count < 0 || max_count <= 0)
    return false;

  for (int i = 0; i < *count; i++) {
    if (mount_profile_equals(&profiles[i], candidate))
      return false;
  }

  if (*count >= max_count)
    return false;

  profiles[*count] = *candidate;
  (*count)++;
  return true;
}

static bool pfs_mount_with_profile(const char *file_path,
                                   image_fs_type_t fs_type,
                                   off_t file_size,
                                   const char *mount_point,
                                   bool mount_read_only,
                                   bool force_mount,
                                   const mount_profile_t *profile,
                                   int *unit_id_out,
                                   char *devname_out,
                                   size_t devname_size,
                                   uint32_t *mount_ms_out) {
  if (!file_path || !mount_point || !profile || !unit_id_out || !devname_out)
    return false;

  if (mount_ms_out)
    *mount_ms_out = 0;

  pfs_attach_tuple_t tuple = {
      .image_type = profile->image_type,
      .raw_flags = profile->raw_flags,
      .normalized_flags = profile->normalized_flags,
      .sector_size = profile->sector_size,
      .secondary_unit = profile->secondary_unit,
  };

  int attach_err = 0;
  if (!stage_a_attach_tuple(file_path, file_size, &tuple,
                            unit_id_out, devname_out,
                            devname_size, &attach_err)) {
    return false;
  }

  pfs_nmount_profile_t np = {
      .fstype = profile->fstype ? profile->fstype : "pfs",
      .budgetid = profile->budgetid ? profile->budgetid : DEVPFS_BUDGET_GAME,
      .mkeymode = profile->mkeymode ? profile->mkeymode : DEVPFS_MKEYMODE_GD,
      .sigverify = profile->sigverify,
      .playgo = profile->playgo,
      .disc = profile->disc,
      .include_ekpfs = profile->include_ekpfs,
      .supports_noatime = profile->supports_noatime,
      .key_level = 3,
  };

  int nmount_err = 0;
  char errmsg[256];
  uint64_t start_us = monotonic_time_us();
  bool used_noatime = np.supports_noatime;
  bool mounted = stage_b_nmount_profile(mount_point, devname_out,
                                        mount_read_only, force_mount,
                                        &np, used_noatime,
                                        errmsg, sizeof(errmsg), &nmount_err);
  if (!mounted && nmount_err == EINVAL && used_noatime) {
    mounted = stage_b_nmount_profile(mount_point, devname_out,
                                     mount_read_only, force_mount,
                                     &np, false,
                                     errmsg, sizeof(errmsg), &nmount_err);
    used_noatime = false;
  }

  if (mount_ms_out)
    *mount_ms_out = (uint32_t)((monotonic_time_us() - start_us) / 1000u);

  if (!mounted || !validate_mounted_image(file_path, fs_type,
                                          ATTACH_BACKEND_LVD,
                                          *unit_id_out, devname_out,
                                          mount_point)) {
    (void)unmount_image(file_path, *unit_id_out, ATTACH_BACKEND_LVD);
    *unit_id_out = -1;
    if (devname_size > 0)
      devname_out[0] = '\0';
    return false;
  }

  (void)used_noatime;
  return true;
}

static int pfs_collect_working_profiles(const runtime_config_t *cfg,
                                        const char *file_path,
                                        image_fs_type_t fs_type,
                                        off_t file_size,
                                        const char *mount_point,
                                        bool mount_read_only,
                                        bool force_mount,
                                        mount_profile_t *profiles_out,
                                        int max_profiles) {
  if (!cfg || !file_path || !mount_point || !profiles_out || max_profiles <= 0)
    return 0;

  static const uint16_t k_fast_image_types[] = {0};
  static const uint16_t k_fast_fallback_image_types[] = {5};
  static const uint16_t k_secondary_image_types[] = {2, 3, 4, 6};
  static const uint16_t k_last_resort_image_types[] = {1, 7};
  static const uint16_t k_primary_raw_flags[] = {0x9, 0x8};
  static const uint16_t k_last_resort_raw_flags[] = {0xD, 0xC};
  static const uint16_t k_primary_image_types[] = {0, 5};
  static const uint32_t k_sector_candidates[] = {4096u};
  static const pfs_attach_pass_t k_attach_passes[] = {
      {.image_types = k_fast_image_types,
       .image_type_count = sizeof(k_fast_image_types) / sizeof(k_fast_image_types[0]),
       .raw_flags = k_primary_raw_flags,
       .raw_flag_count = 1,
       .sector_sizes = k_sector_candidates,
       .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
       .force_secondary_65536 = false,
       .label = "fast-img0"},
      {.image_types = k_fast_fallback_image_types,
       .image_type_count = sizeof(k_fast_fallback_image_types) / sizeof(k_fast_fallback_image_types[0]),
       .raw_flags = k_primary_raw_flags,
       .raw_flag_count = 1,
       .sector_sizes = k_sector_candidates,
       .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
       .force_secondary_65536 = false,
       .label = "fast-img5"},
      {.image_types = k_fast_image_types,
       .image_type_count = sizeof(k_fast_image_types) / sizeof(k_fast_image_types[0]),
       .raw_flags = &k_primary_raw_flags[1],
       .raw_flag_count = 1,
       .sector_sizes = k_sector_candidates,
       .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
       .force_secondary_65536 = false,
       .label = "expand-img0"},
      {.image_types = k_fast_fallback_image_types,
       .image_type_count = sizeof(k_fast_fallback_image_types) / sizeof(k_fast_fallback_image_types[0]),
       .raw_flags = &k_primary_raw_flags[1],
       .raw_flag_count = 1,
       .sector_sizes = k_sector_candidates,
       .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
       .force_secondary_65536 = false,
       .label = "expand-img5"},
      {.image_types = k_secondary_image_types,
       .image_type_count = sizeof(k_secondary_image_types) / sizeof(k_secondary_image_types[0]),
       .raw_flags = k_primary_raw_flags,
       .raw_flag_count = sizeof(k_primary_raw_flags) / sizeof(k_primary_raw_flags[0]),
       .sector_sizes = k_sector_candidates,
       .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
       .force_secondary_65536 = false,
       .label = "secondary-images"},
      {.image_types = k_last_resort_image_types,
       .image_type_count = sizeof(k_last_resort_image_types) / sizeof(k_last_resort_image_types[0]),
       .raw_flags = k_primary_raw_flags,
       .raw_flag_count = sizeof(k_primary_raw_flags) / sizeof(k_primary_raw_flags[0]),
       .sector_sizes = k_sector_candidates,
       .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
       .force_secondary_65536 = false,
       .label = "last-images"},
      {.image_types = k_primary_image_types,
       .image_type_count = sizeof(k_primary_image_types) / sizeof(k_primary_image_types[0]),
       .raw_flags = k_last_resort_raw_flags,
       .raw_flag_count = sizeof(k_last_resort_raw_flags) / sizeof(k_last_resort_raw_flags[0]),
       .sector_sizes = k_sector_candidates,
       .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
       .force_secondary_65536 = false,
       .label = "last-raws"},
  };

  pfs_bruteforce_state_t state = {
      .cfg = cfg,
      .file_path = file_path,
      .fs_type = fs_type,
      .file_size = file_size,
      .mount_point = mount_point,
      .mount_read_only = mount_read_only,
      .force_mount = force_mount,
      .start_time = time(NULL),
  };

  int found = 0;
  for (size_t pass_idx = 0;
       pass_idx < sizeof(k_attach_passes) / sizeof(k_attach_passes[0]);
       pass_idx++) {
    const pfs_attach_pass_t *pass = &k_attach_passes[pass_idx];
    for (size_t i = 0; i < pass->image_type_count; i++) {
      for (size_t r = 0; r < pass->raw_flag_count; r++) {
        for (size_t s = 0; s < pass->sector_size_count; s++) {
          if (pfs_bruteforce_limits_reached(&state))
            return found;

          pfs_attach_tuple_t tuple = {
              .image_type = pass->image_types[i],
              .raw_flags = pass->raw_flags[r],
              .normalized_flags = normalize_lvd_raw_flags(pass->raw_flags[r]),
              .sector_size = pass->sector_sizes[s],
              .secondary_unit = pass->force_secondary_65536
                                    ? 65536u
                                    : pass->sector_sizes[s],
          };

          int unit_id = -1;
          int attach_err = 0;
          char devname[64];
          memset(devname, 0, sizeof(devname));

          if (!stage_a_attach_tuple(file_path, file_size, &tuple,
                                    &unit_id, devname, sizeof(devname),
                                    &attach_err)) {
            count_attach_failure(&state, attach_err);
            state.attempt_idx++;
            pfs_bruteforce_sleep(&state);
            continue;
          }

          mount_profile_t winner;
          memset(&winner, 0, sizeof(winner));
          if (pfs_try_attached_tuple_profiles(&state, &tuple,
                                              &unit_id, devname,
                                              sizeof(devname), &winner)) {
            if (append_unique_profile(profiles_out, &found, max_profiles,
                                      &winner)) {
              log_debug("  [IMG][PROBE] working profile #%d found for %s",
                        found, file_path);
            }
            (void)unmount_image(file_path, unit_id, ATTACH_BACKEND_LVD);
            unit_id = -1;
          } else if (unit_id >= 0) {
            (void)detach_attached_unit(ATTACH_BACKEND_LVD, unit_id);
          }

          pfs_bruteforce_sleep(&state);
        }
      }
    }
  }

  return found;
}

// --- Image Attach + nmount Pipeline ---
bool mount_image(const char *file_path, image_fs_type_t fs_type) {
  sm_error_clear();
  const runtime_config_t *cfg = runtime_config();
  bool mount_read_only = cfg->mount_read_only;
  bool mount_mode_overridden = false;
  bool force_mount = cfg->force_mount;
  const char *filename = get_filename_component(file_path);
  if (filename[0] != '\0')
    mount_mode_overridden =
        get_image_mode_override(filename, &mount_read_only);

  if (is_cached_image_path(file_path))
    return true;

  char mount_point[MAX_PATH];
  build_image_mount_point(file_path, mount_point);
  struct stat st;
  bool cache_failed = false;
  if (reuse_existing_image_mount(file_path, mount_point, &cache_failed))
    return true;
  if (cache_failed)
    return false;
  if (!stat_image_file(file_path, &st))
    return false;

  log_debug("  [IMG] Mounting image (%s): %s -> %s", image_fs_name(fs_type),
            file_path, mount_point);
  if (mount_mode_overridden) {
    log_debug("  [CFG] Image mode override: %s -> %s", file_path,
              mount_read_only ? "ro" : "rw");
  }

  ensure_mount_dirs(mount_point);

  attach_backend_t attach_backend = select_image_backend(cfg, fs_type);
  log_debug("  [IMG][%s] attach backend selected for %s",
            attach_backend_name(attach_backend), file_path);

  int unit_id = -1;
  char devname[64];
  memset(devname, 0, sizeof(devname));

  // For PFS images with brute-force enabled, use adaptive mount strategy
  if (fs_type == IMAGE_FS_PFS && cfg->pfs_bruteforce_enabled) {
    time_t cooldown_remaining = 0;
    if (is_pfs_cooldown_active(file_path, &cooldown_remaining)) {
      errno = EAGAIN;
      return false;
    }

    time_t now = time(NULL);
    uint32_t scan_window_seconds = cfg->scan_interval_us / 1000000u;
    if (scan_window_seconds == 0)
      scan_window_seconds = 1;
    if (g_pfs_global_attempt_window == 0 ||
        now - g_pfs_global_attempt_window >= (time_t)scan_window_seconds) {
      g_pfs_global_attempt_window = now;
      g_pfs_global_attempts = 0;
    }

    log_debug("  [IMG][BRUTE] start two-stage solver: %s", file_path);

    const char *filename_local = get_filename_component(file_path);
    mount_profile_t probe_profiles[SM_PROBE_MAX_WINNERS];
    memset(probe_profiles, 0, sizeof(probe_profiles));
    int probe_profile_count = bench_load_probe(filename_local,
                                               probe_profiles,
                                               SM_PROBE_MAX_WINNERS);
    if (cfg->pfs_probe_enabled && probe_profile_count == 0) {
      log_debug("  [IMG][PROBE] collecting working profiles for: %s", file_path);
      probe_profile_count = pfs_collect_working_profiles(
          cfg, file_path, fs_type, st.st_size, mount_point,
          mount_read_only, force_mount,
          probe_profiles, SM_PROBE_MAX_WINNERS);
      if (probe_profile_count > 0)
        bench_save_probe(filename_local, probe_profiles, probe_profile_count);
      log_debug("  [IMG][PROBE] completed: %d working profiles", probe_profile_count);
    }

    mount_profile_t cached_profile;
    if (get_cached_mount_profile(filename_local, &cached_profile)) {
      pfs_attach_tuple_t cached_tuple = {
          .image_type = cached_profile.image_type,
          .raw_flags = cached_profile.raw_flags,
          .normalized_flags = cached_profile.normalized_flags,
          .sector_size = cached_profile.sector_size,
          .secondary_unit = cached_profile.secondary_unit,
      };
      int cached_err = 0;
      if (stage_a_attach_tuple(file_path, st.st_size, &cached_tuple, &unit_id,
                               devname, sizeof(devname), &cached_err)) {
        char cached_errmsg[256];
        int nmount_err = 0;
        pfs_nmount_profile_t cp = {
            .fstype = cached_profile.fstype ? cached_profile.fstype : "pfs",
            .budgetid = cached_profile.budgetid ? cached_profile.budgetid : DEVPFS_BUDGET_GAME,
            .mkeymode = cached_profile.mkeymode ? cached_profile.mkeymode : DEVPFS_MKEYMODE_SD,
            .sigverify = cached_profile.sigverify,
            .playgo = cached_profile.playgo,
            .disc = cached_profile.disc,
            .include_ekpfs = cached_profile.include_ekpfs,
            .supports_noatime = cached_profile.supports_noatime,
            .key_level = 3,
        };
        bool cached_ok = stage_b_nmount_profile(
            mount_point, devname, mount_read_only, force_mount, &cp,
            cp.supports_noatime, cached_errmsg, sizeof(cached_errmsg),
            &nmount_err);
        bool cached_used_noatime = cp.supports_noatime;
        if (!cached_ok && nmount_err == EINVAL && cached_used_noatime) {
          cached_ok = stage_b_nmount_profile(
              mount_point, devname, mount_read_only, force_mount, &cp, false,
              cached_errmsg, sizeof(cached_errmsg), &nmount_err);
          cached_used_noatime = false;
        }
        if (cached_ok && validate_mounted_image(file_path, fs_type,
                                                ATTACH_BACKEND_LVD, unit_id,
                                                devname, mount_point)) {
          if (cached_profile.supports_noatime != cached_used_noatime) {
            cached_profile.supports_noatime = cached_used_noatime;
            (void)cache_mount_profile(filename_local, &cached_profile);
          }
          log_debug("  [IMG][BRUTE] cached winner reused: %s (noatime=%d)",
                    file_path, cached_used_noatime ? 1 : 0);
          goto mount_success;
        }
        (void)unmount_image(file_path, unit_id, ATTACH_BACKEND_LVD);
        unit_id = -1;
        memset(devname, 0, sizeof(devname));
      }
    }

    static const uint16_t k_fast_image_types[] = {0};
    static const uint16_t k_fast_fallback_image_types[] = {5};
    static const uint16_t k_secondary_image_types[] = {2, 3, 4, 6};
    static const uint16_t k_last_resort_image_types[] = {1, 7};
    static const uint16_t k_primary_raw_flags[] = {0x9, 0x8};
    static const uint16_t k_last_resort_raw_flags[] = {0xD, 0xC};
    static const uint16_t k_primary_image_types[] = {0, 5};
    static const uint32_t k_sector_candidates[] = {4096u};
    static const pfs_attach_pass_t k_attach_passes[] = {
        {.image_types = k_fast_image_types,
         .image_type_count = sizeof(k_fast_image_types) / sizeof(k_fast_image_types[0]),
         .raw_flags = k_primary_raw_flags,
         .raw_flag_count = 1,
         .sector_sizes = k_sector_candidates,
         .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
         .force_secondary_65536 = false,
         .label = "fast-img0"},
        {.image_types = k_fast_fallback_image_types,
         .image_type_count = sizeof(k_fast_fallback_image_types) / sizeof(k_fast_fallback_image_types[0]),
         .raw_flags = k_primary_raw_flags,
         .raw_flag_count = 1,
         .sector_sizes = k_sector_candidates,
         .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
         .force_secondary_65536 = false,
         .label = "fast-img5"},
        {.image_types = k_fast_image_types,
         .image_type_count = sizeof(k_fast_image_types) / sizeof(k_fast_image_types[0]),
         .raw_flags = &k_primary_raw_flags[1],
         .raw_flag_count = 1,
         .sector_sizes = k_sector_candidates,
         .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
         .force_secondary_65536 = false,
         .label = "expand-img0"},
        {.image_types = k_fast_fallback_image_types,
         .image_type_count = sizeof(k_fast_fallback_image_types) / sizeof(k_fast_fallback_image_types[0]),
         .raw_flags = &k_primary_raw_flags[1],
         .raw_flag_count = 1,
         .sector_sizes = k_sector_candidates,
         .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
         .force_secondary_65536 = false,
         .label = "expand-img5"},
        {.image_types = k_secondary_image_types,
         .image_type_count = sizeof(k_secondary_image_types) / sizeof(k_secondary_image_types[0]),
         .raw_flags = k_primary_raw_flags,
         .raw_flag_count = sizeof(k_primary_raw_flags) / sizeof(k_primary_raw_flags[0]),
         .sector_sizes = k_sector_candidates,
         .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
         .force_secondary_65536 = false,
         .label = "secondary-images"},
        {.image_types = k_last_resort_image_types,
         .image_type_count = sizeof(k_last_resort_image_types) / sizeof(k_last_resort_image_types[0]),
         .raw_flags = k_primary_raw_flags,
         .raw_flag_count = sizeof(k_primary_raw_flags) / sizeof(k_primary_raw_flags[0]),
         .sector_sizes = k_sector_candidates,
         .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
         .force_secondary_65536 = false,
         .label = "last-images"},
        {.image_types = k_primary_image_types,
         .image_type_count = sizeof(k_primary_image_types) / sizeof(k_primary_image_types[0]),
         .raw_flags = k_last_resort_raw_flags,
         .raw_flag_count = sizeof(k_last_resort_raw_flags) / sizeof(k_last_resort_raw_flags[0]),
         .sector_sizes = k_sector_candidates,
         .sector_size_count = sizeof(k_sector_candidates) / sizeof(k_sector_candidates[0]),
         .force_secondary_65536 = false,
         .label = "last-raws"},
    };

    bool mounted = false;
    mount_profile_t winner;
    memset(&winner, 0, sizeof(winner));
    pfs_bruteforce_state_t brute_state = {
        .cfg = cfg,
        .file_path = file_path,
        .fs_type = fs_type,
        .file_size = st.st_size,
        .mount_point = mount_point,
        .mount_read_only = mount_read_only,
        .force_mount = force_mount,
        .start_time = time(NULL),
    };

    for (size_t pass_idx = 0;
         pass_idx < sizeof(k_attach_passes) / sizeof(k_attach_passes[0]) &&
         !mounted;
         pass_idx++) {
      mounted = pfs_try_attach_pass(&brute_state, &k_attach_passes[pass_idx],
                                    &winner, &unit_id, devname,
                                    sizeof(devname));
    }

    if (mounted) {
      (void)cache_mount_profile(filename_local, &winner);
      log_debug("  [IMG][BRUTE] winner selected: img=%u raw=0x%x flags=0x%x sec=%u sec2=%u fstype=%s budget=%s mkey=%s ekpfs=%u noatime=%u",
                winner.image_type, winner.raw_flags, winner.normalized_flags,
                winner.sector_size, winner.secondary_unit,
                winner.fstype ? winner.fstype : "pfs",
                winner.budgetid ? winner.budgetid : DEVPFS_BUDGET_GAME,
                winner.mkeymode ? winner.mkeymode : DEVPFS_MKEYMODE_SD,
                winner.include_ekpfs ? 1u : 0u,
                winner.supports_noatime ? 1u : 0u);

      if (cfg->pfs_bench_enabled) {
        int profile_count = probe_profile_count;
        if (profile_count <= 0) {
          probe_profiles[0] = winner;
          profile_count = 1;
        }

        bench_result_t existing[SM_PROBE_MAX_WINNERS];
        memset(existing, 0, sizeof(existing));
        int next_idx = 0;
        bool bench_done = false;
        (void)bench_load_results(filename_local, existing,
                                 SM_PROBE_MAX_WINNERS,
                                 &next_idx, &bench_done);

        if (!bench_done && next_idx >= 0 && next_idx < profile_count) {
          int target_idx = next_idx;
          mount_profile_t target_profile = probe_profiles[target_idx];
          mount_profile_t original_profile = winner;

          uint32_t mount_ms = 0;
          bool mount_ok_for_bench = mount_profile_equals(&winner, &target_profile);
          if (!mount_ok_for_bench) {
            (void)unmount_image(file_path, unit_id, ATTACH_BACKEND_LVD);
            unit_id = -1;
            memset(devname, 0, sizeof(devname));
            mount_ok_for_bench = pfs_mount_with_profile(
                file_path, fs_type, st.st_size, mount_point,
                mount_read_only, force_mount, &target_profile,
                &unit_id, devname, sizeof(devname), &mount_ms);
            if (mount_ok_for_bench)
              winner = target_profile;
          }

          bench_result_t result;
          memset(&result, 0, sizeof(result));
          result.profile = target_profile;
          result.mount_ok = mount_ok_for_bench;
          result.mount_ms = mount_ms;
          if (mount_ok_for_bench) {
            (void)bench_run_mounted(mount_point, cfg, &result);
          } else {
            result.any_failed = true;
            result.score_ms = 0;
          }

          bench_result_t snapshot[SM_PROBE_MAX_WINNERS];
          memset(snapshot, 0, sizeof(snapshot));
          (void)bench_load_results(filename_local, snapshot,
                                   SM_PROBE_MAX_WINNERS,
                                   NULL, NULL);
          snapshot[target_idx] = result;

          bool bench_complete = (target_idx + 1 >= profile_count);
          int best_idx = bench_complete ? bench_find_best(snapshot, profile_count)
                                        : -1;
          (void)bench_save_result(filename_local, target_idx, &result,
                                  profile_count, bench_complete, best_idx);

          if (bench_complete) {
            bench_log_report(filename_local, snapshot, profile_count, best_idx);
            if (best_idx >= 0)
              (void)cache_mount_profile(filename_local,
                                        &snapshot[best_idx].profile);
          }

          mount_profile_t desired_profile = original_profile;
          if (bench_complete && best_idx >= 0)
            desired_profile = snapshot[best_idx].profile;

          if (!mount_profile_equals(&winner, &desired_profile)) {
            (void)unmount_image(file_path, unit_id, ATTACH_BACKEND_LVD);
            unit_id = -1;
            memset(devname, 0, sizeof(devname));
            uint32_t remount_ms = 0;
            if (pfs_mount_with_profile(file_path, fs_type, st.st_size,
                                       mount_point, mount_read_only,
                                       force_mount, &desired_profile,
                                       &unit_id, devname, sizeof(devname),
                                       &remount_ms)) {
              (void)remount_ms;
              winner = desired_profile;
            }
          }
        }
      }

      notify_system_info("PFS mounted:\n%s", file_path);
      attach_backend = ATTACH_BACKEND_LVD;
      goto mount_success;
    }

    set_pfs_cooldown(file_path, cfg->pfs_bruteforce_cooldown_seconds);
    log_debug("  [IMG][BRUTE] exhausted summary: attach_e22=%u nmount_e22=%u nmount_e96=%u other=%u attempts=%u",
              brute_state.attach_einval_count, brute_state.nmount_einval_count,
              brute_state.nmount_semantic_count, brute_state.other_fail_count,
              brute_state.attempt_idx);
    log_debug("  [IMG][BRUTE] all profiles failed, moving to next image");
    return false;
  }

  // Standard mount flow for non-PFS or brute-force disabled
  if (!attach_image_device(file_path, fs_type, mount_read_only, st.st_size,
                           attach_backend, &unit_id, devname, sizeof(devname))) {
    return false;
  }
  if (!perform_image_nmount(file_path, fs_type, attach_backend, unit_id, devname,
                            mount_point, mount_read_only, force_mount)) {
    return false;
  }

mount_success:
  if (!validate_mounted_image(file_path, fs_type, attach_backend, unit_id, devname,
                              mount_point)) {
    return false;
  }

  log_debug("  [IMG][%s] Mounted (%s) %s -> %s",
            attach_backend_name(attach_backend), image_fs_name(fs_type),
            devname, mount_point);
  log_fs_stats("IMG", mount_point, image_fs_name(fs_type));

  if (!cache_image_mount(file_path, mount_point, unit_id, attach_backend)) {
    sm_error_set("IMG", ENOSPC, file_path,
                 "Image cache full (%u entries), rolling back mount",
                 (unsigned)MAX_IMAGE_MOUNTS);
    log_debug("  [IMG] image cache full, rolling back mount: %s -> %s",
              file_path, mount_point);
    (void)unmount_image(file_path, unit_id, attach_backend);
    errno = ENOSPC;
    return false;
  }
  return true;
}

bool unmount_image(const char *file_path, int unit_id, attach_backend_t backend) {
  char mount_point[MAX_PATH];
  build_image_mount_point(file_path, mount_point);
  int resolved_unit = unit_id;
  attach_backend_t resolved_backend = backend;

  if (resolved_unit < 0 || resolved_backend == ATTACH_BACKEND_NONE) {
    if (!resolve_device_from_mount(mount_point, &resolved_backend,
                                   &resolved_unit)) {
      resolved_backend = ATTACH_BACKEND_NONE;
      resolved_unit = -1;
    }
  }

  log_debug("  [IMG][%s] unmount start: source=%s mount=%s unit=%d",
            attach_backend_name(resolved_backend), file_path, mount_point,
            resolved_unit);

  // Remove mount.lnk and unmount /system_ex/app/<titleid> that point to this
  // source before unmounting the virtual disk itself.
  cleanup_mount_links(mount_point, true);
  clear_cached_game(mount_point);

  // Unmount stacked layers (unionfs over image fs).
  for (int i = 0; i < MAX_LAYERED_UNMOUNT_ATTEMPTS; i++) {
    if (!is_active_image_mount_point(mount_point))
      break;
    if (unmount(mount_point, 0) == 0)
      continue;
    if (errno == ENOENT || errno == EINVAL)
      break;
    if (unmount(mount_point, MNT_FORCE) != 0 && errno != ENOENT &&
        errno != EINVAL) {
      log_debug("  [IMG][%s] unmount failed for %s: %s",
                attach_backend_name(resolved_backend), mount_point,
                strerror(errno));
      return false;
    }
  }

  if (is_active_image_mount_point(mount_point)) {
    log_debug("  [IMG][%s] unmount incomplete for %s",
              attach_backend_name(resolved_backend), mount_point);
    return false;
  }

  bool detach_ok = true;
  if (resolved_backend != ATTACH_BACKEND_NONE && resolved_unit >= 0)
    detach_ok = detach_attached_unit(resolved_backend, resolved_unit);

  if (rmdir(mount_point) == 0) {
    log_debug("  [IMG] Removed mount directory: %s", mount_point);
    log_debug("  [IMG][%s] unmount complete: source=%s mount=%s unit=%d",
              attach_backend_name(resolved_backend), file_path, mount_point,
              resolved_unit);
    return detach_ok;
  }

  int err = errno;
  if (err == ENOENT) {
    log_debug("  [IMG][%s] unmount complete: source=%s mount=%s unit=%d",
              attach_backend_name(resolved_backend), file_path, mount_point,
              resolved_unit);
    return detach_ok;
  }
  if (err == ENOTEMPTY || err == EBUSY) {
    log_debug("  [IMG] Mount directory not removed (%s): %s",
              strerror(err), mount_point);
    if (detach_ok) {
      log_debug("  [IMG][%s] unmount complete with leftover dir: source=%s "
                "mount=%s unit=%d",
                attach_backend_name(resolved_backend), file_path, mount_point,
                resolved_unit);
    }
    return detach_ok;
  }
  log_debug("  [IMG] Failed to remove mount directory %s: %s", mount_point,
            strerror(err));
  return detach_ok;
}

void cleanup_stale_image_mounts(void) {
  if (should_stop_requested())
    return;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (should_stop_requested())
      return;
    if (!get_image_cache_entry(k, &cached_entry))
      continue;

    if (!path_exists(cached_entry.path)) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      if (unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend))
        invalidate_image_cache_entry(k);
      continue;
    }

    image_fs_type_t fs_type = detect_image_fs_type(cached_entry.path);
    char mount_point[MAX_PATH];
    char source_path[MAX_PATH];
    if (!prepare_image_mount_retry(&cached_entry, mount_point, source_path))
      continue;

    invalidate_image_cache_entry(k);
    if (mount_image(source_path, fs_type)) {
      clear_image_mount_attempts(source_path);
      continue;
    }

    int mount_err = errno;
    if (bump_image_mount_attempts(source_path) == 1 && !sm_error_notified()) {
      notify_image_mount_failed(source_path, mount_err);
    }
  }
}

void cleanup_stale_image_mounts_for_root(const char *root) {
  if (!root || root[0] == '\0') {
    cleanup_stale_image_mounts();
    return;
  }

  if (should_stop_requested())
    return;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (should_stop_requested())
      return;
    if (!get_image_cache_entry(k, &cached_entry))
      continue;
    if (!path_matches_root_or_child(cached_entry.path, root) &&
        !path_matches_root_or_child(cached_entry.mount_point, root)) {
      continue;
    }

    if (!path_exists(cached_entry.path)) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      if (unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend))
        invalidate_image_cache_entry(k);
      continue;
    }

    image_fs_type_t fs_type = detect_image_fs_type(cached_entry.path);
    char mount_point[MAX_PATH];
    char source_path[MAX_PATH];
    if (!prepare_image_mount_retry(&cached_entry, mount_point, source_path))
      continue;

    invalidate_image_cache_entry(k);
    if (mount_image(source_path, fs_type)) {
      clear_image_mount_attempts(source_path);
      continue;
    }

    int mount_err = errno;
    if (bump_image_mount_attempts(source_path) == 1 && !sm_error_notified()) {
      notify_image_mount_failed(source_path, mount_err);
    }
  }
}

void cleanup_mount_dirs(void) {
  DIR *d = opendir(IMAGE_MOUNT_BASE);
  if (!d) {
    if (errno != ENOENT)
      log_debug("  [IMG] open %s failed: %s", IMAGE_MOUNT_BASE, strerror(errno));
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested())
      break;
    if (entry->d_name[0] == '.')
      continue;

    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", IMAGE_MOUNT_BASE,
             entry->d_name);

    bool is_dir = false;
    if (entry->d_type == DT_DIR) {
      is_dir = true;
    } else if (entry->d_type == DT_UNKNOWN) {
      struct stat st;
      if (stat(full_path, &st) == 0)
        is_dir = S_ISDIR(st.st_mode);
    }
    if (!is_dir)
      continue;

    if (rmdir(full_path) == 0) {
      log_debug("  [IMG] removed empty mount dir: %s", full_path);
      continue;
    }
    if (errno == ENOTEMPTY || errno == EBUSY || errno == ENOENT)
      continue;
    log_debug("  [IMG] failed to remove mount dir %s: %s", full_path,
              strerror(errno));
  }

  closedir(d);
}

void maybe_mount_image_file(const char *full_path, const char *display_name,
                            bool *unstable_out) {
  image_fs_type_t fs_type = detect_image_fs_type(display_name);
  if (fs_type == IMAGE_FS_UNKNOWN)
    return;
  if (fs_type == IMAGE_FS_PFS) {
    time_t remaining = 0;
    if (is_pfs_cooldown_active(full_path, &remaining))
      return;
  }
  if (!is_source_stable_for_mount(full_path, display_name, "IMG")) {
    if (unstable_out)
      *unstable_out = true;
    return;
  }
  if (is_image_mount_limited(full_path))
    return;

  if (mount_image(full_path, fs_type)) {
    clear_image_mount_attempts(full_path);
    return;
  }

  int mount_err = errno;
  if (mount_err == EAGAIN)
    return;
  if (bump_image_mount_attempts(full_path) == 1 && !sm_error_notified()) {
    notify_image_mount_failed(full_path, mount_err);
  }
}

bool shutdown_image_mounts(void) {
  for (int pass = 0; pass < MAX_LAYERED_UNMOUNT_ATTEMPTS; pass++) {
    bool any_remaining = false;
    bool progress = false;

    for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
      image_cache_entry_t cached_entry;
      if (!get_image_cache_entry(k, &cached_entry))
        continue;

      if (unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend)) {
        invalidate_image_cache_entry(k);
        progress = true;
        continue;
      }

      any_remaining = true;
      log_debug("  [IMG] shutdown unmount pending (%d/%d): %s", pass + 1,
                MAX_LAYERED_UNMOUNT_ATTEMPTS, cached_entry.path);
    }

    if (!any_remaining)
      return true;
    if (!progress)
      break;
  }

  return false;
}
