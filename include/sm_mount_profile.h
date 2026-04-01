#ifndef SM_MOUNT_PROFILE_H
#define SM_MOUNT_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_types.h"

// Mount profile tuple: one complete set of parameters for a single mount attempt
typedef struct {
  // LVD/Mount protocol version
  uint16_t io_version;
  
  // LVD image type (0=UFS_DD, 5=PFS_SAVE, 7=PFS_UNKNOWN, etc)
  uint16_t image_type;
  
  // Raw flags before normalization (0x9, 0x8, 0xD, 0xC, etc)
  uint16_t raw_flags;
  
  // Normalized flags (computed from raw_flags)
  uint16_t normalized_flags;
  
  // Device sector size in bytes (4096, 32768, 65536, etc)
  uint32_t sector_size;
  
  // Secondary unit (typically sector_size, fallback 0x10000)
  uint32_t secondary_unit;
  
  // Filesystem type fstype for nmount ("pfs", "ppr_pfs", "transaction_pfs")
  const char *fstype;
  
  // Budget domain ("game" or "system")
  const char *budgetid;
  
  // Mount key mode (normally "SD", optionally "GD", "AC")
  const char *mkeymode;
  
  // Signature verification flag (0 or 1)
  uint8_t sigverify;
  
  // PlayGo support flag (0 or 1)
  uint8_t playgo;
  
  // Disc flag (0 or 1)
  uint8_t disc;
  
  // Read-only mode flag
  bool mount_read_only;
  
  // Optional human-readable label for logging
  const char *label;
} mount_profile_t;

// Profile validation and helpers
bool mount_profile_validate(const mount_profile_t *profile);
void mount_profile_log(const mount_profile_t *profile, const char *tag);
void mount_profile_format_compact(const mount_profile_t *profile, 
                                  char *buf, size_t buf_size);

// Helper to create a basic profile with sensible defaults
mount_profile_t mount_profile_create_default(image_fs_type_t fs_type, 
                                             bool mount_read_only);

#endif
