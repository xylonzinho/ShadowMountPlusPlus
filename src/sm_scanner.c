#include "sm_platform.h"

#include <fcntl.h>
#include <sys/event.h>

#include "sm_install.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_paths.h"
#include "sm_config_mount.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_types.h"

static int g_scanner_wake_pipe[2] = {-1, -1};
static int g_scanner_control_dir_fd = -1;
static volatile sig_atomic_t g_scanner_wake_write_fd = -1;

static bool set_fd_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_scanner_wake_pipe(void) {
  g_scanner_wake_write_fd = -1;
  if (g_scanner_wake_pipe[0] >= 0) {
    close(g_scanner_wake_pipe[0]);
    g_scanner_wake_pipe[0] = -1;
  }
  if (g_scanner_wake_pipe[1] >= 0) {
    close(g_scanner_wake_pipe[1]);
    g_scanner_wake_pipe[1] = -1;
  }
}

static void close_scanner_control_dir(void) {
  if (g_scanner_control_dir_fd >= 0) {
    close(g_scanner_control_dir_fd);
    g_scanner_control_dir_fd = -1;
  }
}

static void drain_scanner_wake_pipe(void) {
  if (g_scanner_wake_pipe[0] < 0)
    return;

  char buf[64];
  while (read(g_scanner_wake_pipe[0], buf, sizeof(buf)) > 0) {
  }
}

static void log_immediate_scan_reason(const char *reason) {
  if (!reason || reason[0] == '\0')
    return;
  log_debug("[SCAN] running immediate scan (%s)", reason);
}

static bool run_scan_cycle(bool startup_sync, const char *reason) {
  scan_candidate_t candidates[MAX_PENDING];
  memset(candidates, 0, sizeof(candidates));

  log_immediate_scan_reason(reason);

  cleanup_lost_sources_before_scan();

  int total_found_games = 0;
  int *total_found_ptr = startup_sync ? &total_found_games : NULL;
  int candidate_count =
      collect_scan_candidates(candidates, MAX_PENDING, total_found_ptr);
  if (candidate_count > 0) {
    if (startup_sync) {
      int new_games = 0;
      for (int i = 0; i < candidate_count; i++) {
        if (!candidates[i].installed)
          new_games++;
      }
      if (new_games > 0)
        notify_system_info("Found %d new games. Executing...", new_games);
    }

    process_scan_candidates(candidates, candidate_count);
  }

  mount_backport_overlays();

  if (startup_sync) {
    notify_system_rich(true, "Library Synchronized.\nFound %d games.",
                       total_found_games);
  }

  return !should_stop_requested();
}

static bool wait_for_scan_event(int kq, bool *timed_out_out) {
  if (timed_out_out)
    *timed_out_out = false;

  struct timespec timeout;
  timeout.tv_sec = (time_t)(runtime_config()->scan_interval_us / 1000000u);
  timeout.tv_nsec =
      (long)((runtime_config()->scan_interval_us % 1000000u) * 1000u);

  struct kevent event;
  int nev = kevent(kq, NULL, 0, &event, 1, &timeout);
  if (nev < 0) {
    if (errno == EINTR)
      return true;

    log_debug("  [SCAN] kevent wait failed: %s", strerror(errno));
    return false;
  }

  if (nev == 0) {
    if (timed_out_out)
      *timed_out_out = true;
    return true;
  }

  if (event.filter == EVFILT_READ &&
      event.ident == (uintptr_t)g_scanner_wake_pipe[0]) {
    drain_scanner_wake_pipe();
  }

  return true;
}

static bool register_scanner_kevents(int kq) {
  struct kevent kev;

  EV_SET(&kev, (uintptr_t)g_scanner_wake_pipe[0], EVFILT_READ,
         EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [SCAN] wake pipe registration failed: %s", strerror(errno));
    return false;
  }

  if (g_scanner_control_dir_fd >= 0) {
    EV_SET(&kev, (uintptr_t)g_scanner_control_dir_fd, EVFILT_VNODE,
           EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME |
               NOTE_REVOKE,
           0, NULL);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
      log_debug("  [SCAN] control dir watcher registration failed: %s",
                strerror(errno));
      return false;
    }
  }

  return true;
}

static void run_scanner_polling_loop(void) {
  const char *scan_reason = NULL;

  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    if (!consume_scan_now_request(&scan_reason)) {
      if (sleep_with_stop_check(runtime_config()->scan_interval_us)) {
        log_debug("[SHUTDOWN] stop requested during sleep");
        break;
      }
      (void)consume_scan_now_request(&scan_reason);
    }

    if (!run_scan_cycle(false, scan_reason))
      break;
    scan_reason = NULL;
  }
}

bool sm_scanner_init(void) {
  close_scanner_wake_pipe();
  close_scanner_control_dir();

  if (pipe(g_scanner_wake_pipe) != 0) {
    log_debug("  [SCAN] wake pipe creation failed: %s", strerror(errno));
    close_scanner_wake_pipe();
    return false;
  }
  if (!set_fd_nonblocking(g_scanner_wake_pipe[0]) ||
      !set_fd_nonblocking(g_scanner_wake_pipe[1])) {
    log_debug("  [SCAN] wake pipe nonblocking setup failed: %s",
              strerror(errno));
    close_scanner_wake_pipe();
    return false;
  }
  g_scanner_wake_write_fd = (sig_atomic_t)g_scanner_wake_pipe[1];

  g_scanner_control_dir_fd = open(LOG_DIR, O_RDONLY | O_DIRECTORY);
  if (g_scanner_control_dir_fd < 0) {
    log_debug("  [SCAN] control dir watch unavailable for %s: %s", LOG_DIR,
              strerror(errno));
  }

  return true;
}

void sm_scanner_wake(void) {
  sig_atomic_t wake_fd = g_scanner_wake_write_fd;
  if (wake_fd < 0)
    return;

  static const char token = 'S';
  (void)write((int)wake_fd, &token, sizeof(token));
}

bool sm_scanner_run_startup_sync(void) {
  if (should_stop_requested())
    return false;

  return run_scan_cycle(true, NULL);
}

void sm_scanner_run_loop(void) {
  if (g_scanner_wake_pipe[0] < 0 || g_scanner_wake_pipe[1] < 0) {
    log_debug("  [SCAN] scanner wake pipe unavailable, using polling loop");
    run_scanner_polling_loop();
    return;
  }

  int kq = kqueue();
  if (kq < 0) {
    log_debug("  [SCAN] kqueue init failed, using polling loop: %s",
              strerror(errno));
    run_scanner_polling_loop();
    return;
  }

  if (!register_scanner_kevents(kq)) {
    close(kq);
    run_scanner_polling_loop();
    return;
  }

  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    const char *scan_reason = NULL;
    if (consume_scan_now_request(&scan_reason)) {
      if (!run_scan_cycle(false, scan_reason))
        break;
      continue;
    }

    bool timed_out = false;
    if (!wait_for_scan_event(kq, &timed_out)) {
      log_debug("  [SCAN] falling back to polling loop after kevent failure");
      close(kq);
      run_scanner_polling_loop();
      return;
    }
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested during scanner wait");
      break;
    }

    if (consume_scan_now_request(&scan_reason)) {
      if (!run_scan_cycle(false, scan_reason))
        break;
      continue;
    }

    if (!timed_out)
      continue;

    if (!run_scan_cycle(false, NULL))
      break;
  }

  close(kq);
}

void sm_scanner_shutdown(void) {
  close_scanner_control_dir();
  close_scanner_wake_pipe();
}
