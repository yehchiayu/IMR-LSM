#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${TEST_SCRIPT_DIR}/.." && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

BACKING_DEVICE="${1:-}"
MAPPER_NAME="${IMR_LSM_YCSB_COMPARE_MAPPER_NAME:-imrsim}"
MAPPER_DEVICE="/dev/mapper/${MAPPER_NAME}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
WORKLOADS="workloada workloadb workloadc workloadd workloade workloadf"
REPETITIONS="${IMR_LSM_YCSB_COMPARE_REPETITIONS:-3}"
RESULT_ROOT="${IMR_LSM_YCSB_COMPARE_RESULT_DIR:-/var/tmp/imr-lsm-ycsb-a-f-$(date -u +%Y%m%dT%H%M%SZ)}"
STOP_ON_KERNEL_ISSUE="${IMR_LSM_YCSB_COMPARE_STOP_ON_KERNEL_ISSUE:-1}"

readonly RECORD_COUNT=100000
readonly OPERATION_COUNT=100000
readonly THREAD_COUNT=1
readonly COMPACTION_THRESHOLD=4096
readonly MAX_BYTES_FOR_LEVEL_BASE=4096
readonly MAX_BYTES_FOR_LEVEL_MULTIPLIER=10
readonly LSM_RECORD_BYTES=32

readonly YCSB_RUNNER="${TEST_SCRIPT_DIR}/imr_lsm_ycsb_rocksdb_workload_test.sh"
readonly FORMATTER="${REPO_ROOT}/imrsim_util/imr_format.sh"
readonly IMRSIM_UTIL="${IMR_LSM_YCSB_IMRSIM_UTIL:-${REPO_ROOT}/imrsim_util/imrsim_util}"
readonly KERNEL_ISSUE_PATTERN='blocked for more than|hung task|task (jbd2|sync)[^:]*:.*blocked|I/O error|Buffer I/O error|blk_update_request.*error|end_request.*I/O error|EXT4-fs error|JBD2:.*(error|abort)|journal has aborted'

RUNS_CSV=""
MEDIANS_CSV=""
BACKING_REAL=""
ACTIVE_MAPPER=0

log()
{
    printf '[imr-lsm ycsb-compare] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm ycsb-compare] FAIL: %s\n' "$*" >&2
    exit 1
}

usage()
{
    cat <<EOF
Usage:
  sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \\
    IMR_LSM_YCSB_HOME=/path/to/YCSB \\
    bash $0 BACKING_DEVICE

The backing device is reset before every run. The fixed comparison matrix is:
  workloads: workloada through workloadf
  records/operations/default threads: 100000/100000/1
  compaction threshold: 4096
  dynamic level base/multiplier: 4096 bytes / 10
  cache mode: warm

Optional controls:
  IMR_LSM_YCSB_COMPARE_RESULT_DIR=/absolute/path
  IMR_LSM_YCSB_COMPARE_REPETITIONS=3
  IMR_LSM_YCSB_COMPARE_MAPPER_NAME=imrsim
  IMR_LSM_YCSB_COMPARE_STOP_ON_KERNEL_ISSUE=0|1
  IMR_LSM_YCSB_IMRSIM_UTIL=/path/to/imrsim_util
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

    for tool in awk bash blockdev cp date dmesg dmsetup grep mkdir readlink \
        rm sleep sort tail tee uname; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
    [[ -r "${YCSB_RUNNER}" ]] || fail "missing YCSB runner: ${YCSB_RUNNER}"
    [[ -r "${FORMATTER}" ]] || fail "missing formatter: ${FORMATTER}"
    [[ -x "${IMRSIM_UTIL}" ]] ||
        fail "IMRSim statistics utility is not executable: ${IMRSIM_UTIL}; run 'make -C ${REPO_ROOT}/imrsim_util' first"
}

resolve_ycsb_preflight()
{
    local ycsb_bin="${IMR_LSM_YCSB_BIN:-}"
    local ycsb_home="${IMR_LSM_YCSB_HOME:-}"
    local workload

    if [[ -n "${ycsb_bin}" ]]; then
        if [[ "${ycsb_bin}" == */* ]]; then
            [[ -x "${ycsb_bin}" ]] || fail "YCSB binary is not executable: ${ycsb_bin}"
        else
            command -v "${ycsb_bin}" >/dev/null 2>&1 ||
                fail "missing YCSB binary: ${ycsb_bin}"
        fi
    elif [[ -n "${ycsb_home}" ]]; then
        [[ -x "${ycsb_home%/}/bin/ycsb" ||
           -x "${ycsb_home%/}/bin/ycsb.sh" ]] ||
            fail "cannot find an executable YCSB runner below ${ycsb_home}"
    else
        command -v ycsb >/dev/null 2>&1 ||
            fail "set IMR_LSM_YCSB_HOME or IMR_LSM_YCSB_BIN"
    fi

    for workload in ${WORKLOADS}; do
        if [[ -n "${ycsb_home}" ]]; then
            [[ -r "${ycsb_home%/}/workloads/${workload}" ]] ||
                fail "missing YCSB workload file: ${ycsb_home%/}/workloads/${workload}"
        else
            [[ -r "${workload}" ]] ||
                fail "cannot find ${workload}; set IMR_LSM_YCSB_HOME"
        fi
    done
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
    [[ "${EUID}" -eq 0 ]] || fail "run as root with sudo"
    [[ "${IMR_LSM_TEST_DESTRUCTIVE:-0}" == "1" ]] ||
        fail "the backing device will be reset; set IMR_LSM_TEST_DESTRUCTIVE=1"
    [[ -n "${BACKING_DEVICE}" ]] || {
        usage >&2
        fail "BACKING_DEVICE is required"
    }
    [[ -b "${BACKING_DEVICE}" ]] || fail "not a block device: ${BACKING_DEVICE}"
    BACKING_REAL="$(readlink -f "${BACKING_DEVICE}")" ||
        fail "cannot resolve backing device: ${BACKING_DEVICE}"
    [[ "${MAPPER_NAME}" =~ ^[A-Za-z0-9_.+-]+$ ]] ||
        fail "invalid mapper name: ${MAPPER_NAME}"
    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_YCSB_COMPARE_REPETITIONS "${REPETITIONS}"
    [[ "${REPETITIONS}" -gt 0 ]] || fail "repetitions must be > 0"
    [[ "${STOP_ON_KERNEL_ISSUE}" == "0" ||
       "${STOP_ON_KERNEL_ISSUE}" == "1" ]] ||
        fail "IMR_LSM_YCSB_COMPARE_STOP_ON_KERNEL_ISSUE must be 0 or 1"
    [[ "${RESULT_ROOT}" == /* ]] ||
        fail "IMR_LSM_YCSB_COMPARE_RESULT_DIR must be an absolute path"
    [[ ! -e "${RESULT_ROOT}" ]] ||
        fail "result path already exists: ${RESULT_ROOT}"
    [[ -r "${DEBUGFS}/stats" ]] ||
        fail "missing readable ${DEBUGFS}/stats; load dm-imrsim first"
    [[ -w /dev/kmsg ]] || fail "cannot write dmesg boundary markers to /dev/kmsg"
    dmsetup targets | awk '$1 == "imrsim" { found = 1 }
        END { exit found ? 0 : 1 }' ||
        fail "device-mapper target imrsim is not loaded"

    resolve_ycsb_preflight
    imr_lsm_test_require_safety_tools
    imr_lsm_test_acquire_lock
    validate_existing_mapper
}

write_manifest()
{
    local git_commit="unavailable"
    local ycsb_commit="unavailable"

    if command -v git >/dev/null 2>&1; then
        git_commit="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null ||
            printf 'unavailable')"
        if [[ -n "${IMR_LSM_YCSB_HOME:-}" ]]; then
            ycsb_commit="$(git -C "${IMR_LSM_YCSB_HOME}" rev-parse HEAD \
                2>/dev/null || printf 'unavailable')"
        fi
    fi

    {
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'kernel=%s\n' "$(uname -a)"
        printf 'repo_commit=%s\n' "${git_commit}"
        printf 'ycsb_commit=%s\n' "${ycsb_commit}"
        printf 'backing_device=%s\n' "${BACKING_REAL}"
        printf 'backing_bytes=%s\n' "$(blockdev --getsize64 "${BACKING_REAL}")"
        printf 'mapper_name=%s\n' "${MAPPER_NAME}"
        printf 'workloads=%s\n' "${WORKLOADS}"
        printf 'workloada_proportions=read:0.5,update:0.5\n'
        printf 'workloadb_proportions=read:0.95,update:0.05\n'
        printf 'workloadc_proportions=read:1.0,update:0.0\n'
        printf 'workloadd_proportions=read:0.95,insert:0.05,distribution:latest\n'
        printf 'workloade_proportions=scan:0.95,insert:0.05\n'
        printf 'workloadf_proportions=read:0.5,read-modify-write:0.5\n'
        printf 'repetitions=%s\n' "${REPETITIONS}"
        printf 'record_count=%s\n' "${RECORD_COUNT}"
        printf 'operation_count=%s\n' "${OPERATION_COUNT}"
        printf 'default_threads=%s\n' "${THREAD_COUNT}"
        printf 'compaction_threshold=%s\n' "${COMPACTION_THRESHOLD}"
        printf 'max_bytes_for_level_base=%s\n' "${MAX_BYTES_FOR_LEVEL_BASE}"
        printf 'max_bytes_for_level_multiplier=%s\n' \
            "${MAX_BYTES_FOR_LEVEL_MULTIPLIER}"
        printf 'cache_mode=warm\n'
        printf 'imr_wa_formula=write_total_delta/(write_total_delta-extra_write_total_delta)\n'
        printf 'metadata_wa_formula=(lsm_ingest_records*32+metadata_compaction_output_bytes_delta)/(lsm_ingest_records*32)\n'
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
        invalid_incremental_segment_publish_count \
        invalid_incremental_fallback_recalc_count; do
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

    printf '%s\n' "$(($(stat_value "${after}" "${key}") -
        $(stat_value "${before}" "${key}")))"
}

device_stat_value()
{
    local file="$1"
    local label="$2"

    awk -F': ' -v label="${label}" '$1 == label {
        print $2; found = 1; exit
    } END { if (!found) exit 1 }' "${file}"
}

device_stat_delta()
{
    local before="$1"
    local after="$2"
    local label="$3"

    printf '%s\n' "$(($(device_stat_value "${after}" "${label}") -
        $(device_stat_value "${before}" "${label}")))"
}

ratio_or_na()
{
    local numerator="$1"
    local denominator="$2"

    awk -v numerator="${numerator}" -v denominator="${denominator}" '
        BEGIN {
            if (denominator > 0)
                printf "%.6f", numerator / denominator
            else
                printf "NA"
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

ycsb_operation_value()
{
    local file="$1"
    local operation="$2"
    local metric="$3"

    awk -F',[[:space:]]*' -v operation="[${operation}]" -v metric="${metric}" '
        $1 == operation && $2 == metric {
            gsub(/\r/, "", $3)
            print $3
            found = 1
        }
        END { if (!found) print "NA" }
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

durable_throughput()
{
    local operations="$1"
    local process_ms="$2"
    local sync_ms="$3"
    local drain_ms="$4"

    awk -v operations="${operations}" \
        -v elapsed="$((process_ms + sync_ms + drain_ms))" '
        BEGIN {
            if (elapsed > 0)
                printf "%.3f", operations * 1000 / elapsed
            else
                printf "0.000"
        }
    '
}

append_failure_row()
{
    local workload="$1"
    local repetition="$2"
    local status="$3"
    local kernel_issues="$4"
    local review="$5"
    local row=("${workload}" "${repetition}" "${status}")
    local column

    for ((column = 4; column <= 35; column++)); do
        row+=("")
    done
    row+=("${kernel_issues}" "${review}")
    (IFS=,; printf '%s\n' "${row[*]}") >> "${RUNS_CSV}"
}

summarize_run()
{
    local workload="$1"
    local repetition="$2"
    local run_dir="$3"
    local status="$4"
    local kernel_issues="$5"
    local artifacts="${run_dir}/artifacts"
    local before_load="${artifacts}/before-load.stats"
    local after_load="${artifacts}/after-load.stats"
    local before_run="${artifacts}/before-run.stats"
    local after_run="${artifacts}/after-run.stats"
    local before_load_device="${artifacts}/before-load.device-stats"
    local after_load_device="${artifacts}/after-load.device-stats"
    local before_run_device="${artifacts}/before-run.device-stats"
    local after_run_device="${artifacts}/after-run.device-stats"
    local load_ycsb="${artifacts}/ycsb-load.log"
    local run_ycsb="${artifacts}/ycsb-run.log"
    local runner_log="${run_dir}/runner.log"
    local required
    local load_throughput load_durable run_throughput run_durable
    local load_process_ms load_sync_ms load_drain_ms
    local run_process_ms run_sync_ms run_drain_ms
    local read_ops read_avg read_p95 read_p99 read_max
    local write_ops write_avg write_p95 write_p99 write_max
    local read_operation write_operation
    local load_write_total load_extra_writes load_host_writes load_imr_wa
    local run_write_total run_extra_writes run_host_writes run_imr_wa
    local load_ingest load_compaction_input load_compaction_output
    local run_ingest run_compaction_input run_compaction_output
    local load_logical_bytes load_metadata_wa
    local run_logical_bytes run_metadata_wa
    local compaction_count l0_compaction_count newest_valid
    local newest_update_fail newest_fallback invalid_incremental_fallback
    local review="none"
    local row

    for required in "${before_load}" "${after_load}" "${before_run}" \
        "${after_run}" "${before_load_device}" "${after_load_device}" \
        "${before_run_device}" "${after_run_device}" "${load_ycsb}" \
        "${run_ycsb}" "${runner_log}"; do
        [[ -s "${required}" ]] ||
            fail "missing result artifact for workload=${workload} run=${repetition}: ${required}"
    done

    load_throughput="$(ycsb_overall_value "${load_ycsb}" 'Throughput(ops/sec)')"
    run_throughput="$(ycsb_overall_value "${run_ycsb}" 'Throughput(ops/sec)')"
    load_process_ms="$(runner_timing_ms "${runner_log}" load 'process wall time')"
    load_sync_ms="$(runner_timing_ms "${runner_log}" load 'final sync time')"
    load_drain_ms="$(runner_timing_ms "${runner_log}" load \
        'background level compaction drain time')"
    run_process_ms="$(runner_timing_ms "${runner_log}" run 'process wall time')"
    run_sync_ms="$(runner_timing_ms "${runner_log}" run 'final sync time')"
    run_drain_ms="$(runner_timing_ms "${runner_log}" run \
        'background level compaction drain time')"
    load_durable="$(durable_throughput "${RECORD_COUNT}" "${load_process_ms}" \
        "${load_sync_ms}" "${load_drain_ms}")"
    run_durable="$(durable_throughput "${OPERATION_COUNT}" "${run_process_ms}" \
        "${run_sync_ms}" "${run_drain_ms}")"

    case "${workload}" in
        workloade) read_operation=SCAN ;;
        *) read_operation=READ ;;
    esac
    case "${workload}" in
        workloada|workloadb) write_operation=UPDATE ;;
        workloadc) write_operation=NONE ;;
        workloadd|workloade) write_operation=INSERT ;;
        workloadf) write_operation=READ-MODIFY-WRITE ;;
        *) fail "unsupported workload: ${workload}" ;;
    esac

    read_ops="$(ycsb_operation_value "${run_ycsb}" \
        "${read_operation}" Operations)"
    read_avg="$(ycsb_operation_value "${run_ycsb}" \
        "${read_operation}" 'AverageLatency(us)')"
    read_p95="$(ycsb_operation_value "${run_ycsb}" \
        "${read_operation}" '95thPercentileLatency(us)')"
    read_p99="$(ycsb_operation_value "${run_ycsb}" \
        "${read_operation}" '99thPercentileLatency(us)')"
    read_max="$(ycsb_operation_value "${run_ycsb}" \
        "${read_operation}" 'MaxLatency(us)')"
    if [[ "${write_operation}" == "NONE" ]]; then
        write_ops=NA
        write_avg=NA
        write_p95=NA
        write_p99=NA
        write_max=NA
    else
        write_ops="$(ycsb_operation_value "${run_ycsb}" \
            "${write_operation}" Operations)"
        write_avg="$(ycsb_operation_value "${run_ycsb}" \
            "${write_operation}" 'AverageLatency(us)')"
        write_p95="$(ycsb_operation_value "${run_ycsb}" \
            "${write_operation}" '95thPercentileLatency(us)')"
        write_p99="$(ycsb_operation_value "${run_ycsb}" \
            "${write_operation}" '99thPercentileLatency(us)')"
        write_max="$(ycsb_operation_value "${run_ycsb}" \
            "${write_operation}" 'MaxLatency(us)')"
    fi

    load_write_total="$(device_stat_delta "${before_load_device}" \
        "${after_load_device}" 'imrsim write total count')"
    load_extra_writes="$(device_stat_delta "${before_load_device}" \
        "${after_load_device}" 'imrsim extra write total count')"
    load_host_writes=$((load_write_total - load_extra_writes))
    load_imr_wa="$(ratio_or_na "${load_write_total}" "${load_host_writes}")"
    run_write_total="$(device_stat_delta "${before_run_device}" \
        "${after_run_device}" 'imrsim write total count')"
    run_extra_writes="$(device_stat_delta "${before_run_device}" \
        "${after_run_device}" 'imrsim extra write total count')"
    run_host_writes=$((run_write_total - run_extra_writes))
    run_imr_wa="$(ratio_or_na "${run_write_total}" "${run_host_writes}")"

    load_ingest="$(stat_delta "${before_load}" "${after_load}" \
        lsm_record_insert_count)"
    load_compaction_input="$(stat_delta "${before_load}" "${after_load}" \
        metadata_compaction_input_bytes)"
    load_compaction_output="$(stat_delta "${before_load}" "${after_load}" \
        metadata_compaction_output_bytes)"
    load_logical_bytes=$((load_ingest * LSM_RECORD_BYTES))
    load_metadata_wa="$(ratio_or_na \
        "$((load_logical_bytes + load_compaction_output))" \
        "${load_logical_bytes}")"
    run_ingest="$(stat_delta "${before_run}" "${after_run}" \
        lsm_record_insert_count)"
    run_compaction_input="$(stat_delta "${before_run}" "${after_run}" \
        metadata_compaction_input_bytes)"
    run_compaction_output="$(stat_delta "${before_run}" "${after_run}" \
        metadata_compaction_output_bytes)"
    run_logical_bytes=$((run_ingest * LSM_RECORD_BYTES))
    run_metadata_wa="$(ratio_or_na \
        "$((run_logical_bytes + run_compaction_output))" \
        "${run_logical_bytes}")"
    compaction_count="$(stat_delta "${before_run}" "${after_run}" \
        compaction_count)"
    l0_compaction_count="$(stat_delta "${before_run}" "${after_run}" \
        level0_compaction_time_count)"

    newest_valid="$(stat_value "${after_run}" newest_index_valid)"
    newest_update_fail="$(stat_delta "${before_load}" "${after_run}" \
        newest_index_update_fail_count)"
    newest_fallback="$(stat_delta "${before_load}" "${after_run}" \
        newest_index_fallback_count)"
    invalid_incremental_fallback="$(stat_delta \
        "${before_load}" "${after_run}" \
        invalid_incremental_fallback_recalc_count)"
    if [[ "${newest_valid}" != "1" || "${newest_update_fail}" -gt 0 ||
          "${newest_fallback}" -gt 0 ||
          "${invalid_incremental_fallback}" -gt 0 ]]; then
        review="newest_index_issue"
    fi
    if [[ "${kernel_issues}" -gt 0 ]]; then
        if [[ "${review}" == "none" ]]; then
            review="kernel_issue"
        else
            review+=";kernel_issue"
        fi
    fi

    row=(
        "${workload}" "${repetition}" "${status}"
        "${load_throughput}" "${load_durable}"
        "${run_throughput}" "${run_durable}"
        "${read_operation}" "${read_ops}" "${read_avg}" "${read_p95}"
        "${read_p99}" "${read_max}" "${write_operation}" "${write_ops}"
        "${write_avg}" "${write_p95}" "${write_p99}" "${write_max}"
        "${load_write_total}" "${load_extra_writes}" "${load_imr_wa}"
        "${run_write_total}" "${run_extra_writes}" "${run_imr_wa}"
        "${load_ingest}" "${load_compaction_input}" "${load_compaction_output}"
        "${load_metadata_wa}" "${run_ingest}" "${run_compaction_input}"
        "${run_compaction_output}" "${run_metadata_wa}" "${compaction_count}"
        "${l0_compaction_count}" "${kernel_issues}" "${review}"
    )
    (IFS=,; printf '%s\n' "${row[*]}") >> "${RUNS_CSV}"
}

run_one()
{
    local workload="$1"
    local repetition="$2"
    local run_dir="${RESULT_ROOT}/${workload}/run-${repetition}"
    local start_marker="imr_lsm_ycsb_compare_BEGIN_${workload}_r${repetition}_$$_$(date +%s%N)"
    local end_marker="imr_lsm_ycsb_compare_END_${workload}_r${repetition}_$$_$(date +%s%N)"
    local runner_status
    local kernel_issues
    local status="PASS"
    local read_proportion
    local update_proportion
    local insert_proportion=0
    local scan_proportion=0
    local read_modify_write_proportion=0
    local request_distribution=zipfian

    case "${workload}" in
        workloada)
            read_proportion=0.5
            update_proportion=0.5
            ;;
        workloadb)
            read_proportion=0.95
            update_proportion=0.05
            ;;
        workloadc)
            read_proportion=1.0
            update_proportion=0.0
            ;;
        workloadd)
            read_proportion=0.95
            update_proportion=0
            insert_proportion=0.05
            request_distribution=latest
            ;;
        workloade)
            read_proportion=0
            update_proportion=0
            insert_proportion=0.05
            scan_proportion=0.95
            ;;
        workloadf)
            read_proportion=0.5
            update_proportion=0
            read_modify_write_proportion=0.5
            ;;
        *)
            fail "unsupported workload: ${workload}"
            ;;
    esac

    mkdir -p -- "${run_dir}/artifacts"
    log "starting workload=${workload} repetition=${repetition}"
    write_kmsg_marker "${start_marker}"
    prepare_mapper
    verify_fresh_mapper
    dmsetup table "${MAPPER_NAME}" > "${run_dir}/mapper-table.txt"
    cp -- "${DEBUGFS}/stats" "${run_dir}/fresh-mapper.stats"

    set +e
    IMR_LSM_TEST_DESTRUCTIVE=1 \
    IMR_LSM_YCSB_WORKLOAD="${workload}" \
    IMR_LSM_YCSB_RECORD_COUNT="${RECORD_COUNT}" \
    IMR_LSM_YCSB_OPERATION_COUNT="${OPERATION_COUNT}" \
    IMR_LSM_YCSB_THREAD_COUNT="${THREAD_COUNT}" \
    IMR_LSM_YCSB_READ_PROPORTION="${read_proportion}" \
    IMR_LSM_YCSB_UPDATE_PROPORTION="${update_proportion}" \
    IMR_LSM_YCSB_INSERT_PROPORTION="${insert_proportion}" \
    IMR_LSM_YCSB_SCAN_PROPORTION="${scan_proportion}" \
    IMR_LSM_YCSB_DELETE_PROPORTION=0 \
    IMR_LSM_YCSB_READ_MODIFY_WRITE_PROPORTION="${read_modify_write_proportion}" \
    IMR_LSM_YCSB_REQUEST_DISTRIBUTION="${request_distribution}" \
    IMR_LSM_YCSB_DROP_CACHES=0 \
    IMR_LSM_YCSB_CLEAR_READ_TREE=0 \
    IMR_LSM_YCSB_ALLOW_DIRTY=0 \
    IMR_LSM_YCSB_COMPACTION_THRESHOLD="${COMPACTION_THRESHOLD}" \
    IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_BASE="${MAX_BYTES_FOR_LEVEL_BASE}" \
    IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_MULTIPLIER="${MAX_BYTES_FOR_LEVEL_MULTIPLIER}" \
    IMR_LSM_YCSB_CAPTURE_DEVICE_STATS=1 \
    IMR_LSM_YCSB_IMRSIM_UTIL="${IMRSIM_UTIL}" \
    IMR_LSM_YCSB_RESULT_DIR="${run_dir}/artifacts" \
        bash "${YCSB_RUNNER}" "${MAPPER_DEVICE}" 2>&1 |
        tee "${run_dir}/runner.log"
    runner_status="${PIPESTATUS[0]}"
    set -e

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
        append_failure_row "${workload}" "${repetition}" YCSB_FAIL \
            "${kernel_issues}" ycsb_failed
        log "workload=${workload} repetition=${repetition} failed; see ${run_dir}"
        return 1
    fi
    if [[ "${kernel_issues}" -gt 0 ]]; then
        status="KERNEL_ISSUE"
    fi
    summarize_run "${workload}" "${repetition}" "${run_dir}" \
        "${status}" "${kernel_issues}"
    log "completed workload=${workload} repetition=${repetition} status=${status}"

    if [[ "${kernel_issues}" -gt 0 && "${STOP_ON_KERNEL_ISSUE}" == "1" ]]; then
        return 2
    fi
}

median_field()
{
    local workload="$1"
    local column="$2"

    awk -F',' -v workload="${workload}" -v column="${column}" '
        NR > 1 && $1 == workload && $3 == "PASS" &&
        $column ~ /^[0-9]+([.][0-9]+)?$/ { print $column }
    ' "${RUNS_CSV}" | sort -n | awk '
        { values[NR] = $1 }
        END {
            if (NR == 0) {
                print "NA"
            } else if (NR % 2 == 1) {
                print values[(NR + 1) / 2]
            } else {
                printf "%.6f\n", (values[NR / 2] + values[NR / 2 + 1]) / 2
            }
        }
    '
}

write_medians()
{
    local workload
    local successful_runs
    local read_operation
    local write_operation
    local row

    printf '%s\n' \
        'workload,successful_runs,read_operation,write_operation,median_load_throughput_ops_s,median_load_durable_ops_s,median_run_throughput_ops_s,median_run_durable_ops_s,median_read_avg_us,median_read_p95_us,median_read_p99_us,median_read_max_us,median_write_avg_us,median_write_p95_us,median_write_p99_us,median_write_max_us,median_load_imr_wa,median_run_imr_wa,median_load_metadata_wa,median_run_metadata_wa' \
        > "${MEDIANS_CSV}"

    for workload in ${WORKLOADS}; do
        successful_runs="$(awk -F',' -v workload="${workload}" '
            NR > 1 && $1 == workload && $3 == "PASS" { count++ }
            END { print count + 0 }
        ' "${RUNS_CSV}")"
        case "${workload}" in
            workloade) read_operation=SCAN ;;
            *) read_operation=READ ;;
        esac
        case "${workload}" in
            workloada|workloadb) write_operation=UPDATE ;;
            workloadc) write_operation=NONE ;;
            workloadd|workloade) write_operation=INSERT ;;
            workloadf) write_operation=READ-MODIFY-WRITE ;;
        esac
        row=(
            "${workload}" "${successful_runs}" "${read_operation}"
            "${write_operation}"
            "$(median_field "${workload}" 4)" "$(median_field "${workload}" 5)"
            "$(median_field "${workload}" 6)" "$(median_field "${workload}" 7)"
            "$(median_field "${workload}" 10)" "$(median_field "${workload}" 11)"
            "$(median_field "${workload}" 12)" "$(median_field "${workload}" 13)"
            "$(median_field "${workload}" 16)" "$(median_field "${workload}" 17)"
            "$(median_field "${workload}" 18)" "$(median_field "${workload}" 19)"
            "$(median_field "${workload}" 22)" "$(median_field "${workload}" 25)"
            "$(median_field "${workload}" 29)" "$(median_field "${workload}" 33)"
        )
        (IFS=,; printf '%s\n' "${row[*]}") >> "${MEDIANS_CSV}"
    done
}

write_comparison()
{
    awk -F',' '
        NR == 1 { next }
        {
            printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", \
                $1, $7, $3, $9, $11, $4, $13, $18, $20
        }
    ' "${MEDIANS_CSV}" > "${RESULT_ROOT}/comparison.rows"
    {
        printf '# YCSB Workload A-F comparison\n\n'
        printf 'All values are medians of successful runs. Latency is in microseconds.\n\n'
        printf '| Workload | Run throughput (ops/s) | Read op | Read avg | Read P99 | Write op | Write avg | IMR WA | Metadata WA |\n'
        printf '|---|---:|---|---:|---:|---|---:|---:|---:|\n'
        while IFS= read -r line; do
            printf '%s\n' "${line}"
        done < "${RESULT_ROOT}/comparison.rows"
        printf '\nFor Workload E, the read columns report SCAN latency. Workloads D/E use INSERT as the write operation, and Workload F uses READ-MODIFY-WRITE. Workload C has no write operation, so its write-latency fields are `NA`. A WA field is `NA` only when that phase produces no corresponding writes.\n'
    } > "${RESULT_ROOT}/comparison.md"
    rm -f -- "${RESULT_ROOT}/comparison.rows"
}

main()
{
    local repetition
    local workload
    local run_status
    local preflight_issue_count

    require_tools
    validate_config
    mkdir -p -- "${RESULT_ROOT}"
    trap cleanup EXIT
    RUNS_CSV="${RESULT_ROOT}/runs.csv"
    MEDIANS_CSV="${RESULT_ROOT}/medians.csv"
    write_manifest
    dmesg > "${RESULT_ROOT}/preflight-dmesg.log"
    tail -n 1000 "${RESULT_ROOT}/preflight-dmesg.log" |
        grep -Ei "${KERNEL_ISSUE_PATTERN}" \
            > "${RESULT_ROOT}/preflight-kernel-issues.log" || true
    preflight_issue_count="$(awk 'END { print NR + 0 }' \
        "${RESULT_ROOT}/preflight-kernel-issues.log")"
    if [[ "${preflight_issue_count}" -gt 0 ]]; then
        log "WARNING: preflight dmesg contains ${preflight_issue_count} matching line(s)"
    fi

    printf '%s\n' \
        'workload,repetition,status,load_throughput_ops_s,load_durable_ops_s,run_throughput_ops_s,run_durable_ops_s,read_operation,read_operations,read_avg_us,read_p95_us,read_p99_us,read_max_us,write_operation,write_operations,write_avg_us,write_p95_us,write_p99_us,write_max_us,load_imr_write_total,load_imr_extra_writes,load_imr_wa,run_imr_write_total,run_imr_extra_writes,run_imr_wa,load_lsm_ingest_records,load_metadata_compaction_input_bytes,load_metadata_compaction_output_bytes,load_metadata_wa,run_lsm_ingest_records,run_metadata_compaction_input_bytes,run_metadata_compaction_output_bytes,run_metadata_wa,run_compaction_count,run_l0_compaction_count,kernel_issue_count,review' \
        > "${RUNS_CSV}"

    remove_mapper
    for ((repetition = 1; repetition <= REPETITIONS; repetition++)); do
        for workload in ${WORKLOADS}; do
            set +e
            run_one "${workload}" "${repetition}"
            run_status="$?"
            set -e
            if [[ "${run_status}" -eq 1 ]]; then
                fail "YCSB run failed; partial results are in ${RESULT_ROOT}"
            fi
            if [[ "${run_status}" -eq 2 ]]; then
                fail "kernel issue detected; stopped before further benchmark runs"
            fi
        done
    done

    write_medians
    write_comparison
    log "PASS: YCSB Workload A-F comparison completed"
    log "per-run results: ${RUNS_CSV}"
    log "median results: ${MEDIANS_CSV}"
    log "comparison table: ${RESULT_ROOT}/comparison.md"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
