#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${TEST_SCRIPT_DIR}/.." && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

BACKING_DEVICE="${1:-}"
MAPPER_NAME="${IMR_LSM_SWEEP_MAPPER_NAME:-imrsim}"
MAPPER_DEVICE="/dev/mapper/${MAPPER_NAME}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
THRESHOLDS="${IMR_LSM_SWEEP_THRESHOLDS:-128 256 512 1024}"
REPETITIONS="${IMR_LSM_SWEEP_REPETITIONS:-3}"
RESULT_ROOT="${IMR_LSM_SWEEP_RESULT_DIR:-/var/tmp/imr-lsm-ycsb-threshold-$(date -u +%Y%m%dT%H%M%SZ)}"
STOP_ON_KERNEL_ISSUE="${IMR_LSM_SWEEP_STOP_ON_KERNEL_ISSUE:-1}"

readonly RECORD_COUNT="${IMR_LSM_SWEEP_RECORD_COUNT:-10000}"
readonly OPERATION_COUNT="${IMR_LSM_SWEEP_OPERATION_COUNT:-10000}"
readonly THREAD_COUNT="${IMR_LSM_SWEEP_THREAD_COUNT:-1}"
readonly MIN_L0_COMPACTIONS="${IMR_LSM_SWEEP_MIN_L0_COMPACTIONS:-0}"
readonly MIN_ZONE_COMPACTIONS="${IMR_LSM_SWEEP_MIN_ZONE_COMPACTIONS:-0}"
readonly MAX_BYTES_FOR_LEVEL_BASE=4096
readonly MAX_BYTES_FOR_LEVEL_MULTIPLIER=10
readonly INVALID_AVG_REVIEW_MS=1000
readonly INVALID_MAX_NEAR_REVIEW_MS=1800

readonly YCSB_RUNNER="${TEST_SCRIPT_DIR}/imr_lsm_ycsb_rocksdb_workload_test.sh"
readonly FORMATTER="${REPO_ROOT}/imrsim_util/imr_format.sh"
readonly KERNEL_ISSUE_PATTERN='blocked for more than|hung task|task (jbd2|sync)[^:]*:.*blocked|I/O error|Buffer I/O error|blk_update_request.*error|end_request.*I/O error|EXT4-fs error|JBD2:.*(error|abort)|journal has aborted'

SUMMARY_CSV=""
MEDIAN_CSV=""
BACKING_REAL=""
ACTIVE_MAPPER=0
LAST_L0_COMPACTION_COUNT=0
LAST_ZONE_COMPACTION_COUNT=0

log()
{
    printf '[imr-lsm threshold-sweep] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm threshold-sweep] FAIL: %s\n' "$*" >&2
    exit 1
}

usage()
{
    cat <<EOF
Usage:
  sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \\
    IMR_LSM_YCSB_HOME=/path/to/YCSB \\
    bash $0 BACKING_DEVICE

The backing device is reset before every run. The default formal matrix is:
  thresholds: 128 256 512 1024
  repetitions: 3
  records/operations/threads: 10000/10000/1
  cache mode: warm (no Linux cache drop; IMR-LSM read tree retained)
  dynamic level base/multiplier: 4096 bytes / 10

Optional controls:
  IMR_LSM_SWEEP_RESULT_DIR=/absolute/path
  IMR_LSM_SWEEP_MAPPER_NAME=imrsim
  IMR_LSM_SWEEP_THRESHOLDS="128 256 512 1024"
  IMR_LSM_SWEEP_REPETITIONS=3
  IMR_LSM_SWEEP_RECORD_COUNT=10000
  IMR_LSM_SWEEP_OPERATION_COUNT=10000
  IMR_LSM_SWEEP_THREAD_COUNT=1
  IMR_LSM_SWEEP_MIN_L0_COMPACTIONS=0
  IMR_LSM_SWEEP_MIN_ZONE_COMPACTIONS=0
  IMR_LSM_SWEEP_STOP_ON_KERNEL_ISSUE=0|1
EOF
}

mapper_exists()
{
    dmsetup info "${MAPPER_NAME}" >/dev/null 2>&1
}

remove_mapper()
{
    if mapper_exists; then
        dmsetup remove "${MAPPER_NAME}" ||
            fail "cannot remove mapper ${MAPPER_NAME}; verify it is not mounted or busy"
    fi
    ACTIVE_MAPPER=0
}

cleanup()
{
    set +e
    if [[ "${ACTIVE_MAPPER}" -eq 1 ]] && mapper_exists; then
        dmsetup remove "${MAPPER_NAME}"
    fi
}

require_tools()
{
    local tool

    for tool in awk bash blockdev cp date dmesg dmsetup grep mkdir readlink rm \
        sleep sort tail tee uname; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
    [[ -r "${YCSB_RUNNER}" ]] || fail "missing YCSB runner: ${YCSB_RUNNER}"
    [[ -r "${FORMATTER}" ]] || fail "missing readable formatter: ${FORMATTER}"
}

resolve_ycsb_preflight()
{
    local ycsb_bin="${IMR_LSM_YCSB_BIN:-}"
    local ycsb_home="${IMR_LSM_YCSB_HOME:-}"

    if [[ -n "${ycsb_bin}" ]]; then
        if [[ "${ycsb_bin}" == */* ]]; then
            [[ -x "${ycsb_bin}" ]] ||
                fail "YCSB binary is not executable: ${ycsb_bin}"
        else
            command -v "${ycsb_bin}" >/dev/null 2>&1 ||
                fail "missing YCSB binary: ${ycsb_bin}"
        fi
        return
    fi
    if [[ -n "${ycsb_home}" ]]; then
        [[ -x "${ycsb_home%/}/bin/ycsb" ||
           -x "${ycsb_home%/}/bin/ycsb.sh" ]] ||
            fail "cannot find an executable YCSB runner below ${ycsb_home}"
        return
    fi
    command -v ycsb >/dev/null 2>&1 ||
        fail "set IMR_LSM_YCSB_HOME or IMR_LSM_YCSB_BIN"
}

validate_existing_mapper()
{
    local table
    local target
    local existing_backing
    local mapper

    while read -r mapper _; do
        [[ -n "${mapper}" && "${mapper}" != "No" ]] || continue
        [[ "${mapper}" == "${MAPPER_NAME}" ]] ||
            fail "another imrsim mapper is active: ${mapper}"
    done < <(dmsetup ls --target imrsim 2>/dev/null || true)

    mapper_exists || return 0
    table="$(dmsetup table "${MAPPER_NAME}")" ||
        fail "cannot inspect existing mapper ${MAPPER_NAME}"
    [[ "$(awk 'NF { rows++ } END { print rows + 0 }' <<< "${table}")" -eq 1 ]] ||
        fail "existing mapper ${MAPPER_NAME} does not have exactly one table row"
    target="$(awk 'NF { print $3; exit }' <<< "${table}")"
    [[ "${target}" == "imrsim" ]] ||
        fail "existing mapper ${MAPPER_NAME} is not an imrsim target"
    existing_backing="$(awk 'NF { print $4; exit }' <<< "${table}")"
    case "${existing_backing}" in
        /*) ;;
        [0-9]*:[0-9]*) existing_backing="/dev/block/${existing_backing}" ;;
        *) existing_backing="/dev/${existing_backing}" ;;
    esac
    existing_backing="$(readlink -f "${existing_backing}")" ||
        fail "cannot resolve existing mapper backing device"
    [[ "${existing_backing}" == "${BACKING_REAL}" ]] ||
        fail "existing mapper uses ${existing_backing}, not requested ${BACKING_REAL}"
}

validate_config()
{
    local threshold

    [[ "${EUID}" -eq 0 ]] || fail "run as root with sudo"
    [[ "${IMR_LSM_TEST_DESTRUCTIVE:-0}" == "1" ]] ||
        fail "the backing device will be reset; set IMR_LSM_TEST_DESTRUCTIVE=1"
    [[ -n "${BACKING_DEVICE}" ]] || {
        usage >&2
        fail "BACKING_DEVICE is required"
    }
    [[ -b "${BACKING_DEVICE}" ]] ||
        fail "not a block device: ${BACKING_DEVICE}"
    BACKING_REAL="$(readlink -f "${BACKING_DEVICE}")" ||
        fail "cannot resolve backing device: ${BACKING_DEVICE}"
    [[ "${MAPPER_NAME}" =~ ^[A-Za-z0-9_.+-]+$ ]] ||
        fail "invalid mapper name: ${MAPPER_NAME}"
    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_SWEEP_REPETITIONS "${REPETITIONS}"
    [[ "${REPETITIONS}" -gt 0 ]] || fail "repetitions must be > 0"
    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_SWEEP_RECORD_COUNT "${RECORD_COUNT}"
    [[ "${RECORD_COUNT}" -gt 0 ]] || fail "record count must be > 0"
    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_SWEEP_OPERATION_COUNT "${OPERATION_COUNT}"
    [[ "${OPERATION_COUNT}" -gt 0 ]] || fail "operation count must be > 0"
    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_SWEEP_THREAD_COUNT "${THREAD_COUNT}"
    [[ "${THREAD_COUNT}" -gt 0 ]] || fail "thread count must be > 0"
    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_SWEEP_MIN_L0_COMPACTIONS "${MIN_L0_COMPACTIONS}"
    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_SWEEP_MIN_ZONE_COMPACTIONS "${MIN_ZONE_COMPACTIONS}"
    [[ "${STOP_ON_KERNEL_ISSUE}" == "0" ||
       "${STOP_ON_KERNEL_ISSUE}" == "1" ]] ||
        fail "IMR_LSM_SWEEP_STOP_ON_KERNEL_ISSUE must be 0 or 1"
    [[ "${RESULT_ROOT}" == /* ]] ||
        fail "IMR_LSM_SWEEP_RESULT_DIR must be an absolute path"
    [[ ! -e "${RESULT_ROOT}" ]] ||
        fail "result path already exists: ${RESULT_ROOT}"
    [[ -r "${DEBUGFS}/stats" ]] ||
        fail "missing readable ${DEBUGFS}/stats; load dm-imrsim first"
    [[ -w /dev/kmsg ]] || fail "cannot write dmesg boundary markers to /dev/kmsg"
    dmsetup targets | awk '$1 == "imrsim" { found = 1 } END { exit found ? 0 : 1 }' ||
        fail "device-mapper target imrsim is not loaded"

    [[ -n "${THRESHOLDS//[[:space:]]/}" ]] || fail "threshold list is empty"
    for threshold in ${THRESHOLDS}; do
        imr_lsm_test_require_nonnegative_integer threshold "${threshold}"
        [[ "${threshold}" -ge 1 && "${threshold}" -le 4096 ]] ||
            fail "threshold out of range: ${threshold}"
    done

    resolve_ycsb_preflight
    imr_lsm_test_require_safety_tools
    imr_lsm_test_acquire_lock
    validate_existing_mapper
}

write_manifest()
{
    local git_commit="unavailable"
    local git_state="unavailable"
    local algorithm_state="unavailable"
    local algorithm_source_sha256="unavailable"
    local loaded_module_path="unavailable"
    local loaded_module_sha256="unavailable"
    local ycsb_commit="unavailable"

    if command -v git >/dev/null 2>&1; then
        git_commit="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null ||
            printf 'unavailable')"
        if [[ -z "$(git -C "${REPO_ROOT}" status --porcelain \
            --untracked-files=normal 2>/dev/null)" ]]; then
            git_state="clean"
        else
            git_state="dirty"
        fi
        if [[ -z "$(git -C "${REPO_ROOT}" status --porcelain \
            --untracked-files=normal -- imrsim_kmod 2>/dev/null)" ]]; then
            algorithm_state="clean"
        else
            algorithm_state="dirty"
        fi
        if [[ -n "${IMR_LSM_YCSB_HOME:-}" ]]; then
            ycsb_commit="$(git -C "${IMR_LSM_YCSB_HOME}" rev-parse HEAD \
                2>/dev/null || printf 'unavailable')"
        fi
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        algorithm_source_sha256="$(sha256sum \
            "${REPO_ROOT}/imrsim_kmod/dm-imrsim.c" | awk '{ print $1 }')"
        if command -v modinfo >/dev/null 2>&1; then
            loaded_module_path="$(modinfo -n dm-imrsim 2>/dev/null ||
                printf 'unavailable')"
            if [[ -f "${loaded_module_path}" ]]; then
                loaded_module_sha256="$(sha256sum "${loaded_module_path}" |
                    awk '{ print $1 }')"
            fi
        fi
    fi

    {
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'kernel=%s\n' "$(uname -a)"
        printf 'repo_commit=%s\n' "${git_commit}"
        printf 'repo_state=%s\n' "${git_state}"
        printf 'algorithm_tree_state=%s\n' "${algorithm_state}"
        printf 'algorithm_source_sha256=%s\n' "${algorithm_source_sha256}"
        printf 'loaded_module_path=%s\n' "${loaded_module_path}"
        printf 'loaded_module_sha256=%s\n' "${loaded_module_sha256}"
        printf 'ycsb_commit=%s\n' "${ycsb_commit}"
        printf 'backing_device=%s\n' "${BACKING_REAL}"
        printf 'backing_bytes=%s\n' "$(blockdev --getsize64 "${BACKING_REAL}")"
        printf 'mapper_name=%s\n' "${MAPPER_NAME}"
        printf 'thresholds=%s\n' "${THRESHOLDS}"
        printf 'repetitions=%s\n' "${REPETITIONS}"
        printf 'record_count=%s\n' "${RECORD_COUNT}"
        printf 'operation_count=%s\n' "${OPERATION_COUNT}"
        printf 'thread_count=%s\n' "${THREAD_COUNT}"
        printf 'minimum_l0_compactions=%s\n' "${MIN_L0_COMPACTIONS}"
        printf 'minimum_zone_compactions=%s\n' \
            "${MIN_ZONE_COMPACTIONS}"
        printf 'cache_mode=warm\n'
        printf 'drop_caches=0\n'
        printf 'clear_read_tree=0\n'
        printf 'max_bytes_for_level_base=%s\n' \
            "${MAX_BYTES_FOR_LEVEL_BASE}"
        printf 'max_bytes_for_level_multiplier=%s\n' \
            "${MAX_BYTES_FOR_LEVEL_MULTIPLIER}"
        printf 'invalid_average_review_ms=%s\n' "${INVALID_AVG_REVIEW_MS}"
        printf 'invalid_max_near_review_ms=%s\n' \
            "${INVALID_MAX_NEAR_REVIEW_MS}"
    } > "${RESULT_ROOT}/manifest.txt"
}

prepare_mapper()
{
    local sectors
    local attempt

    remove_mapper
    sectors="$(
        IMR_LSM_TEST_DESTRUCTIVE=1 \
            bash "${FORMATTER}" -i -d "${BACKING_REAL}"
    )" || fail "cannot initialize IMR-LSM persistence on ${BACKING_REAL}"
    [[ "${sectors}" =~ ^[1-9][0-9]*$ ]] ||
        fail "formatter returned invalid sector count: ${sectors}"

    dmsetup create "${MAPPER_NAME}" \
        --table "0 ${sectors} imrsim ${BACKING_REAL} 0" ||
        fail "cannot create mapper ${MAPPER_NAME}"
    ACTIVE_MAPPER=1
    for ((attempt = 0; attempt < 50; attempt++)); do
        [[ -b "${MAPPER_DEVICE}" ]] && break
        sleep 0.1
    done
    [[ -b "${MAPPER_DEVICE}" ]] ||
        fail "mapper block device did not appear: ${MAPPER_DEVICE}"
}

verify_fresh_mapper()
{
    local key
    local value

    for key in logical_write_count lsm_record_insert_count compaction_count \
        read_tree_size newest_index_size newest_index_update_fail_count \
        newest_index_fallback_count invalid_recalc_count \
        zone_compaction_candidate_count zone_compaction_auto_pending \
        zone_compaction_auto_running zone_compaction_auto_pending_count \
        zone_compaction_auto_run_count \
        zone_compaction_auto_run_failed_count zone_compaction_count \
        zone_compaction_failed_count; do
        value="$(awk -F': ' -v key="${key}" \
            '$1 == key { print $2; found = 1; exit }
             END { if (!found) exit 1 }' "${DEBUGFS}/stats")" ||
            fail "fresh mapper is missing stat ${key}"
        [[ "${value}" == "0" ]] ||
            fail "fresh mapper check failed: ${key}=${value}"
    done

    value="$(awk -F': ' '$1 == "newest_index_valid" {
        print $2; found = 1; exit
    } END { if (!found) exit 1 }' "${DEBUGFS}/stats")" ||
        fail "fresh mapper is missing stat newest_index_valid"
    [[ "${value}" == "1" ]] ||
        fail "fresh mapper check failed: newest_index_valid=${value}"

    value="$(awk -F': ' '$1 == "zone_compaction_auto_run_enabled" {
        print $2; found = 1; exit
    } END { if (!found) exit 1 }' "${DEBUGFS}/stats")" ||
        fail "fresh mapper is missing stat zone_compaction_auto_run_enabled"
    [[ "${value}" == "1" ]] ||
        fail "fresh mapper check failed: zone_compaction_auto_run_enabled=${value}"
}

write_kmsg_marker()
{
    printf '<6>%s\n' "$1" > /dev/kmsg
}

capture_dmesg_interval()
{
    local start_marker="$1"
    local end_marker="$2"
    local output="$3"
    local snapshot="${output}.snapshot"

    dmesg > "${snapshot}"
    awk -v start="${start_marker}" -v finish="${end_marker}" '
        index($0, start) { capture = 1; next }
        index($0, finish) { if (capture) exit }
        capture { print }
    ' "${snapshot}" > "${output}"
    grep -Fq "${start_marker}" "${snapshot}" ||
        fail "dmesg start marker was not retained"
    grep -Fq "${end_marker}" "${snapshot}" ||
        fail "dmesg end marker was not retained"
    rm -f -- "${snapshot}"
}

stat_value()
{
    local file="$1"
    local key="$2"

    awk -v key="${key}" '$1 == key { print $2; found = 1; exit }
        END { if (!found) exit 1 }' "${file}"
}

stat_delta()
{
    local before="$1"
    local after="$2"
    local key="$3"
    local old
    local new

    old="$(stat_value "${before}" "${key}")"
    new="$(stat_value "${after}" "${key}")"
    printf '%s\n' "$((new - old))"
}

ns_to_ms()
{
    awk -v value="$1" 'BEGIN { printf "%.3f", value / 1000000 }'
}

average_ns_to_ms()
{
    local total="$1"
    local count="$2"

    awk -v total="${total}" -v count="${count}" '
        BEGIN {
            if (count > 0)
                printf "%.3f", total / count / 1000000
            else
                printf "0.000"
        }
    '
}

ycsb_overall_value()
{
    local file="$1"
    local metric="$2"

    awk -F',[[:space:]]*' -v metric="${metric}" '
        $1 == "[OVERALL]" && $2 == metric {
            gsub(/\r/, "", $3)
            print $3
            found = 1
        }
        END { if (!found) exit 1 }
    ' "${file}"
}

ycsb_max_operation_metric()
{
    local file="$1"
    local metric="$2"

    awk -F',[[:space:]]*' -v metric="${metric}" '
        $1 ~ /^\[(READ|UPDATE|INSERT|SCAN|READ-MODIFY-WRITE|DELETE)\]$/ &&
        $2 == metric {
            value = $3 + 0
            if (!found || value > maximum)
                maximum = value
            found = 1
        }
        END {
            if (!found) exit 1
            printf "%.0f", maximum
        }
    ' "${file}"
}

runner_timing_ms()
{
    local file="$1"
    local phase="$2"
    local label="$3"

    awk -v match_text="${phase} ${label}:" '
        index($0, match_text) { print $(NF - 1); found = 1 }
        END { if (!found) exit 1 }
    ' "${file}"
}

append_review()
{
    local -n review_ref="$1"
    local reason="$2"

    if [[ -n "${review_ref}" ]]; then
        review_ref+=";"
    fi
    review_ref+="${reason}"
}

greater_equal()
{
    awk -v left="$1" -v right="$2" 'BEGIN { exit left >= right ? 0 : 1 }'
}

append_failure_row()
{
    local threshold="$1"
    local repetition="$2"
    local status="$3"
    local kernel_issues="$4"
    local review="$5"
    local column

    printf '%s,%s,%s' "${threshold}" "${repetition}" "${status}" \
        >> "${SUMMARY_CSV}"
    for ((column = 4; column <= 33; column++)); do
        printf ',' >> "${SUMMARY_CSV}"
    done
    printf ',%s,%s\n' "${kernel_issues}" "${review}" >> "${SUMMARY_CSV}"
}

summarize_run()
{
    local threshold="$1"
    local repetition="$2"
    local run_dir="$3"
    local status="$4"
    local kernel_issues="$5"
    local artifacts="${run_dir}/artifacts"
    local before_load="${artifacts}/before-load.stats"
    local after_load="${artifacts}/after-load.stats"
    local before_run="${artifacts}/before-run.stats"
    local after_run="${artifacts}/after-run.stats"
    local load_ycsb="${artifacts}/ycsb-load.log"
    local run_ycsb="${artifacts}/ycsb-run.log"
    local runner_log="${run_dir}/runner.log"
    local load_throughput
    local load_process_ms
    local load_sync_ms
    local load_drain_ms
    local load_durable_ops_s
    local run_throughput
    local run_p99_us
    local run_max_us
    local run_process_ms
    local run_sync_ms
    local run_drain_ms
    local run_durable_ops_s
    local compaction_count
    local l0_compaction_count
    local l0_actual_bytes
    local load_zone_compaction_count
    local run_zone_compaction_count
    local zone_compaction_count
    local zone_compaction_candidate_count
    local zone_compaction_auto_run_failed_count
    local zone_compaction_failed_count
    local load_invalid_count
    local load_invalid_total_ns
    local load_invalid_avg_ms
    local run_invalid_count
    local run_invalid_total_ns
    local run_invalid_avg_ms
    local invalid_max_ms
    local invalid_entries_scanned
    local run_fg_wait_count
    local run_fg_wait_total_ns
    local run_fg_wait_avg_ms
    local run_fg_wait_max_ms
    local newest_index_valid
    local newest_index_update_fail_count
    local newest_index_fallback_count
    local review=""
    local required

    for required in "${before_load}" "${after_load}" "${before_run}" \
        "${after_run}" "${load_ycsb}" "${run_ycsb}" "${runner_log}"; do
        [[ -s "${required}" ]] ||
            fail "missing result artifact for threshold=${threshold} run=${repetition}: ${required}"
    done

    load_throughput="$(ycsb_overall_value "${load_ycsb}" 'Throughput(ops/sec)')"
    load_process_ms="$(runner_timing_ms "${runner_log}" load 'process wall time')"
    load_sync_ms="$(runner_timing_ms "${runner_log}" load 'final sync time')"
    load_drain_ms="$(runner_timing_ms \
        "${runner_log}" load 'background compaction drain time')"
    load_durable_ops_s="$(awk -v operations="${RECORD_COUNT}" \
        -v elapsed="$((load_process_ms + load_sync_ms + load_drain_ms))" '
        BEGIN {
            if (elapsed > 0)
                printf "%.3f", operations * 1000 / elapsed
            else
                printf "0.000"
        }')"
    run_throughput="$(ycsb_overall_value "${run_ycsb}" 'Throughput(ops/sec)')"
    run_p99_us="$(ycsb_max_operation_metric \
        "${run_ycsb}" '99thPercentileLatency(us)')"
    run_max_us="$(ycsb_max_operation_metric "${run_ycsb}" 'MaxLatency(us)')"
    run_process_ms="$(runner_timing_ms "${runner_log}" run 'process wall time')"
    run_sync_ms="$(runner_timing_ms "${runner_log}" run 'final sync time')"
    run_drain_ms="$(runner_timing_ms \
        "${runner_log}" run 'background compaction drain time')"
    run_durable_ops_s="$(awk -v operations="${OPERATION_COUNT}" \
        -v elapsed="$((run_process_ms + run_sync_ms + run_drain_ms))" '
        BEGIN {
            if (elapsed > 0)
                printf "%.3f", operations * 1000 / elapsed
            else
                printf "0.000"
        }')"

    compaction_count="$(stat_delta \
        "${before_load}" "${after_run}" compaction_count)"
    l0_compaction_count="$(stat_delta \
        "${before_load}" "${after_run}" level0_compaction_time_count)"
    l0_actual_bytes="$(stat_value "${after_run}" level0_actual_bytes)"
    LAST_L0_COMPACTION_COUNT="${l0_compaction_count}"
    load_zone_compaction_count="$(stat_delta \
        "${before_load}" "${after_load}" zone_compaction_count)"
    run_zone_compaction_count="$(stat_delta \
        "${before_run}" "${after_run}" zone_compaction_count)"
    zone_compaction_count="$(stat_delta \
        "${before_load}" "${after_run}" zone_compaction_count)"
    zone_compaction_candidate_count="$(stat_delta \
        "${before_load}" "${after_run}" zone_compaction_candidate_count)"
    zone_compaction_auto_run_failed_count="$(stat_delta \
        "${before_load}" "${after_run}" \
        zone_compaction_auto_run_failed_count)"
    zone_compaction_failed_count="$(stat_delta \
        "${before_load}" "${after_run}" zone_compaction_failed_count)"
    LAST_ZONE_COMPACTION_COUNT="${zone_compaction_count}"
    load_invalid_count="$(stat_delta \
        "${before_load}" "${after_load}" invalid_recalc_count)"
    load_invalid_total_ns="$(stat_delta \
        "${before_load}" "${after_load}" invalid_recalc_total_ns)"
    load_invalid_avg_ms="$(average_ns_to_ms \
        "${load_invalid_total_ns}" "${load_invalid_count}")"
    run_invalid_count="$(stat_delta \
        "${before_run}" "${after_run}" invalid_recalc_count)"
    run_invalid_total_ns="$(stat_delta \
        "${before_run}" "${after_run}" invalid_recalc_total_ns)"
    run_invalid_avg_ms="$(average_ns_to_ms \
        "${run_invalid_total_ns}" "${run_invalid_count}")"
    invalid_max_ms="$(ns_to_ms \
        "$(stat_value "${after_run}" invalid_recalc_max_ns)")"
    invalid_entries_scanned="$(stat_delta "${before_load}" "${after_run}" \
        invalid_recalc_entries_scanned_total)"
    run_fg_wait_count="$(stat_delta "${before_run}" "${after_run}" \
        foreground_zone_lock_wait_count)"
    run_fg_wait_total_ns="$(stat_delta "${before_run}" "${after_run}" \
        foreground_zone_lock_wait_total_ns)"
    run_fg_wait_avg_ms="$(average_ns_to_ms \
        "${run_fg_wait_total_ns}" "${run_fg_wait_count}")"
    run_fg_wait_max_ms="$(ns_to_ms \
        "$(stat_value "${after_run}" foreground_zone_lock_wait_max_ns)")"
    newest_index_valid="$(stat_value "${after_run}" newest_index_valid)"
    newest_index_update_fail_count="$(stat_delta \
        "${before_load}" "${after_run}" newest_index_update_fail_count)"
    newest_index_fallback_count="$(stat_delta \
        "${before_load}" "${after_run}" newest_index_fallback_count)"

    if greater_equal "${load_invalid_avg_ms}" "${INVALID_AVG_REVIEW_MS}"; then
        append_review review load_invalid_average_over_1s
    fi
    if greater_equal "${run_invalid_avg_ms}" "${INVALID_AVG_REVIEW_MS}"; then
        append_review review run_invalid_average_over_1s
    fi
    if greater_equal "${invalid_max_ms}" "${INVALID_MAX_NEAR_REVIEW_MS}"; then
        append_review review invalid_max_near_2s
    fi
    if [[ "${newest_index_valid}" != "1" ]]; then
        append_review review newest_index_invalid
    fi
    if [[ "${newest_index_update_fail_count}" -gt 0 ]]; then
        append_review review newest_index_update_failed
    fi
    if [[ "${newest_index_fallback_count}" -gt 0 ]]; then
        append_review review newest_index_fallback_used
    fi
    if [[ "${zone_compaction_auto_run_failed_count}" -gt 0 ]]; then
        append_review review zone_compaction_auto_run_failed
    fi
    if [[ "${zone_compaction_failed_count}" -gt 0 ]]; then
        append_review review zone_compaction_failed
    fi
    if [[ "${kernel_issues}" -gt 0 ]]; then
        append_review review kernel_issue
    fi
    if [[ -z "${review}" ]]; then
        review="none"
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${threshold}" "${repetition}" "${status}" \
        "${load_throughput}" "${load_process_ms}" "${load_sync_ms}" \
        "${load_drain_ms}" "${load_durable_ops_s}" "${run_throughput}" \
        "${run_p99_us}" "${run_max_us}" "${run_process_ms}" \
        "${run_sync_ms}" "${run_drain_ms}" "${run_durable_ops_s}" \
        "${compaction_count}" "${l0_compaction_count}" \
        "${l0_actual_bytes}" "${load_zone_compaction_count}" \
        "${run_zone_compaction_count}" "${zone_compaction_count}" \
        "${zone_compaction_candidate_count}" \
        "${zone_compaction_auto_run_failed_count}" \
        "${zone_compaction_failed_count}" "${load_invalid_count}" \
        "${load_invalid_avg_ms}" \
        "${run_invalid_count}" "${run_invalid_avg_ms}" \
        "${invalid_max_ms}" "${invalid_entries_scanned}" \
        "${run_fg_wait_count}" "${run_fg_wait_avg_ms}" \
        "${run_fg_wait_max_ms}" "${kernel_issues}" "${review}" \
        >> "${SUMMARY_CSV}"
}

run_one()
{
    local threshold="$1"
    local repetition="$2"
    local run_dir="${RESULT_ROOT}/threshold-${threshold}/run-${repetition}"
    local start_marker="imr_lsm_threshold_sweep_BEGIN_t${threshold}_r${repetition}_$$_$(date +%s%N)"
    local end_marker="imr_lsm_threshold_sweep_END_t${threshold}_r${repetition}_$$_$(date +%s%N)"
    local runner_status
    local kernel_issues
    local status="PASS"

    mkdir -p -- "${run_dir}/artifacts"
    log "starting threshold=${threshold} repetition=${repetition}"
    write_kmsg_marker "${start_marker}"
    prepare_mapper
    verify_fresh_mapper
    dmsetup table "${MAPPER_NAME}" > "${run_dir}/mapper-table.txt"
    cp -- "${DEBUGFS}/stats" "${run_dir}/fresh-mapper.stats"

    set +e
    IMR_LSM_TEST_DESTRUCTIVE=1 \
    IMR_LSM_YCSB_RECORD_COUNT="${RECORD_COUNT}" \
    IMR_LSM_YCSB_OPERATION_COUNT="${OPERATION_COUNT}" \
    IMR_LSM_YCSB_THREAD_COUNT="${THREAD_COUNT}" \
    IMR_LSM_YCSB_DROP_CACHES=0 \
    IMR_LSM_YCSB_CLEAR_READ_TREE=0 \
    IMR_LSM_YCSB_ALLOW_DIRTY=0 \
    IMR_LSM_YCSB_COMPACTION_THRESHOLD="${threshold}" \
    IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_BASE="${MAX_BYTES_FOR_LEVEL_BASE}" \
    IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_MULTIPLIER="${MAX_BYTES_FOR_LEVEL_MULTIPLIER}" \
    IMR_LSM_YCSB_RESULT_DIR="${run_dir}/artifacts" \
        bash "${YCSB_RUNNER}" "${MAPPER_DEVICE}" 2>&1 |
        tee "${run_dir}/runner.log"
    runner_status="${PIPESTATUS[0]}"
    set -e
    printf '%s\n' "${runner_status}" > "${run_dir}/runner-exit-status.txt"

    if [[ -r "${DEBUGFS}/stats" ]]; then
        cp -- "${DEBUGFS}/stats" "${run_dir}/final.stats"
    fi
    remove_mapper
    write_kmsg_marker "${end_marker}"
    capture_dmesg_interval "${start_marker}" "${end_marker}" \
        "${run_dir}/dmesg.log"
    grep -Ei "${KERNEL_ISSUE_PATTERN}" "${run_dir}/dmesg.log" \
        > "${run_dir}/kernel-issues.log" || true
    kernel_issues="$(awk 'END { print NR + 0 }' \
        "${run_dir}/kernel-issues.log")"

    if [[ "${runner_status}" -ne 0 ]]; then
        append_failure_row "${threshold}" "${repetition}" YCSB_FAIL \
            "${kernel_issues}" ycsb_failed
        log "threshold=${threshold} repetition=${repetition} failed; see ${run_dir}"
        return 1
    fi
    if [[ "${kernel_issues}" -gt 0 ]]; then
        status="KERNEL_ISSUE"
    fi
    summarize_run "${threshold}" "${repetition}" "${run_dir}" \
        "${status}" "${kernel_issues}"
    log "completed threshold=${threshold} repetition=${repetition} status=${status}"
    log "threshold=${threshold} repetition=${repetition} L0 compactions=${LAST_L0_COMPACTION_COUNT} minimum=${MIN_L0_COMPACTIONS}"
    log "threshold=${threshold} repetition=${repetition} zone compactions=${LAST_ZONE_COMPACTION_COUNT} minimum=${MIN_ZONE_COMPACTIONS}"

    if [[ "${kernel_issues}" -gt 0 && "${STOP_ON_KERNEL_ISSUE}" == "1" ]]; then
        return 2
    fi
    if [[ "${MIN_L0_COMPACTIONS}" -gt 0 &&
          "${LAST_L0_COMPACTION_COUNT}" -lt "${MIN_L0_COMPACTIONS}" ]]; then
        return 3
    fi
    if [[ "${MIN_ZONE_COMPACTIONS}" -gt 0 &&
          "${LAST_ZONE_COMPACTION_COUNT}" -lt \
          "${MIN_ZONE_COMPACTIONS}" ]]; then
        return 4
    fi
}

median_field()
{
    local threshold="$1"
    local column="$2"

    awk -F',' -v threshold="${threshold}" -v column="${column}" '
        NR > 1 && $1 == threshold && $3 == "PASS" { print $column }
    ' "${SUMMARY_CSV}" | sort -n | awk '
        { values[NR] = $1 }
        END {
            if (NR == 0) exit 1
            if (NR % 2 == 1)
                print values[(NR + 1) / 2]
            else
                printf "%.3f\n", \
                    (values[NR / 2] + values[NR / 2 + 1]) / 2
        }
    '
}

write_medians()
{
    local threshold
    local successful_runs
    local review_runs

    printf '%s\n' \
        'threshold,successful_runs,median_load_throughput_ops_s,median_load_durable_ops_s,median_run_throughput_ops_s,median_run_p99_us,median_run_max_us,median_run_durable_ops_s,median_compaction_count,median_l0_compaction_count,median_l0_actual_bytes,median_load_zone_compaction_count,median_run_zone_compaction_count,median_zone_compaction_count,median_zone_compaction_candidate_count,median_load_invalid_avg_ms,median_run_invalid_avg_ms,median_mapper_invalid_max_ms,median_invalid_entries_scanned,median_run_fg_wait_avg_ms,median_mapper_fg_wait_max_ms,review_runs' \
        > "${MEDIAN_CSV}"

    for threshold in ${THRESHOLDS}; do
        successful_runs="$(awk -F',' -v threshold="${threshold}" '
            NR > 1 && $1 == threshold && $3 == "PASS" { count++ }
            END { print count + 0 }
        ' "${SUMMARY_CSV}")"
        [[ "${successful_runs}" -gt 0 ]] || continue
        review_runs="$(awk -F',' -v threshold="${threshold}" '
            NR > 1 && $1 == threshold && $3 == "PASS" && $35 != "none" {
                count++
            }
            END { print count + 0 }
        ' "${SUMMARY_CSV}")"
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${threshold}" "${successful_runs}" \
            "$(median_field "${threshold}" 4)" \
            "$(median_field "${threshold}" 8)" \
            "$(median_field "${threshold}" 9)" \
            "$(median_field "${threshold}" 10)" \
            "$(median_field "${threshold}" 11)" \
            "$(median_field "${threshold}" 15)" \
            "$(median_field "${threshold}" 16)" \
            "$(median_field "${threshold}" 17)" \
            "$(median_field "${threshold}" 18)" \
            "$(median_field "${threshold}" 19)" \
            "$(median_field "${threshold}" 20)" \
            "$(median_field "${threshold}" 21)" \
            "$(median_field "${threshold}" 22)" \
            "$(median_field "${threshold}" 26)" \
            "$(median_field "${threshold}" 28)" \
            "$(median_field "${threshold}" 29)" \
            "$(median_field "${threshold}" 30)" \
            "$(median_field "${threshold}" 32)" \
            "$(median_field "${threshold}" 33)" \
            "${review_runs}" >> "${MEDIAN_CSV}"
    done
}

write_decision()
{
    local best_threshold
    local best_durable
    local review_runs
    local kernel_issue_runs

    best_threshold="$(awk -F',' -v expected="${REPETITIONS}" '
        NR > 1 && $2 == expected && (!found || $8 > best) {
            best = $8
            threshold = $1
            found = 1
        }
        END { if (found) print threshold }
    ' "${MEDIAN_CSV}")"
    best_durable="$(awk -F',' -v threshold="${best_threshold}" '
        NR > 1 && $1 == threshold { print $8 }
    ' "${MEDIAN_CSV}")"
    review_runs="$(awk -F',' 'NR > 1 && $35 != "none" { count++ }
        END { print count + 0 }' "${SUMMARY_CSV}")"
    kernel_issue_runs="$(awk -F',' 'NR > 1 && $34 > 0 { count++ }
        END { print count + 0 }' "${SUMMARY_CSV}")"

    {
        printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'best_threshold_by_median_run_durable_ops_s=%s\n' \
            "${best_threshold:-unavailable}"
        printf 'best_median_run_durable_ops_s=%s\n' \
            "${best_durable:-unavailable}"
        printf 'review_runs=%s\n' "${review_runs}"
        printf 'kernel_issue_runs=%s\n' "${kernel_issue_runs}"
        printf 'minimum_l0_compactions=%s\n' "${MIN_L0_COMPACTIONS}"
        printf 'minimum_zone_compactions=%s\n' \
            "${MIN_ZONE_COMPACTIONS}"
        if [[ "${review_runs}" -gt 0 || "${kernel_issue_runs}" -gt 0 ]]; then
            printf 'third_phase_algorithm_review=required\n'
        else
            printf 'third_phase_algorithm_review=not_triggered\n'
            printf 'next_step=threads_1_2_4_scaling_at_best_threshold\n'
        fi
        printf '%s\n' \
            'foreground_lock_wait_causality=compare run_fg_wait metrics with YCSB P99/max manually'
        printf '%s\n' \
            'scan_growth_gate=requires a separate larger-record-count experiment'
    } > "${RESULT_ROOT}/decision.txt"
}

main()
{
    local repetition
    local threshold
    local run_status
    local preflight_issue_count

    require_tools
    validate_config
    mkdir -p -- "${RESULT_ROOT}"
    trap cleanup EXIT
    SUMMARY_CSV="${RESULT_ROOT}/runs.csv"
    MEDIAN_CSV="${RESULT_ROOT}/medians.csv"
    write_manifest
    dmesg > "${RESULT_ROOT}/preflight-dmesg.log"
    tail -n 1000 "${RESULT_ROOT}/preflight-dmesg.log" |
        grep -Ei "${KERNEL_ISSUE_PATTERN}" \
            > "${RESULT_ROOT}/preflight-kernel-issues.log" || true
    preflight_issue_count="$(awk 'END { print NR + 0 }' \
        "${RESULT_ROOT}/preflight-kernel-issues.log")"
    if [[ "${preflight_issue_count}" -gt 0 ]]; then
        log "WARNING: preflight dmesg contains ${preflight_issue_count} matching line(s); review ${RESULT_ROOT}/preflight-kernel-issues.log"
    else
        log "preflight dmesg check: no hung-task, jbd2/sync-blocked, or I/O-error signature"
    fi
    printf '%s\n' \
        'threshold,repetition,status,load_throughput_ops_s,load_process_ms,load_sync_ms,load_compaction_drain_ms,load_durable_ops_s,run_throughput_ops_s,run_p99_us,run_max_us,run_process_ms,run_sync_ms,run_compaction_drain_ms,run_durable_ops_s,compaction_count,l0_compaction_count,l0_actual_bytes,load_zone_compaction_count,run_zone_compaction_count,zone_compaction_count,zone_compaction_candidate_count,zone_compaction_auto_run_failed_count,zone_compaction_failed_count,load_invalid_recalc_count,load_invalid_recalc_avg_ms,run_invalid_recalc_count,run_invalid_recalc_avg_ms,mapper_invalid_recalc_max_ms,invalid_entries_scanned,run_fg_zone_wait_count,run_fg_zone_wait_avg_ms,mapper_fg_zone_wait_max_ms,kernel_issue_count,review' \
        > "${SUMMARY_CSV}"

    remove_mapper
    for ((repetition = 1; repetition <= REPETITIONS; repetition++)); do
        for threshold in ${THRESHOLDS}; do
            set +e
            run_one "${threshold}" "${repetition}"
            run_status="$?"
            set -e
            if [[ "${run_status}" -eq 1 ]]; then
                fail "YCSB run failed; partial results are in ${RESULT_ROOT}"
            fi
            if [[ "${run_status}" -eq 2 ]]; then
                fail "kernel issue detected; stopped before further benchmark runs"
            fi
            if [[ "${run_status}" -eq 3 ]]; then
                fail "L0 compaction coverage ${LAST_L0_COMPACTION_COUNT} is below required ${MIN_L0_COMPACTIONS}; increase records/operations and rerun"
            fi
            if [[ "${run_status}" -eq 4 ]]; then
                fail "zone compaction coverage ${LAST_ZONE_COMPACTION_COUNT} is below required ${MIN_ZONE_COMPACTIONS}; inspect candidate/ready stats before increasing the workload"
            fi
        done
    done

    write_medians
    write_decision
    log "PASS: formal threshold sweep completed"
    log "per-run results: ${SUMMARY_CSV}"
    log "threshold medians: ${MEDIAN_CSV}"
    log "decision: ${RESULT_ROOT}/decision.txt"
}

main "$@"
