#include "sm_mount_profile.h"

#include <stdio.h>
#include <string.h>

#include "sm_log.h"
#include "sm_mount_defs.h"

// Helper to normalize raw flags (mirrors normalize_lvd_raw_flags from sm_image.c)
static uint16_t profile_normalize_lvd_raw_flags(uint16_t raw_flags) {
  if ((raw_flags & 0x800Eu) != 0u) {
    uint32_t raw = (uint32_t)raw_flags;
    uint32_t len = (raw & 0xFFFF8000u) + ((raw & 2u) << 6) +
                   (8u * (raw & 1u)) + (2u * ((raw >> 2) & 1u)) +
                   (2u * (raw & 8u)) + 4u;
    return (uint16_t)len;
  }
  return (uint16_t)(8u * ((uint32_t)raw_flags & 1u) + 4u);
}

// Helper to get image type (mirrors get_lvd_image_type from sm_image.c)
static uint16_t profile_get_lvd_image_type(image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_UFS)
    return LVD_ATTACH_IMAGE_TYPE_UFS_DOWNLOAD_DATA;
  if (fs_type == IMAGE_FS_PFS)
    return LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA;
  if (fs_type == IMAGE_FS_ZFS)
    return LVD_ATTACH_IMAGE_TYPE_ZFS;
  return LVD_ATTACH_IMAGE_TYPE_SINGLE;
}

bool mount_profile_validate(const mount_profile_t *profile) {
  if (!profile)
    return false;
  
  if (profile->io_version != LVD_ATTACH_IO_VERSION_V0)
    return false;
  
  if (!profile->fstype || profile->fstype[0] == '\0')
    return false;
  
  if (!profile->budgetid || profile->budgetid[0] == '\0')
    return false;
  
  if (!profile->mkeymode || profile->mkeymode[0] == '\0')
    return false;
  
  if (profile->sector_size == 0)
    return false;
  
  if (profile->secondary_unit == 0)
    return false;
  
  return true;
}

void mount_profile_log(const mount_profile_t *profile, const char *tag) {
  if (!profile || !tag)
    return;
  
  char compact_buf[128];
  mount_profile_format_compact(profile, compact_buf, sizeof(compact_buf));
  
  log_debug("  [%s] profile: %s", tag, compact_buf);
}

void mount_profile_format_compact(const mount_profile_t *profile,
                                  char *buf, size_t buf_size) {
  if (!profile || !buf || buf_size == 0)
    return;
  
  snprintf(buf, buf_size,
           "img=%u raw=0x%x flags=0x%x sec=%u sec2=%u fstype=%s budget=%s "
           "mkeymode=%s sigv=%u playgo=%u disc=%u ekpfs=%u noatime=%u ro=%d",
           profile->image_type, profile->raw_flags, profile->normalized_flags,
           profile->sector_size, profile->secondary_unit, profile->fstype,
           profile->budgetid, profile->mkeymode, profile->sigverify,
           profile->playgo, profile->disc, profile->include_ekpfs ? 1u : 0u,
           profile->supports_noatime ? 1u : 0u,
           profile->mount_read_only ? 1 : 0);
}

mount_profile_t mount_profile_create_default(image_fs_type_t fs_type,
                                             bool mount_read_only) {
  mount_profile_t profile;
  memset(&profile, 0, sizeof(profile));
  
  profile.io_version = LVD_ATTACH_IO_VERSION_V0;
  profile.image_type = profile_get_lvd_image_type(fs_type);
  profile.sector_size = 4096;
  profile.secondary_unit = 4096;
  profile.fstype = "pfs";
  profile.budgetid = DEVPFS_BUDGET_GAME;
  profile.mkeymode = DEVPFS_MKEYMODE_SD;
  profile.sigverify = (PFS_MOUNT_SIGVERIFY != 0) ? 1u : 0u;
  profile.playgo = (PFS_MOUNT_PLAYGO != 0) ? 1u : 0u;
  profile.disc = (PFS_MOUNT_DISC != 0) ? 1u : 0u;
  profile.include_ekpfs = (fs_type == IMAGE_FS_PFS);
  profile.supports_noatime = true;
  profile.mount_read_only = mount_read_only;
  profile.label = "default";
  
  // Compute raw_flags and normalized_flags based on mount_read_only
  // This mirrors get_lvd_attach_raw_flags logic
  if (fs_type == IMAGE_FS_UFS) {
    profile.raw_flags = mount_read_only ? LVD_ATTACH_RAW_FLAGS_DD_RO
                                        : LVD_ATTACH_RAW_FLAGS_DD_RW;
  } else {
    profile.raw_flags = mount_read_only ? LVD_ATTACH_RAW_FLAGS_SINGLE_RO
                                        : LVD_ATTACH_RAW_FLAGS_SINGLE_RW;
  }
  
  // Normalize flags
  profile.normalized_flags = profile_normalize_lvd_raw_flags(profile.raw_flags);
  
  return profile;
}
