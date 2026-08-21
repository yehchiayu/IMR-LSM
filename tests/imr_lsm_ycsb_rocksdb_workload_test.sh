#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
YCSB_HOME="${IMR_LSM_YCSB_HOME:-}"
YCSB_BIN="${IMR_LSM_YCSB_BIN:-}"
YCSB_BINDING="${IMR_LSM_YCSB_BINDING:-rocksdb}"
YCSB_WORKLOAD="${IMR_LSM_YCSB_WORKLOAD:-workloada}"
YCSB_WORKLOAD_FILE="${IMR_LSM_YCSB_WORKLOAD_FILE:-}"
RECORD_COUNT="${IMR_LSM_YCSB_RECORD_COUNT:-10000}"
OPERATION_COUNT="${IMR_LSM_YCSB_OPERATION_COUNT:-10000}"
THREAD_COUNT="${IMR_LSM_YCSB_THREAD_COUNT:-4}"
TARGET="${IMR_LSM_YCSB_TARGET:-0}"
FIELD_COUNT="${IMR_LSM_YCSB_FIELD_COUNT:-10}"
FIELD_LENGTH="${IMR_LSM_YCSB_FIELD_LENGTH:-100}"
REQUEST_DISTRIBUTION="${IMR_LSM_YCSB_REQUEST_DISTRIBUTION:-zipfian}"
READ_PROPORTION="${IMR_LSM_YCSB_READ_PROPORTION:-}"
UPDATE_PROPORTION="${IMR_LSM_YCSB_UPDATE_PROPORTION:-}"
INSERT_PROPORTION="${IMR_LSM_YCSB_INSERT_PROPORTION:-}"
SCAN_PROPORTION="${IMR_LSM_YCSB_SCAN_PROPORTION:-}"
DELETE_PROPORTION="${IMR_LSM_YCSB_DELETE_PROPORTION:-}"
RUN_LOAD="${IMR_LSM_YCSB_LOAD:-1}"
RUN_RUN="${IMR_LSM_YCSB_RUN:-1}"
DROP_CACHES="${IMR_LSM_YCSB_DROP_CACHES:-0}"
CLEAR_READ_TREE="${IMR_LSM_YCSB_CLEAR_READ_TREE:-0}"
COMPACTION_THRESHOLD="${IMR_LSM_YCSB_COMPACTION_THRESHOLD:-}"
ALLOW_DIRTY="${IMR_LSM_YCSB_ALLOW_DIRTY:-0}"
RUN_FSTRIM="${IMR_LSM_YCSB_FSTRIM:-0}"
FSTRIM_LENGTH_BYTES="${IMR_LSM_YCSB_FSTRIM_LENGTH_BYTES:-16777216}"
MOUNT_OPTIONS="${IMR_LSM_YCSB_MOUNT_OPTIONS:-noatime,nodiratime}"
MKFS_EXT_OPTS="${IMR_LSM_YCSB_MKFS_EXT_OPTS:-nodiscard,lazy_itable_init=0,lazy_journal_init=0}"
MKFS_BLOCK_SIZE="${IMR_LSM_YCSB_MKFS_BLOCK_SIZE:-4096}"
MKFS_BLOCKS="${IMR_LSM_YCSB_MKFS_BLOCKS:-262144}"
DB_SUBDIR="${IMR_LSM_YCSB_DB_SUBDIR:-ycsb-rocksdb}"

BLOCK_SIZE=4096
TMPDIR=""
MNT=""
DB_PATH=""
ORIGINAL_COMPACTION_THRESHOLD=""

log()
{
    printf '[imr-lsm ycsb-rocksdb] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm ycsb-rocksdb] FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    set +e
    if [[ -n "${MNT}" ]] && mountpoint -q "${MNT}"; then
        umount "${MNT}" || umount -l "${MNT}"
    fi
    if [[ -n "${MNT}" && -d "${MNT}" ]]; then
        rmdir "${MNT}"
    fi
    if [[ -n "${TMPDIR}" && -d "${TMPDIR}" ]]; then
        rm -rf -- "${TMPDIR}"
    fi
    if [[ -n "${ORIGINAL_COMPACTION_THRESHOLD}" &&
          -w "${DEBUGFS}/compaction_threshold" ]]; then
        printf '%s\n' "${ORIGINAL_COMPACTION_THRESHOLD}" \
            > "${DEBUGFS}/compaction_threshold"
    fi
}

require_root()
{
    [[ "${EUID}" -eq 0 ]] ||
        fail "run as root, for example: sudo env IMR_LSM_TEST_DESTRUCTIVE=1 IMR_LSM_YCSB_HOME=/path/to/YCSB $0 ${DEVICE}"
}

require_debugfs()
{
    [[ -r "${DEBUGFS}/stats" ]] ||
        fail "missing readable ${DEBUGFS}/stats"
}

require_tools()
{
    local tool

    for tool in awk basename blockdev cp date dirname mkdir mkfs.ext4 mktemp mount \
        mountpoint rm rmdir sync tee umount; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
    if [[ "${RUN_FSTRIM}" == "1" ]]; then
        command -v fstrim >/dev/null 2>&1 ||
            fail "missing required tool: fstrim"
    fi
}

require_nonnegative()
{
    local label="$1"
    local value="$2"

    imr_lsm_test_require_nonnegative_integer "${label}" "${value}"
}

require_positive()
{
    local label="$1"
    local value="$2"

    require_nonnegative "${label}" "${value}"
    [[ "${value}" -gt 0 ]] ||
        fail "${label}=${value} must be > 0"
}

require_boolean()
{
    local label="$1"
    local value="$2"

    [[ "${value}" == "0" || "${value}" == "1" ]] ||
        fail "${label}=${value} must be 0 or 1"
}

resolve_ycsb()
{
    if [[ -z "${YCSB_BIN}" ]]; then
        if [[ -n "${YCSB_HOME}" ]]; then
            if [[ -x "${YCSB_HOME%/}/bin/ycsb" ]]; then
                YCSB_BIN="${YCSB_HOME%/}/bin/ycsb"
            elif [[ -x "${YCSB_HOME%/}/bin/ycsb.sh" ]]; then
                YCSB_BIN="${YCSB_HOME%/}/bin/ycsb.sh"
            else
                YCSB_BIN="${YCSB_HOME%/}/bin/ycsb"
            fi
        else
            YCSB_BIN="ycsb"
        fi
    fi

    if [[ "${YCSB_BIN}" == */* ]]; then
        [[ -x "${YCSB_BIN}" ]] ||
            fail "YCSB binary is not executable: ${YCSB_BIN}"
    else
        command -v "${YCSB_BIN}" >/dev/null 2>&1 ||
            fail "missing YCSB binary: ${YCSB_BIN}; set IMR_LSM_YCSB_HOME or IMR_LSM_YCSB_BIN"
    fi

    if [[ -z "${YCSB_WORKLOAD_FILE}" ]]; then
        if [[ -n "${YCSB_HOME}" &&
              -r "${YCSB_HOME%/}/workloads/${YCSB_WORKLOAD}" ]]; then
            YCSB_WORKLOAD_FILE="${YCSB_HOME%/}/workloads/${YCSB_WORKLOAD}"
        elif [[ -r "${YCSB_WORKLOAD}" ]]; then
            YCSB_WORKLOAD_FILE="${YCSB_WORKLOAD}"
        else
            fail "cannot find workload ${YCSB_WORKLOAD}; set IMR_LSM_YCSB_HOME or IMR_LSM_YCSB_WORKLOAD_FILE"
        fi
    fi
    [[ -r "${YCSB_WORKLOAD_FILE}" ]] ||
        fail "workload file is not readable: ${YCSB_WORKLOAD_FILE}"
}

require_test_range()
{
    local device_bytes
    local filesystem_bytes

    require_positive RECORD_COUNT "${RECORD_COUNT}"
    require_positive OPERATION_COUNT "${OPERATION_COUNT}"
    require_positive THREAD_COUNT "${THREAD_COUNT}"
    require_nonnegative TARGET "${TARGET}"
    require_positive FIELD_COUNT "${FIELD_COUNT}"
    require_positive FIELD_LENGTH "${FIELD_LENGTH}"
    require_positive MKFS_BLOCK_SIZE "${MKFS_BLOCK_SIZE}"
    require_positive MKFS_BLOCKS "${MKFS_BLOCKS}"
    require_boolean IMR_LSM_YCSB_DROP_CACHES "${DROP_CACHES}"
    require_boolean IMR_LSM_YCSB_CLEAR_READ_TREE "${CLEAR_READ_TREE}"
    require_boolean IMR_LSM_YCSB_ALLOW_DIRTY "${ALLOW_DIRTY}"
    if [[ "${CLEAR_READ_TREE}" == "1" ]]; then
        [[ "${RUN_LOAD}" == "1" && "${RUN_RUN}" == "1" ]] ||
            fail "IMR_LSM_YCSB_CLEAR_READ_TREE=1 requires both load and run phases"
        [[ -w "${DEBUGFS}/clear_read_tree" ]] ||
            fail "missing writable ${DEBUGFS}/clear_read_tree"
    fi
    if [[ -n "${COMPACTION_THRESHOLD}" ]]; then
        require_positive IMR_LSM_YCSB_COMPACTION_THRESHOLD \
            "${COMPACTION_THRESHOLD}"
    fi
    [[ "${MKFS_BLOCK_SIZE}" -eq "${BLOCK_SIZE}" ]] ||
        fail "MKFS_BLOCK_SIZE=${MKFS_BLOCK_SIZE} must match the 4 KiB IMR-LSM block size"

    device_bytes="$(blockdev --getsize64 "${DEVICE}")" ||
        fail "cannot read byte size for ${DEVICE}"
    filesystem_bytes=$((MKFS_BLOCKS * MKFS_BLOCK_SIZE))
    [[ "${device_bytes}" -ge "${filesystem_bytes}" ]] ||
        fail "device is smaller than requested filesystem: device=${device_bytes} fs=${filesystem_bytes}"

    if [[ "${RUN_FSTRIM}" == "1" ]]; then
        require_positive FSTRIM_LENGTH_BYTES "${FSTRIM_LENGTH_BYTES}"
        [[ $((FSTRIM_LENGTH_BYTES % BLOCK_SIZE)) -eq 0 ]] ||
            fail "FSTRIM_LENGTH_BYTES=${FSTRIM_LENGTH_BYTES} must be 4 KiB aligned"
        [[ "${filesystem_bytes}" -ge "${FSTRIM_LENGTH_BYTES}" ]] ||
            fail "filesystem is smaller than requested fstrim window: fs=${filesystem_bytes} fstrim=${FSTRIM_LENGTH_BYTES}"
    fi
}

configure_compaction_threshold()
{
    local actual

    [[ -n "${COMPACTION_THRESHOLD}" ]] || return
    [[ -r "${DEBUGFS}/compaction_threshold" &&
       -w "${DEBUGFS}/compaction_threshold" ]] ||
        fail "missing readable/writable ${DEBUGFS}/compaction_threshold"

    IFS= read -r ORIGINAL_COMPACTION_THRESHOLD \
        < "${DEBUGFS}/compaction_threshold" ||
        fail "cannot read the original compaction threshold"
    [[ "${ORIGINAL_COMPACTION_THRESHOLD}" =~ ^[0-9]+$ ]] ||
        fail "invalid original compaction threshold: ${ORIGINAL_COMPACTION_THRESHOLD}"

    printf '%s\n' "${COMPACTION_THRESHOLD}" \
        > "${DEBUGFS}/compaction_threshold" ||
        fail "cannot set compaction threshold to ${COMPACTION_THRESHOLD}"
    IFS= read -r actual < "${DEBUGFS}/compaction_threshold" ||
        fail "cannot verify compaction threshold"
    [[ "${actual}" == "${COMPACTION_THRESHOLD}" ]] ||
        fail "compaction threshold requested=${COMPACTION_THRESHOLD} actual=${actual}"
    log "compaction threshold=${actual} (original=${ORIGINAL_COMPACTION_THRESHOLD})"
}

stat_value()
{
    local key="$1"

    awk -F': ' -v key="${key}" '
        $1 == key { print $2; found = 1; exit }
        END { if (!found) exit 1 }
    ' "${DEBUGFS}/stats"
}

stat_number()
{
    local key="$1"
    local value

    value="$(stat_value "${key}")" ||
        fail "missing stat ${key}"
    [[ "${value}" =~ ^[0-9]+$ ]] ||
        fail "stat ${key} is not numeric: ${value}"
    printf '%s\n' "${value}"
}

have_stat()
{
    stat_value "$1" >/dev/null 2>&1
}

require_fresh_metadata()
{
    local key
    local value

    if [[ "${ALLOW_DIRTY}" == "1" ]]; then
        log "WARNING: IMR_LSM_YCSB_ALLOW_DIRTY=1 skips the fresh-metadata guard"
        return
    fi

    for key in logical_write_count lsm_record_insert_count lsm_write_count \
        delete_count compaction_count read_tree_size; do
        value="$(stat_number "${key}")"
        [[ "${value}" -eq 0 ]] ||
            fail "fresh IMR-LSM state required before mkfs: ${key}=${value}; remove the mapper, initialize persistence with 'sudo env IMR_LSM_TEST_DESTRUCTIVE=1 bash imrsim_util/imr_format.sh -i -d ${IMR_LSM_TEST_VALIDATED_BACKING_DEVICE}', then recreate the mapper"
    done
}

capture_stats()
{
    local output="$1"
    local key

    : > "${output}"
    for key in \
        lsm_record_insert_count \
        read_lookup_count \
        read_tree_hit_count \
        read_tree_miss_count \
        unsorted_hit_count \
        segment_lookup_count \
        segment_skip_count \
        segment_candidate_count \
        segment_hit_count \
        bloom_lookup_count \
        bloom_negative_count \
        bloom_maybe_count \
        block_table_lookup_count \
        block_table_hit_count \
        block_table_miss_count \
        compaction_count \
        segment_compaction_execute_count \
        zone_compaction_count \
        delete_count \
        discard_delete_count \
        tombstone_hit_count \
        fallback_count \
        segment_output_physical_copy_entry_count; do
        if have_stat "${key}"; then
            printf '%s %s\n' "${key}" "$(stat_number "${key}")" >> "${output}"
        fi
    done
}

log_stats_delta()
{
    local before="$1"
    local after="$2"
    local label="$3"

    log "${label} IMR-LSM counter delta:"
    awk '
        NR == FNR { before[$1] = $2; next }
        {
            old = ($1 in before) ? before[$1] : 0
            printf "  %-45s +%s\n", $1, $2 - old
        }
    ' "${before}" "${after}"
}

log_ycsb_summary()
{
    local phase="$1"
    local output="$2"
    local summary

    summary="$(
        awk -F',[[:space:]]*' '
            function clean(value) {
                gsub(/\r/, "", value)
                return value
            }

            $1 == "[OVERALL]" && $2 == "RunTime(ms)" {
                runtime = clean($3)
            }
            $1 == "[OVERALL]" && $2 == "Throughput(ops/sec)" {
                throughput = clean($3)
            }
            $2 == "Operations" {
                ops[$1] = clean($3)
            }
            $2 == "AverageLatency(us)" {
                avg[$1] = clean($3)
            }
            $2 == "95thPercentileLatency(us)" {
                p95[$1] = clean($3)
            }
            /current ops\/sec/ {
                value = $0
                sub(/^.*operations;[[:space:]]*/, "", value)
                sub(/[[:space:]]+current ops\/sec.*$/, "", value)
                last_interval_throughput = clean(value)
            }
            END {
                if (runtime != "")
                    printf "  %-30s %s ms\n", "runtime", runtime
                if (throughput != "")
                    printf "  %-30s %s ops/sec\n", "throughput", throughput
                if (last_interval_throughput != "")
                    printf "  %-30s %s ops/sec\n", \
                        "last interval throughput", last_interval_throughput

                split("[READ] [UPDATE] [INSERT] [SCAN] [READ-MODIFY-WRITE] [DELETE]", labels, " ")
                for (i = 1; i <= length(labels); i++) {
                    label = labels[i]
                    if (ops[label] != "" || avg[label] != "" || p95[label] != "") {
                        printf "  %-30s ops=%s avg=%s us p95=%s us\n", \
                            label, ops[label] + 0, avg[label] + 0, p95[label] + 0
                    }
                }
            }
        ' "${output}"
    )"

    if [[ -n "${summary}" ]]; then
        log "${phase} YCSB performance summary:"
        printf '%s\n' "${summary}"
    else
        log "${phase} YCSB performance summary unavailable"
    fi
}

now_ns()
{
    date +%s%N
}

timestamp_ycsb_output()
{
    local timing_file="$1"
    local line

    while IFS= read -r line || [[ -n "${line}" ]]; do
        printf '%s\n' "${line}"
        if [[ ! -s "${timing_file}" &&
              "${line}" == DBWrapper:* ]]; then
            now_ns > "${timing_file}"
        fi
    done
}

log_ycsb_wall_timing()
{
    local phase="$1"
    local start_ns="$2"
    local end_ns="$3"
    local timing_file="$4"
    local expected_operations="$5"
    local ready_ns
    local startup_ms
    local post_open_ms
    local post_open_throughput
    local total_ms

    total_ms=$(((end_ns - start_ns) / 1000000))
    log "${phase} process wall time: ${total_ms} ms"

    if [[ -s "${timing_file}" ]]; then
        IFS= read -r ready_ns < "${timing_file}"
        if [[ "${ready_ns}" =~ ^[0-9]+$ &&
              "${ready_ns}" -ge "${start_ns}" &&
              "${ready_ns}" -le "${end_ns}" ]]; then
            startup_ms=$(((ready_ns - start_ns) / 1000000))
            post_open_ms=$(((end_ns - ready_ns) / 1000000))
            post_open_throughput="$(
                awk -v operations="${expected_operations}" \
                    -v duration_ms="${post_open_ms}" '
                    BEGIN {
                        if (duration_ms > 0)
                            printf "%.3f", operations * 1000 / duration_ms
                        else
                            print "unavailable"
                    }
                '
            )"
            log "${phase} DB startup/open time: ${startup_ms} ms"
            log "${phase} post-open operations+cleanup time: ${post_open_ms} ms"
            log "${phase} post-open effective throughput: ${post_open_throughput} ops/sec (includes cleanup)"
            return
        fi
    fi
    log "${phase} DB startup/open timing unavailable"
}

validate_ycsb_output()
{
    local phase="$1"
    local output="$2"
    local failure
    local successful_operations

    failure="$(
        awk -F',[[:space:]]*' '
            /No space left on device/ ||
            /site[.]ycsb[.]DBException/ ||
            /org[.]rocksdb[.]RocksDBException/ ||
            /ERROR site[.]ycsb/ {
                print
                exit
            }
            $1 ~ /-FAILED]$/ && $2 == "Operations" && ($3 + 0) > 0 {
                print
                exit
            }
            $2 == "Return=ERROR" && ($3 + 0) > 0 {
                print
                exit
            }
        ' "${output}"
    )"
    [[ -z "${failure}" ]] ||
        fail "YCSB ${phase} reported an error: ${failure}"

    successful_operations="$(
        awk -F',[[:space:]]*' '
            $1 ~ /^\[(READ|UPDATE|INSERT|SCAN|READ-MODIFY-WRITE|DELETE)\]$/ &&
            $2 == "Operations" {
                total += $3
            }
            END { print total + 0 }
        ' "${output}"
    )"
    [[ "${successful_operations}" -gt 0 ]] ||
        fail "YCSB ${phase} completed without any successful operations"
}

format_device()
{
    local output

    log "mkfs.ext4 -b ${MKFS_BLOCK_SIZE} ${DEVICE} ${MKFS_BLOCKS}"
    if output="$(
        mkfs.ext4 -F -q -b "${MKFS_BLOCK_SIZE}" -E "${MKFS_EXT_OPTS}" \
            "${DEVICE}" "${MKFS_BLOCKS}" 2>&1
    )"; then
        return
    fi

    printf '%s\n' "${output}" >&2
    fail "mkfs.ext4 failed; override IMR_LSM_YCSB_MKFS_EXT_OPTS if this e2fsprogs version needs different options"
}

mount_device()
{
    MNT="$(mktemp -d)"
    if [[ -n "${MOUNT_OPTIONS}" ]]; then
        mount -o "${MOUNT_OPTIONS}" "${DEVICE}" "${MNT}"
    else
        mount "${DEVICE}" "${MNT}"
    fi
    DB_PATH="${MNT}/${DB_SUBDIR}"
    mkdir -p "${DB_PATH}"
    log "mounted ${DEVICE} at ${MNT}"
}

drop_linux_caches()
{
    [[ -w /proc/sys/vm/drop_caches ]] ||
        fail "cannot write /proc/sys/vm/drop_caches"

    log "syncing and dropping Linux page cache before YCSB run"
    sync
    if ! printf '3\n' > /proc/sys/vm/drop_caches; then
        fail "failed to drop Linux page cache"
    fi
}

clear_imr_lsm_read_tree()
{
    local size

    log "clearing IMR-LSM read tree before YCSB run"
    printf '1\n' > "${DEBUGFS}/clear_read_tree" ||
        fail "cannot clear IMR-LSM read tree"
    size="$(stat_value read_tree_size)"
    [[ "${size}" == "0" ]] ||
        fail "IMR-LSM read tree clear left size=${size}"
}

append_optional_prop()
{
    local -n args_ref="$1"
    local key="$2"
    local value="$3"

    if [[ -n "${value}" ]]; then
        args_ref+=("-p" "${key}=${value}")
    fi
}

build_ycsb_args()
{
    local -n args_ref="$1"

    args_ref=(
        "-s"
        "-P" "${YCSB_WORKLOAD_FILE}"
        "-p" "rocksdb.dir=${DB_PATH}"
        "-p" "recordcount=${RECORD_COUNT}"
        "-p" "operationcount=${OPERATION_COUNT}"
        "-p" "threadcount=${THREAD_COUNT}"
        "-p" "fieldcount=${FIELD_COUNT}"
        "-p" "fieldlength=${FIELD_LENGTH}"
        "-p" "requestdistribution=${REQUEST_DISTRIBUTION}"
    )
    if [[ "${TARGET}" -gt 0 ]]; then
        args_ref+=("-target" "${TARGET}")
    fi
    append_optional_prop args_ref readproportion "${READ_PROPORTION}"
    append_optional_prop args_ref updateproportion "${UPDATE_PROPORTION}"
    append_optional_prop args_ref insertproportion "${INSERT_PROPORTION}"
    append_optional_prop args_ref scanproportion "${SCAN_PROPORTION}"
    append_optional_prop args_ref deleteproportion "${DELETE_PROPORTION}"
}

run_ycsb_phase()
{
    local phase="$1"
    local before="$2"
    local after="$3"
    local args=()
    local output
    local status
    local timing_file
    local phase_start_ns
    local phase_end_ns
    local sync_start_ns
    local sync_end_ns
    local sync_ms
    local expected_operations

    build_ycsb_args args
    if [[ "${phase}" == "load" ]]; then
        expected_operations="${RECORD_COUNT}"
    else
        expected_operations="${OPERATION_COUNT}"
    fi
    output="${TMPDIR}/ycsb-${phase}.log"
    timing_file="${TMPDIR}/ycsb-${phase}.db-ready-ns"
    log "ycsb ${phase} ${YCSB_BINDING} workload=$(basename "${YCSB_WORKLOAD_FILE}") recordcount=${RECORD_COUNT} operationcount=${OPERATION_COUNT} threads=${THREAD_COUNT}"
    phase_start_ns="$(now_ns)"
    set +e
    "${YCSB_BIN}" "${phase}" "${YCSB_BINDING}" "${args[@]}" 2>&1 |
        timestamp_ycsb_output "${timing_file}" |
        tee "${output}"
    status="${PIPESTATUS[0]}"
    set -e
    phase_end_ns="$(now_ns)"
    [[ "${status}" -eq 0 ]] ||
        fail "YCSB ${phase} failed"
    log_ycsb_wall_timing "${phase}" "${phase_start_ns}" "${phase_end_ns}" \
        "${timing_file}" "${expected_operations}"
    log_ycsb_summary "${phase}" "${output}"
    sync_start_ns="$(now_ns)"
    sync
    sync_end_ns="$(now_ns)"
    sync_ms=$(((sync_end_ns - sync_start_ns) / 1000000))
    log "${phase} final sync time: ${sync_ms} ms"
    capture_stats "${after}"
    log_stats_delta "${before}" "${after}" "${phase}"
    validate_ycsb_output "${phase}" "${output}"
}

main()
{
    local before_load
    local after_load
    local before_run
    local after_run
    local before_fstrim
    local after_fstrim
    local prepared_run=0

    require_root
    require_tools
    require_debugfs
    resolve_ycsb
    imr_lsm_test_safety_begin "${DEVICE}"
    require_test_range
    require_fresh_metadata

    TMPDIR="$(mktemp -d)"
    trap cleanup EXIT

    configure_compaction_threshold
    format_device
    mount_device

    before_load="${TMPDIR}/before-load.stats"
    after_load="${TMPDIR}/after-load.stats"
    before_run="${TMPDIR}/before-run.stats"
    after_run="${TMPDIR}/after-run.stats"
    before_fstrim="${TMPDIR}/before-fstrim.stats"
    after_fstrim="${TMPDIR}/after-fstrim.stats"

    capture_stats "${before_load}"
    if [[ "${RUN_LOAD}" == "1" ]]; then
        run_ycsb_phase load "${before_load}" "${after_load}"
    else
        cp "${before_load}" "${after_load}"
        log "SKIP: YCSB load disabled"
    fi

    if [[ "${RUN_LOAD}" == "1" && "${RUN_RUN}" == "1" &&
          "${DROP_CACHES}" == "1" ]]; then
        drop_linux_caches
        prepared_run=1
    fi
    if [[ "${RUN_LOAD}" == "1" && "${RUN_RUN}" == "1" &&
          "${CLEAR_READ_TREE}" == "1" ]]; then
        clear_imr_lsm_read_tree
        prepared_run=1
    fi
    if [[ "${prepared_run}" == "1" ]]; then
        capture_stats "${before_run}"
    else
        cp "${after_load}" "${before_run}"
    fi
    if [[ "${RUN_RUN}" == "1" ]]; then
        run_ycsb_phase run "${before_run}" "${after_run}"
    else
        cp "${before_run}" "${after_run}"
        log "SKIP: YCSB run disabled"
    fi

    if [[ "${RUN_FSTRIM}" == "1" ]]; then
        cp "${after_run}" "${before_fstrim}"
        log "fstrim offset=0 length=${FSTRIM_LENGTH_BYTES}"
        fstrim -o 0 -l "${FSTRIM_LENGTH_BYTES}" -m "${BLOCK_SIZE}" "${MNT}"
        sync
        capture_stats "${after_fstrim}"
        log_stats_delta "${before_fstrim}" "${after_fstrim}" fstrim
    fi

    log "PASS: YCSB over RocksDB workload completed"
}

main "$@"
