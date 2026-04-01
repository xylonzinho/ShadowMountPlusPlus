# ZFS Kernel Support Research and Integration Plan

Date: 2026-03-29

## Goal

Enable reliable automatic availability of ZFS support for image mounting by loading a PS5 kernel module when needed, with minimal user action and safe fallback behavior.

This plan assumes development/research on hardware you own and control.

## What Was Indexed

### Pages

- PS5 syscall table: <https://www.psdevwiki.com/ps5/Syscalls>
  - Relevant entries confirmed:
    - `sys_kldload` at syscall id `0x130`
    - `sys_kldunload` at syscall id `0x131`
    - `sys_kldfind` at syscall id `0x132`
    - `sys_kldsym` at syscall id `0x151`
    - `sys_nmount` at syscall id `0x17a`

### Related repositories cloned under `related_projects/`

- <https://github.com/buzzer-re/ps5-kld-sdk>
- <https://github.com/etaHEN/etaHEN>
- <https://github.com/buzzer-re/PS5_kldload>

### Key local references from indexed repos

- `related_projects/PS5_kldload/README.md`
  - Listener model on port `9022`
  - Firmware support statements and notes
- `related_projects/PS5_kldload/src/main.c`
  - Receives module payload bytes and creates kernel thread
- `related_projects/PS5_kldload/src/server.c`
  - TCP server loop and payload callback flow
- `related_projects/ps5-kld-sdk/include/ps5kld/kernel.h`
  - `kproc_args` interface and module entrypoint assumptions
- `related_projects/ps5-kld-sdk/src/kernel.c`
  - Firmware-offset initialization strategy
- `related_projects/etaHEN/README.md`
  - Existing auto-start concepts and payload ecosystem
- `related_projects/etaHEN/Source Code/daemon/source/main.cpp`
  - Daemon startup lifecycle and config-driven behavior
- `related_projects/etaHEN/Source Code/daemon/include/globalconf.hpp`
  - Runtime config fields and defaults

### ShadowMount++ integration points

- `src/sm_image.c`
  - ZFS image detection and mount path (`.ffzfs`, `fstype=zfs`, `nmount` flow)
  - Best place to trigger "ensure ZFS module loaded" just before ZFS `nmount`
- `src/sm_config_mount.c`
  - Runtime config defaults and parsing (ideal place for autoload settings)
- `src/main.c`
  - Initialization and daemon lifecycle

## Findings Summary

1. Your codebase already has image-type plumbing for ZFS (`IMAGE_FS_ZFS`, `.ffzfs`, zfs nmount iov).
2. The current missing piece is robust module availability orchestration before ZFS mount attempts.
3. Existing ecosystem projects provide a practical transport and runtime model for module loading, but compatibility is firmware-sensitive.
4. A safe design should not assume one loader path. It should support multiple strategies with runtime fallback.

## Constraints and Risks

- Firmware variance is the primary risk: offsets and behavior can change by version.
- "Module loaded" signal quality matters: false positives can cause repeated mount failures.
- Startup race conditions can occur if mount attempts happen before loader/service readiness.
- Repeated load attempts can destabilize runtime if throttling/backoff is absent.
- ZFS module ABI must match the target kernel expectations.

## Proposed Architecture: ZFS Module Manager Layer

Add a small internal component in ShadowMount++:

- New component suggestion: `src/sm_zfs_module.c` with header `include/sm_zfs_module.h`
- Responsibility:
  - Determine whether ZFS is already available
  - Perform one-time load attempt if needed
  - Cache state, backoff on failures, expose status

Suggested API:

- `bool sm_zfs_module_ensure_loaded(char *err, size_t err_sz);`
- `bool sm_zfs_module_is_ready(void);`
- `void sm_zfs_module_reset_state(void);`

## Solution Options

## Solution A (Recommended): Loader-Bridge strategy (network loader endpoint)

Use a running loader service endpoint as the module injection transport (for example, a loader daemon pattern comparable to `PS5_kldload`) and make ShadowMount++ trigger it only when first ZFS mount is requested.

### Why this is recommended

- Decouples ShadowMount++ from low-level kernel primitive maintenance
- Keeps your project focused on mount orchestration and policy
- Easier to support multiple environments by configuration

### Integration design

1. Add config keys in `config.ini.example` and parser in `src/sm_config_mount.c`:
   - `zfs_autoload = 0|1`
   - `zfs_loader_mode = tcp|none`
   - `zfs_loader_host = 127.0.0.1`
   - `zfs_loader_port = 9022`
   - `zfs_module_path = /data/shadowmount/zfs_kmod.bin`
   - `zfs_autoload_cooldown_seconds = 30`
2. In `mount_image()` inside `src/sm_image.c`:
   - If fs type is ZFS and `zfs_autoload=1`, call `sm_zfs_module_ensure_loaded(...)` before `perform_image_nmount(...)`.
3. In `sm_zfs_module_ensure_loaded(...)`:
   - Fast path: if known-ready, return true.
   - Probe path: perform a light readiness check (implementation-specific, with timeout).
   - If not ready, attempt one load via configured loader bridge.
   - Re-probe readiness; on success set cache-ready.
   - On failure set cooldown window and return detailed error.
4. Add structured logs and user notifications on:
   - attempt started
   - success
   - failure with cooldown active

### Operational behavior

- First `.ffzfs` mount triggers load attempt.
- Successful load is cached; subsequent mounts do not reload.
- Failures are throttled to avoid repeated aggressive retries.

## Solution B: etaHEN-daemon plugin/service orchestration

Implement ZFS module load orchestration in etaHEN side (plugin/daemon), and let ShadowMount++ only request/check "ZFS ready" status.

### Pros (Solution B)

- Better centralization if your stack already depends on etaHEN services
- Can integrate with existing startup and toolbox/autostart workflows

### Cons (Solution B)

- Adds external runtime dependency to ShadowMount++
- Version and deployment coupling to etaHEN ecosystem

### When to pick it

- You already deploy etaHEN on all targets and want single control plane for multiple payloads/modules.

## Solution C: Direct syscall-oriented loader path

Call kernel module load path through direct syscall wrappers in your own code path.

### Pros (Solution C)

- Fewer moving parts at runtime
- Potentially lower latency

### Cons (Solution C)

- Highest maintenance burden across firmware versions
- Strongly coupled to kernel ABI/offset details
- Riskier to keep stable over updates

### Recommendation

- Keep this as an experimental fallback path only, not primary.

## Recommended Final Strategy

Use Solution A as primary and optionally support Solution B in parallel for users with etaHEN-centric setups.

Concretely:

- Primary mode: `zfs_loader_mode=tcp`
- Optional mode: `zfs_loader_mode=etahen` (future extension)
- Disable mode: `zfs_loader_mode=none` for manual workflows

## Implementation Plan (Phased)

## Phase 1: Foundation and Config

1. Add runtime config fields in `include/sm_types.h`:
   - autoload enable flag
   - loader mode enum/string
   - host/port/path/cooldown values
2. Parse and log them in `src/sm_config_mount.c`.
3. Document them in `config.ini.example` and `README.md`.

Exit criteria:

- Config loads with sane defaults and appears in debug logs.

## Phase 2: ZFS Module Manager

1. Add `include/sm_zfs_module.h` and `src/sm_zfs_module.c`.
2. Implement internal state machine:
   - `UNKNOWN` -> `READY`
   - `UNKNOWN` -> `FAILED_COOLDOWN`
   - `FAILED_COOLDOWN` -> retry after deadline
3. Implement short timeouts and non-blocking-safe behavior where possible.
4. Add detailed error codes/messages.

Exit criteria:

- Standalone manager unit behavior validated by logs and synthetic tests.

## Phase 3: Mount Pipeline Hook

1. In `src/sm_image.c`, before ZFS `nmount`, call ensure-loaded.
2. If ensure-loaded fails:
   - fail mount with explicit actionable error
   - notify once per cooldown period
3. Keep non-ZFS image paths untouched.

Exit criteria:

- `.ffzfs` mount attempts trigger exactly one load flow during cooldown window.
- UFS/exFAT/PFS behavior unchanged.

## Phase 4: Validation Matrix

1. Test combinations:
   - Loader available/unavailable
   - Module file present/missing
   - ZFS already loaded/not loaded
   - Repeated mount attempts under failure
2. Firmware matrix at minimum across your active target versions.
3. Confirm no regressions for existing image types.

Exit criteria:

- Known failure modes are deterministic, throttled, and clearly logged.

## Phase 5: Hardening

1. Add retry backoff policy and upper bound.
2. Add optional health check interval to refresh readiness cache.
3. Add kill-switch config: `zfs_autoload=0` immediate disable.

Exit criteria:

- Runtime stability over long sessions and repeated scan/mount cycles.

## Suggested Config Defaults

- `zfs_autoload = 1`
- `zfs_loader_mode = tcp`
- `zfs_loader_host = 127.0.0.1`
- `zfs_loader_port = 9022`
- `zfs_module_path = /data/shadowmount/zfs_kmod.bin`
- `zfs_autoload_cooldown_seconds = 30`

## Observability Requirements

Add dedicated log tags:

- `[ZFSMOD] ready=true source=probe`
- `[ZFSMOD] autoload attempt mode=tcp host=... port=...`
- `[ZFSMOD] autoload failed err=... cooldown=...`
- `[ZFSMOD] autoload success elapsed_ms=...`

This is important to distinguish module-availability failures from `nmount` argument/FS failures.

## Minimal Test Cases

1. First mount with module absent and loader reachable -> autoload success, mount success.
2. First mount with module absent and loader unreachable -> autoload fail, throttled retries.
3. First mount with module already present -> no load attempt, mount proceeds.
4. Non-ZFS mounts -> no calls to module manager.
5. Process restart with persistent module state -> fast path readiness check succeeds.

## Decision

Proceed with Solution A now, keep Solution B as optional secondary integration, and leave Solution C as experimental fallback only.
