# PFS Adaptive Mount Strategy - Implementation Guide

## Overview
When a `.ffpfs` (PFS) image mount fails or you want to discover the exact parameters needed, ShadowMount now automatically tries different parameter combinations until one succeeds. This is temporary—the winning combination is cached for future mounts.

## What Gets Tried (Pass 1 Implementation)

### Stage A: Fast Path (High Priority)
- **Image Types:** 0, 5, 7
- **Raw Flags:** 0x9, 0x8
- **Sector Sizes:** 4096, 32768
- **Filesystem Types:** "pfs", "ppr_pfs"

### Stage B: Expanded Path (If Stage A Fails)
- **Image Types:** 1, 2, 3, 4, 6, 8, 9, 10, 11, 12, 0xA, 0xB, 0xC
- **Raw Flags:** 0xD, 0xC
- **Sector Sizes:** 65536, 16384, 8192
- **Filesystem Types:** "transaction_pfs"

Combined: ~50-100 combinations in roughly priority order.

## What's Always Fixed (Pass 1)
- Budget ID: "game"
- Mount Key Mode: "SD"
- Signature Verify: 0
- PlayGo: 0
- Disc: 0
- EKPFS: all-zeros key

(Pass 2 can vary these if needed.)

## Configuration

In `/data/shadowmount/config.ini`:

```ini
# Enable/disable PFS discovery (default: enabled)
pfs_bruteforce_enabled=1

# Wait between failed attempts (milliseconds, default: 3000)
pfs_bruteforce_sleep_ms=3000

# Max attempts per image (default: 20) 
pfs_bruteforce_max_attempts=20

# Max seconds per image (default: 60)
pfs_bruteforce_max_seconds_per_image=60
```

## Cache Location

`/data/shadowmount/autotune.ini`

Entry format:
```ini
mount_profile=MyGame.ffpfs:v0:5:0x8:0x8:0x1C:4096:pfs:game:SD:0:0:0:1
```

Breakdown:
- `v0` — Protocol version
- `5` — Image type
- `0x8` — Raw flags
- `0x8` — Raw flags (stored redundantly)
- `0x1C` — Normalized flags
- `4096` — Sector size
- `pfs` — Filesystem type
- `game` — Budget ID
- `SD` — Mount key mode
- `0` — Sigverify
- `0` — Playgo
- `0` — Disc
- `1` — Mount read-only

## Log Output Format

### Each Attempt
```
[IMG][BRUTE] attempt=2/50 result=NMOUNT_FAILED errno=22 profile=(img=5 raw=0x8 flags=0x1C sec=4096 fstype=pfs)
```

### On Success
```
[IMG][BRUTE] profile selected and cached: img=5 raw=0x8 flags=0x1C sec=4096 fstype=pfs budget=game ...
```

### All Profiles Failed
```
[IMG][BRUTE] all profiles failed, moving to next image
```

### Cached Profile Reused
```
[IMG][BRUTE] trying cached profile first
```

## Workflow Example

1. **First Mount of Unknown PFS:**
   ```
   [IMG] Mounting image (pfs): /mnt/games/MyTitle.ffpfs -> /mnt/image/mytitle_xyz1
   [IMG][BRUTE] starting adaptive mount strategy for /mnt/games/MyTitle.ffpfs
   [IMG][BRUTE] trying profile: img=0 raw=0x9 flags=0x1C sec=4096 fstype=pfs
   [IMG][BRUTE] attempt=1/50 result=ATTACH_FAILED errno=22 ...
   [IMG][BRUTE] trying profile: img=0 raw=0x9 flags=0x1C sec=4096 fstype=ppr_pfs
   [IMG][BRUTE] attempt=2/50 result=NMOUNT_FAILED errno=35 ...
   [IMG][BRUTE] trying profile: img=5 raw=0x8 flags=0x1C sec=4096 fstype=pfs
   [IMG][BRUTE] attempt=3/50 result=OK errno=0 profile=(img=5 raw=0x8...)
   [IMG][BRUTE] profile selected and cached: img=5 raw=0x8 flags=0x1C sec=4096 fstype=pfs
   [IMG] Mounted (pfs) /dev/lvd2 -> /mnt/image/mytitle_xyz1
   ```

2. **Second Mount of Same Image:**
   ```
   [IMG] Mounting image (pfs): /mnt/games/MyTitle.ffpfs -> /mnt/image/mytitle_xyz1
   [IMG][BRUTE] starting adaptive mount strategy for /mnt/games/MyTitle.ffpfs
   [IMG][BRUTE] trying cached profile first
   [IMG][BRUTE] cached profile succeeded, validating mount
   [IMG][BRUTE] profile selected and cached: img=5 raw=0x8 flags=0x1C sec=4096 fstype=pfs
   [IMG] Mounted (pfs) /dev/lvd2 -> /mnt/image/mytitle_xyz1
   ```

## Analysis: What to Look For

When analyzing logs after a PFS discovery:

1. **What image_type worked?** (0, 5, 7, or 1-12)
2. **What raw_flags worked?** (0x8, 0x9, 0xC, 0xD)
3. **What sector_size worked?** (4096, 8192, 16384, 32768, 65536)
4. **What fstype worked?** (pfs, ppr_pfs, transaction_pfs)

Patterns across multiple games/firmwares will tell you:
- Are certain image_types more common for certain game types?
- Do certain raw_flags correlate with firmware versions?
- Does sector size depend on the original dump method?
- Is fstype determined by the image metadata?

## Pass 2 Expansion (Future)

Once you identify patterns, Pass 2 can add:
- Budget ID variations: "system" (for system/app images)
- Mount key mode variations: "GD", "AC"
- Different signature verify / playgo / disc flags

But for now, the Pass 1 approach keeps things simple and focuses on discovering the core three parameters that vary most: **image_type, raw_flags, sector_size, fstype**.

## Implementation Files

- **Headers:**
  - `include/sm_mount_profile.h` — Profile struct + helpers
  - `include/sm_brute_force.h` — Candidate generation + attempt tracking
  - `include/sm_mount_cache.h` — Cache lookup/update

- **Implementation:**
  - `src/sm_mount_profile.c`
  - `src/sm_brute_force.c`
  - `src/sm_mount_cache.c`

- **Integration:**
  - `src/sm_image.c` — Updated `mount_image()` with brute-force wrapper
  - `src/sm_config_mount.c` — Config key parsing
  - `config.ini.example` — Documentation
  - `README.md` — User documentation

## Control Flow (Simplified)

```
mount_image( file_path, IMAGE_FS_PFS )
  ↓
  if pfs_bruteforce_enabled and IMAGE_FS_PFS:
    ├─ check cached_profile()
    │  ├─ if cached exists: try_brute_mount_with_profile(cached)
    │  │  └─ if success: cache and return
    │  └─ if cached failed: continue to matrix
    ├─ generate_candidates(stage=A) → candidates[]
    ├─ for each candidate in stage_a:
    │  ├─ try_brute_mount_with_profile()
    │  │  ├─ lvd attach with profile params
    │  │  ├─ nmount with profile fstype
    │  │  └─ validate
    │  ├─ if success: cache_profile() and return
    │  └─ sleep 3s, record attempt
    ├─ if stage A failed, repeat with stage B
    ├─ if all failed: return error
  else:
    └─ standard mount flow (unchanged)
```

## Fast Discovery Tips

- **First run:** Expect 5-60 seconds to discover parameters (depending on which stage succeeds)
- **Subsequent runs:** <1 second (cache hit)
- **Disable for testing:** Set `pfs_bruteforce_enabled=0` to use your manual sector size override
- **Force fresh discovery:** Delete matching line from `/data/shadowmount/autotune.ini`

