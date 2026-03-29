#!/bin/sh
# Create a ZFS image from a application directory
# Usage: makezfs.sh <input_dir> [output_file] [OPTIONS]
# 
# OPTIONS:
#   --no-optimize       Skip two-pass size optimization (write once, add fixed 256MB margin)
#   --no-bruteforce     Disable second-pass margin probing; use the requested margin once
#   --margin N          CoW margin percentage (default: 10, must be 1-50)
#                       Values lower than 10 may risk failure if compression is poor, values higher than 20 will waste space.
# 
# ENVIRONMENT:
#   ZFS_COMPRESSION     Compression algorithm (default: lz4)
#   ZFS_ASHIFT          ZFS ashift value (default: 12)
#   ZFS_RECORD_SIZE     Record size in bytes (default: 131072)
# 
# WHY MARGIN?
#   ZFS uses copy-on-write (CoW), requiring free space during writes.
#   The margin reserves space so rsync can complete safely. After the copy,
#   the pool will be ~(100-margin)% full with minimal wasted space.
#   Two-pass mode measures actual compressed size in pass 1, then sizes the image
#   just large enough in pass 2. If that would exceed raw input size, it uses
#   raw size instead (no compression benefit, but no risk of failure).
#   Bruteforce mode is enabled by default for optimized runs: it first tries the
#   requested margin, grows by +1% if needed to get a success, then probes downward
#   in 1% steps using .tmp outputs until the first failure to keep the smallest
#   working margin.
# 
# EXAMPLES:
#   sudo ./makezfs.sh ./APPXXXX ./output.ffzfs
#   sudo ./makezfs.sh ./APPXXXX ./output.ffzfs --no-optimize
#   sudo ./makezfs.sh ./APPXXXX ./output.ffzfs --no-bruteforce
#   sudo ./makezfs.sh ./APPXXXX ./output.ffzfs --margin 10
#   ZFS_COMPRESSION=zstd ./makezfs.sh ./APPXXXX ./output.ffzfs
# 
# Debian/Ubuntu: sudo apt-get install -y zfsutils-linux rsync
# macOS (Homebrew + OpenZFS): brew install openzfs rsync

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <input_dir> [output_file] [--no-optimize] [--no-bruteforce] [--margin N]"
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: this script must run as root (zpool/zfs operations require it)."
    exit 1
fi

INPUT_DIR="$1"
shift

OUTPUT="download0.ffzfs"
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then
    OUTPUT="$1"
    shift
fi

OPTIMIZE=true
BRUTEFORCE=true
MARGIN=10

is_positive_integer() {
    case "$1" in
        ''|*[!0-9]*)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

# Parse optional arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --no-optimize)
            OPTIMIZE=false
            shift
            ;;
        --no-bruteforce)
            BRUTEFORCE=false
            shift
            ;;
        --margin)
            if [ -z "$2" ]; then
                echo "Error: --margin requires a value"
                exit 1
            fi
            MARGIN="$2"
            if ! is_positive_integer "$MARGIN"; then
                echo "Error: margin must be a positive integer percent"
                exit 1
            fi
            if [ "$MARGIN" -lt 1 ] || [ "$MARGIN" -gt 50 ]; then
                echo "Error: margin must be between 1 and 50 percent"
                exit 1
            fi
            shift 2
            ;;
        *)
            echo "Error: unknown option: $1"
            exit 1
            ;;
    esac
done

if [ "$OPTIMIZE" = "false" ]; then
    BRUTEFORCE=false
fi

if [ ! -d "$INPUT_DIR" ]; then
    echo "Error: input directory not found: $INPUT_DIR"
    exit 1
fi

if [ ! -f "$INPUT_DIR/eboot.bin" ]; then
    echo "Error: eboot.bin not found in source directory: $INPUT_DIR"
    exit 1
fi

# ZFS defaults tuned for speed/compatibility.
ZFS_COMPRESSION="${ZFS_COMPRESSION:-lz4}"
ASHIFT="${ZFS_ASHIFT:-12}"
RECORD_SIZE="${ZFS_RECORD_SIZE:-131072}"

OS_TYPE=$(uname -s)
case "$OS_TYPE" in
    Darwin|Linux) ;;
    *)
        echo "Error: unsupported OS: $OS_TYPE"
        exit 1
        ;;
esac

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: required command not found: $1"
        exit 1
    fi
}

need_cmd find
need_cmd awk
need_cmd grep
need_cmd rsync
need_cmd tee
need_cmd zpool
need_cmd zfs

RSYNC_PROGRESS_ARGS="--progress"
if rsync --help 2>/dev/null | grep -q -- '--info'; then
    RSYNC_PROGRESS_ARGS="--info=progress2"
fi

rsync_copy() {
    # progress2 shows total transfer progress when supported; fallback remains per-file progress.
    rsync -r $RSYNC_PROGRESS_ARGS "$1"/ "$2"/
}

if [ "$OS_TYPE" = "Darwin" ]; then
    need_cmd mkfile
    need_cmd hdiutil
    STAT_FMT="-f %z"
else
    need_cmd truncate
    need_cmd losetup
    STAT_FMT="-c %s"
fi

OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"

SIZES_FILE=$(mktemp)
TMP_IMAGE=$(mktemp)
TMP_MOUNT=$(mktemp -d)
FINAL_MOUNT=$(mktemp -d)
TMP_POOL="smpzfs_t$$"
FINAL_POOL="smpzfs_f$$"
TMP_LOOP=""
FINAL_LOOP=""
TMP_POOL_CREATED=0
FINAL_POOL_CREATED=0
POOL_ALLOC=0
MARGIN_BYTES=0
FIXED_NO_OPT_MARGIN_BYTES=$((256 * 1024 * 1024))
TMP_OUTPUT="${OUTPUT}.tmp"
SELECTED_MARGIN="$MARGIN"
CANDIDATE_FINAL_BYTES=0
CANDIDATE_MARGIN_BYTES=0
CANDIDATE_MB=0
CANDIDATE_MARGIN_PCT_ACTUAL=0
LAST_COPY_NO_SPACE=false
RSYNC_ERR_LOG=$(mktemp)
RSYNC_ERR_PIPE="${RSYNC_ERR_LOG}.pipe"
SELECTED_MARGIN_PCT_ACTUAL=0

detach_device() {
    if [ -z "$1" ]; then
        return 0
    fi

    if [ "$OS_TYPE" = "Darwin" ]; then
        hdiutil detach "$1" >/dev/null 2>&1 || true
    else
        losetup -d "$1" >/dev/null 2>&1 || true
    fi
}

attach_image_file() {
    image_path="$1"
    image_mb="$2"

    if [ "$OS_TYPE" = "Darwin" ]; then
        mkfile -n "${image_mb}m" "$image_path" || return 1
        ATTACH_OUTPUT=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$image_path") || return 1
        loop_device=$(printf '%s\n' "$ATTACH_OUTPUT" | awk 'NR==1 {print $1; exit}')
        if [ -z "$loop_device" ]; then
            echo "Error: failed to determine attached raw device"
            return 1
        fi
    else
        truncate -s "${image_mb}M" "$image_path" || return 1
        loop_device=$(losetup --find --show "$image_path") || return 1
    fi

    printf '%s\n' "$loop_device"
}

cleanup_final_attempt() {
    image_path="$1"

    if [ "$FINAL_POOL_CREATED" -eq 1 ]; then
        zpool export "$FINAL_POOL" >/dev/null 2>&1 || true
        FINAL_POOL_CREATED=0
    fi

    if [ -n "$FINAL_LOOP" ]; then
        detach_device "$FINAL_LOOP"
        FINAL_LOOP=""
    fi

    rm -f "$image_path"
}

compute_candidate_size() {
    candidate_margin_pct="$1"

    candidate_margin_bytes=$(( (POOL_ALLOC * candidate_margin_pct) / 100 ))
    candidate_final_bytes=$(( POOL_ALLOC + candidate_margin_bytes ))

    if [ "$candidate_final_bytes" -gt "$RAW_FILE_BYTES" ]; then
        candidate_final_bytes="$RAW_FILE_BYTES"
        candidate_margin_bytes=$(( RAW_FILE_BYTES - POOL_ALLOC ))
        if [ "$candidate_margin_bytes" -lt 0 ]; then
            candidate_margin_bytes=0
        fi
    fi

    CANDIDATE_FINAL_BYTES="$candidate_final_bytes"
    CANDIDATE_MARGIN_BYTES="$candidate_margin_bytes"
    CANDIDATE_MB=$(( (candidate_final_bytes + 1024*1024 - 1) / (1024*1024) ))
    if [ "$POOL_ALLOC" -gt 0 ] && [ "$candidate_margin_bytes" -gt 0 ]; then
        CANDIDATE_MARGIN_PCT_ACTUAL=$(( (candidate_margin_bytes * 100) / POOL_ALLOC ))
    else
        CANDIDATE_MARGIN_PCT_ACTUAL=0
    fi
}

run_final_pass() {
    image_path="$1"
    image_mb="$2"
    stage_prefix="$3"

    echo "$stage_prefix Creating image container (${image_mb}MB)..."
    FINAL_LOOP=$(attach_image_file "$image_path" "$image_mb") || return 1

    echo "$stage_prefix Creating ZFS pool and copying files..."
    if ! zpool create -f \
      -o ashift="$ASHIFT" \
      -O compression="$ZFS_COMPRESSION" \
      -O atime=off \
      -O mountpoint="$FINAL_MOUNT" \
      "$FINAL_POOL" "$FINAL_LOOP"; then
        cleanup_final_attempt "$image_path"
        return 1
    fi
    FINAL_POOL_CREATED=1

    if ! zfs set recordsize="$RECORD_SIZE" "$FINAL_POOL"; then
        cleanup_final_attempt "$image_path"
        return 1
    fi

    LAST_COPY_NO_SPACE=false
    : > "$RSYNC_ERR_LOG"
    rm -f "$RSYNC_ERR_PIPE"
    if ! mkfifo "$RSYNC_ERR_PIPE"; then
        cleanup_final_attempt "$image_path"
        return 1
    fi

    tee -a "$RSYNC_ERR_LOG" < "$RSYNC_ERR_PIPE" >&2 &
    tee_pid=$!
    if rsync_copy "$INPUT_DIR" "$FINAL_MOUNT" 2>"$RSYNC_ERR_PIPE"; then
        rsync_status=0
    else
        rsync_status=$?
    fi
    wait "$tee_pid" >/dev/null 2>&1 || true
    rm -f "$RSYNC_ERR_PIPE"

    if [ "$rsync_status" -ne 0 ]; then
        if grep -qi 'No space left on device' "$RSYNC_ERR_LOG"; then
            LAST_COPY_NO_SPACE=true
        fi
        cleanup_final_attempt "$image_path"
        return 1
    fi

    echo "$stage_prefix Finalizing image (exporting pool)..."
    if ! zpool export "$FINAL_POOL"; then
        cleanup_final_attempt "$image_path"
        return 1
    fi
    FINAL_POOL_CREATED=0

    detach_device "$FINAL_LOOP"
    FINAL_LOOP=""
    return 0
}

cleanup() {
    if [ "$FINAL_POOL_CREATED" -eq 1 ]; then
        zpool export "$FINAL_POOL" >/dev/null 2>&1 || true
    fi
    if [ "$TMP_POOL_CREATED" -eq 1 ]; then
        zpool export "$TMP_POOL" >/dev/null 2>&1 || true
    fi
    if [ -n "$FINAL_LOOP" ]; then
        detach_device "$FINAL_LOOP"
    fi
    if [ -n "$TMP_LOOP" ]; then
        detach_device "$TMP_LOOP"
    fi
    rm -f "$SIZES_FILE" "$TMP_IMAGE"
    rm -f "$RSYNC_ERR_LOG"
    rm -f "$RSYNC_ERR_PIPE"
    rm -f "$TMP_OUTPUT"
    rmdir "$TMP_MOUNT" >/dev/null 2>&1 || true
    rmdir "$FINAL_MOUNT" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# Collect raw file sizes to estimate a safe temporary image size.
if [ "$OS_TYPE" = "Darwin" ]; then
    find "$INPUT_DIR" -type f -exec stat $STAT_FMT {} \; > "$SIZES_FILE"
else
    find "$INPUT_DIR" -type f -exec stat $STAT_FMT {} + > "$SIZES_FILE"
fi

RAW_FILE_BYTES=$(awk '{s += $1} END {print s+0}' "$SIZES_FILE")
rm -f "$SIZES_FILE"

echo "Input folder: $INPUT_DIR"
echo "Output file: $OUTPUT"
echo "ZFS profile: compression=$ZFS_COMPRESSION ashift=$ASHIFT recordsize=$RECORD_SIZE"
echo "Input size (raw files): $RAW_FILE_BYTES bytes (~$(( RAW_FILE_BYTES / 1024 / 1024 )) MB)"
if [ "$OPTIMIZE" = "true" ]; then
    if [ "$BRUTEFORCE" = "true" ]; then
        echo "Optimize mode: two-pass with bruteforce margin probing enabled"
    else
        echo "Optimize mode: two-pass with single margin attempt"
    fi
else
    echo "Optimize mode: disabled (--no-optimize)"
fi

if [ "$OPTIMIZE" = "false" ]; then
    # Single-pass: no compression measurement, use raw input size + fixed safety margin
    FINAL_BYTES=$((RAW_FILE_BYTES + FIXED_NO_OPT_MARGIN_BYTES))
    MB=$(( (FINAL_BYTES + 1024*1024 - 1) / (1024*1024) ))
    MARGIN_BYTES=$FIXED_NO_OPT_MARGIN_BYTES
    echo ""
    echo "[1/3] Creating image (single-pass mode, no size optimization, +256MB safety margin)..."

    if ! run_final_pass "$OUTPUT" "$MB" "[single-pass]"; then
        echo "Error: failed to create image in single-pass mode"
        exit 1
    fi

    POOL_ALLOC=0
else
    # Two-pass mode: measure compression, then size optimally
    TMP_MB=$(( ((RAW_FILE_BYTES * 5) / 4 + 256*1024*1024 + 1024*1024 - 1) / (1024*1024) ))
    
    echo ""
    echo "Pass 1/${TMP_MB}MB — measuring actual compressed allocation..."

    # ── pass 1: write to oversized temp image ────────────────────────────────────
    echo "[1/6] Creating temporary image for pass 1..."
    if [ "$OS_TYPE" = "Darwin" ]; then
        mkfile -n "${TMP_MB}m" "$TMP_IMAGE"
        TMP_ATTACH=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$TMP_IMAGE")
        TMP_LOOP=$(printf '%s\n' "$TMP_ATTACH" | awk 'NR==1 {print $1; exit}')
        if [ -z "$TMP_LOOP" ]; then
            echo "Error: failed to attach temp image"
            exit 1
        fi
    else
        truncate -s "${TMP_MB}M" "$TMP_IMAGE"
        TMP_LOOP=$(losetup --find --show "$TMP_IMAGE")
    fi

        echo "[2/6] Creating temporary pool and copying files (pass 1)..."
        zpool create -f \
      -o ashift="$ASHIFT" \
      -O compression="$ZFS_COMPRESSION" \
      -O atime=off \
      -O mountpoint="$TMP_MOUNT" \
      "$TMP_POOL" "$TMP_LOOP"
    TMP_POOL_CREATED=1

    zfs set recordsize="$RECORD_SIZE" "$TMP_POOL"
        rsync_copy "$INPUT_DIR" "$TMP_MOUNT"

    echo "[3/6] Measuring compressed allocation and closing pass 1..."
    # Measure actual bytes allocated in the pool (compressed data + all ZFS internal metadata).
    POOL_ALLOC=$(zpool list -Hp -o alloc "$TMP_POOL")

    zpool export "$TMP_POOL"
    TMP_POOL_CREATED=0

    if [ "$OS_TYPE" = "Darwin" ]; then
        hdiutil detach "$TMP_LOOP" >/dev/null 2>&1 || true
    else
        losetup -d "$TMP_LOOP" >/dev/null 2>&1 || true
    fi
    TMP_LOOP=""
    rm -f "$TMP_IMAGE"

    echo "Compressed allocation (pass 1): $POOL_ALLOC bytes (~$(( POOL_ALLOC / 1024 / 1024 )) MB)"
    if [ "$BRUTEFORCE" = "true" ]; then
        echo "Searching for the smallest working pass-2 margin..."
    else
        echo "Running pass 2 with requested margin ${MARGIN}%..."
    fi

    compute_candidate_size "$MARGIN"
    probe_margin="$MARGIN"
    probe_success=false

    while [ "$probe_success" = "false" ]; do
        echo "[pass2] Trying margin=${probe_margin}% -> ${CANDIDATE_MB}MB"
        rm -f "$TMP_OUTPUT"
        if run_final_pass "$TMP_OUTPUT" "$CANDIDATE_MB" "[pass2 ${probe_margin}%]"; then
            mv -f "$TMP_OUTPUT" "$OUTPUT"
            probe_success=true
            SELECTED_MARGIN="$probe_margin"
            SELECTED_MARGIN_PCT_ACTUAL="$CANDIDATE_MARGIN_PCT_ACTUAL"
            MARGIN_BYTES="$CANDIDATE_MARGIN_BYTES"
            FINAL_BYTES="$CANDIDATE_FINAL_BYTES"
            MB="$CANDIDATE_MB"
        else
            if [ "$BRUTEFORCE" != "true" ]; then
                echo "Error: pass 2 failed at margin ${probe_margin}%"
                exit 1
            fi

            if [ "$LAST_COPY_NO_SPACE" != "true" ]; then
                echo "Error: pass 2 failed for a reason other than running out of space"
                exit 1
            fi

            if [ "$CANDIDATE_FINAL_BYTES" -ge "$RAW_FILE_BYTES" ]; then
                echo "Error: pass 2 failed even at the raw-size cap; cannot grow further within current sizing policy"
                exit 1
            fi

            probe_margin=$((probe_margin + 1))
            if [ "$probe_margin" -gt 50 ]; then
                echo "Error: no working pass-2 margin found up to 50%"
                exit 1
            fi
            compute_candidate_size "$probe_margin"
        fi
    done

    if [ "$BRUTEFORCE" = "true" ]; then
        probe_margin=$((SELECTED_MARGIN - 1))
        while [ "$probe_margin" -ge 0 ]; do
            compute_candidate_size "$probe_margin"
            echo "[pass2] Probing smaller margin=${probe_margin}% -> ${CANDIDATE_MB}MB"
            rm -f "$TMP_OUTPUT"
            if run_final_pass "$TMP_OUTPUT" "$CANDIDATE_MB" "[probe ${probe_margin}%]"; then
                mv -f "$TMP_OUTPUT" "$OUTPUT"
                SELECTED_MARGIN="$probe_margin"
                SELECTED_MARGIN_PCT_ACTUAL="$CANDIDATE_MARGIN_PCT_ACTUAL"
                MARGIN_BYTES="$CANDIDATE_MARGIN_BYTES"
                FINAL_BYTES="$CANDIDATE_FINAL_BYTES"
                MB="$CANDIDATE_MB"
                probe_margin=$((probe_margin - 1))
            else
                if [ "$LAST_COPY_NO_SPACE" != "true" ]; then
                    echo "Error: probe at margin ${probe_margin}% failed for a reason other than running out of space"
                    exit 1
                fi
                echo "[pass2] Margin ${probe_margin}% failed; keeping last successful image"
                break
            fi
        done
    fi

    MARGIN="$SELECTED_MARGIN"
fi

# ── output summary ────────────────────────────────────────────────────────────
FINAL_IMAGE_BYTES=$(( MB * 1024 * 1024 ))
REDUCTION_PCT=0
if [ "$RAW_FILE_BYTES" -gt 0 ]; then
    REDUCTION_PCT=$(( (RAW_FILE_BYTES - FINAL_IMAGE_BYTES) * 100 / RAW_FILE_BYTES ))
fi

if [ "$POOL_ALLOC" = "0" ]; then
    COMPRESSION_PCT="(not measured)"
else
    COMPRESSION_PCT=$(( (RAW_FILE_BYTES - POOL_ALLOC) * 100 / RAW_FILE_BYTES ))
fi

echo ""
echo "Task completed successfully!"
echo ""
echo "Compression Results:"
echo "  Input size (raw files): $RAW_FILE_BYTES bytes (~$(( RAW_FILE_BYTES / 1024 / 1024 )) MB)"
if [ "$POOL_ALLOC" != "0" ]; then
    echo "  Compressed data: $POOL_ALLOC bytes (~$(( POOL_ALLOC / 1024 / 1024 )) MB, $COMPRESSION_PCT% reduction)"
fi
if [ "$MARGIN_BYTES" -gt 0 ]; then
    if [ "$OPTIMIZE" = "false" ]; then
        echo "  Free space margin: $MARGIN_BYTES bytes (~$(( MARGIN_BYTES / 1024 / 1024 )) MB, fixed in --no-optimize mode)"
    else
        echo "  Free space margin: $MARGIN_BYTES bytes (~$(( MARGIN_BYTES / 1024 / 1024 )) MB, ${SELECTED_MARGIN_PCT_ACTUAL}% actual)"
    fi
fi
echo "  Final image: $FINAL_IMAGE_BYTES bytes (~${MB} MB, $REDUCTION_PCT% reduction)"
