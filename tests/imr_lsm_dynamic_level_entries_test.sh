#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
BASE_KEY="${IMR_LSM_DYNAMIC_BASE_KEY:-0}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
L0_THRESHOLD=16
RECORD_BYTES=32
BASE_BYTES=$((L0_THRESHOLD * RECORD_BYTES))
LEVEL_MULTIPLIER=10
BATCH_BLOCKS=40
DEEP_BATCH_BLOCKS=240
TOTAL_BLOCKS=$((BATCH_BLOCKS * 2 + DEEP_BATCH_BLOCKS))

log()
{
    printf '[imr-lsm dynamic-level] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm dynamic-level] FAIL: %s\n' "$*" >&2
    exit 1
}

require_root()
{
    if [[ "${EUID}" -ne 0 ]]; then
        fail "run as root, for example: sudo $0 ${DEVICE}"
    fi
}

require_device()
{
    if [[ ! -b "${DEVICE}" ]]; then
        fail "${DEVICE} is not a block device"
    fi

    case "${DEVICE}" in
        /dev/mapper/imrsim|/dev/mapper/imrsim[0-9]*)
            ;;
        *)
            if [[ "${IMR_LSM_TEST_ALLOW_ANY_DEVICE:-0}" != "1" ]]; then
                fail "refusing to write ${DEVICE}; set IMR_LSM_TEST_ALLOW_ANY_DEVICE=1 to override"
            fi
            ;;
    esac
}

require_debugfs()
{
    local file

    [[ -d "${DEBUGFS}" ]] ||
        fail "missing ${DEBUGFS}; load dm-imrsim and mount debugfs"
    for file in stats sorted unsorted segments compaction_threshold \
        max_bytes_for_level_base max_bytes_for_level_multiplier; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

require_tools()
{
    local tool

    for tool in awk blockdev cmp dd perl mktemp sync; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
}

require_range()
{
    local sectors
    local blocks

    imr_lsm_test_require_nonnegative_integer BASE_KEY "${BASE_KEY}"
    [[ "${BASE_KEY}" -ge 0 ]] ||
        fail "BASE_KEY=${BASE_KEY} must be >= 0"
    sectors="$(blockdev --getsz "${DEVICE}")" ||
        fail "cannot read sector count for ${DEVICE}"
    blocks=$((sectors / SECTORS_PER_BLOCK))
    [[ $((BASE_KEY + TOTAL_BLOCKS)) -le "${blocks}" ]] ||
        fail "test range exceeds device blocks: base=${BASE_KEY} total=${TOTAL_BLOCKS} device_blocks=${blocks}"
}

stat_value()
{
    local key="$1"

    awk -F': ' -v key="${key}" '$1 == key { print $2; found = 1; exit }
        END { if(!found) exit 1 }' "${DEBUGFS}/stats"
}

stat_number()
{
    local key="$1"
    local value

    value="$(stat_value "${key}")" ||
        fail "missing stat ${key}"
    case "${value}" in
        ''|*[!0-9]*)
            fail "stat ${key} is not numeric: ${value}"
            ;;
    esac
    printf '%s\n' "${value}"
}

segment_count()
{
    awk -F': ' '$1 == "segment_count" { print $2; found = 1; exit }
        END { if(!found) exit 1 }' "${DEBUGFS}/segments" ||
        fail "missing segment_count"
}

require_fresh_metadata()
{
    local key
    local value

    if [[ "${IMR_LSM_DYNAMIC_ALLOW_DIRTY:-0}" == "1" ]]; then
        log "WARNING: IMR_LSM_DYNAMIC_ALLOW_DIRTY=1 skips fresh-metadata guard; exact assertions may fail"
        return
    fi

    for key in logical_write_count lsm_record_insert_count lsm_write_count delete_count compaction_count; do
        value="$(stat_number "${key}")"
        [[ "${value}" -eq 0 ]] ||
            fail "fresh mapper required: ${key}=${value}; recreate mapper before this test"
    done
    value="$(segment_count)"
    [[ "${value}" -eq 0 ]] ||
        fail "fresh mapper required: segment_count=${value}; recreate mapper before this test"
}

assert_stat_equals()
{
    local key="$1"
    local expected="$2"
    local actual

    actual="$(stat_number "${key}")"
    [[ "${actual}" -eq "${expected}" ]] ||
        fail "${key}: expected ${expected}, got ${actual}"
    log "PASS: ${key}=${actual}"
}

assert_stat_equals_stat()
{
    local actual_key="$1"
    local expected_key="$2"
    local actual
    local expected

    actual="$(stat_number "${actual_key}")"
    expected="$(stat_number "${expected_key}")"
    [[ "${actual}" -eq "${expected}" ]] ||
        fail "${actual_key}: expected ${expected_key}=${expected}, got ${actual}"
    log "PASS: ${actual_key}=${actual} matches ${expected_key}"
}

assert_stat_ge()
{
    local key="$1"
    local expected="$2"
    local actual

    actual="$(stat_number "${key}")"
    [[ "${actual}" -ge "${expected}" ]] ||
        fail "${key}: expected >= ${expected}, got ${actual}"
    log "PASS: ${key}=${actual} >= ${expected}"
}

assert_stat_string_equals()
{
    local key="$1"
    local expected="$2"
    local actual

    actual="$(stat_value "${key}")" ||
        fail "missing stat ${key}"
    [[ "${actual}" == "${expected}" ]] ||
        fail "${key}: expected ${expected}, got ${actual}"
    log "PASS: ${key}=${actual}"
}

level_summary_value()
{
    local file="$1"
    local level="$2"
    local label="$3"
    local field="$4"

    awk -v level="${level}" -v label="${label}" -v field="${field}" '
        $1 == "level" && $2 == level && $3 == label ":" {
            if(field == "count"){
                print $4
            }else if(field == "target"){
                print $6
            }else if(field == "segments"){
                print $8
            }else{
                exit 2
            }
            found = 1
            exit
        }
        END { if(!found) exit 1 }
    ' "${file}" || fail "missing ${label} summary for level ${level} in ${file}"
}

assert_level_summary()
{
    local file_name="$1"
    local level="$2"
    local label="$3"
    local field="$4"
    local expected="$5"
    local actual

    actual="$(level_summary_value "${DEBUGFS}/${file_name}" "${level}" "${label}" "${field}")"
    [[ "${actual}" -eq "${expected}" ]] ||
        fail "${file_name} L${level} ${field}: expected ${expected}, got ${actual}"
    log "PASS: ${file_name} L${level} ${field}=${actual}"
}

assert_debug_value_equals()
{
    local file_name="$1"
    local key="$2"
    local expected="$3"
    local actual

    actual="$(awk -F': ' -v key="${key}" '$1 == key { print $2; found = 1; exit }
        END { if(!found) exit 1 }' "${DEBUGFS}/${file_name}")" ||
        fail "missing ${key} in ${DEBUGFS}/${file_name}"
    [[ "${actual}" == "${expected}" ]] ||
        fail "${file_name} ${key}: expected ${expected}, got ${actual}"
    log "PASS: ${file_name} ${key}=${actual}"
}

assert_level_target_ratio()
{
    local file_name="$1"
    local upper_level="$2"
    local lower_level="$3"
    local expected_ratio="$4"
    local upper_target
    local lower_target

    upper_target="$(level_summary_value "${DEBUGFS}/${file_name}" \
        "${upper_level}" sorted_count target)"
    lower_target="$(level_summary_value "${DEBUGFS}/${file_name}" \
        "${lower_level}" sorted_count target)"
    [[ "${lower_target}" -eq $((upper_target * expected_ratio)) ]] ||
        fail "${file_name} L${upper_level}->L${lower_level} target ratio: expected ${expected_ratio}x, got ${upper_target}->${lower_target}"
    log "PASS: ${file_name} L${upper_level}->L${lower_level} target ratio=${expected_ratio}x (${upper_target}->${lower_target})"
}

assert_level_byte_target()
{
    local level="$1"
    local expected="$2"

    assert_stat_equals "level${level}_target_bytes" "${expected}"
}

assert_rocksdb_target_chain()
{
    local upper_level="$1"
    local lower_level="$2"
    local upper
    local lower

    upper="$(stat_number "level${upper_level}_target_bytes")"
    lower="$(stat_number "level${lower_level}_target_bytes")"
    [[ "${lower}" -le $((upper * LEVEL_MULTIPLIER)) &&
       "${lower}" -gt $(((upper - 1) * LEVEL_MULTIPLIER)) ]] ||
        fail "L${upper_level}->L${lower_level} byte targets do not use ceil(${lower}/${LEVEL_MULTIPLIER}): ${upper}->${lower}"
    log "PASS: L${upper_level}->L${lower_level} byte targets preserve ${LEVEL_MULTIPLIER}x fanout (${upper}->${lower})"
}

configure_dynamic_options()
{
    local level

    printf '%s\n' "${L0_THRESHOLD}" > "${DEBUGFS}/compaction_threshold"
    printf '%s\n' "${BASE_BYTES}" > "${DEBUGFS}/max_bytes_for_level_base"
    printf '%s\n' "${LEVEL_MULTIPLIER}" > \
        "${DEBUGFS}/max_bytes_for_level_multiplier"

    assert_stat_equals compaction_threshold "${L0_THRESHOLD}"
    assert_stat_equals max_bytes_for_level_base "${BASE_BYTES}"
    assert_stat_equals max_bytes_for_level_multiplier "${LEVEL_MULTIPLIER}"
    assert_stat_equals effective_max_bytes_for_level_multiplier \
        "${LEVEL_MULTIPLIER}"
    assert_stat_equals lsm_record_bytes "${RECORD_BYTES}"
    assert_stat_equals level_compaction_dynamic_level_bytes 1
    assert_stat_equals level_compaction_background 1
    assert_stat_equals level_compaction_work_round_limit 1
    assert_stat_equals level_compaction_work_error_count 0
    assert_stat_equals level_compaction_time_count 0
    assert_stat_equals level_compaction_work_time_count 0
    assert_stat_equals compaction_queue_depth 0
    assert_stat_equals compaction_queue_depth_max 0
    assert_stat_equals level_compaction_zone_lock_hold_count 0
    assert_stat_equals level_compaction_lsm_lock_hold_count 0
    assert_stat_equals level_compaction_post_round_count 0
    assert_stat_equals level_compaction_post_recalc_count 0
    assert_stat_equals level_compaction_post_recalc_total_ns 0
    assert_stat_equals level_compaction_post_recalc_max_ns 0
    assert_stat_equals last_level_compaction_post_recalc_ns 0
    assert_stat_equals invalid_recalc_count 0
    assert_stat_equals invalid_recalc_total_ns 0
    assert_stat_equals invalid_recalc_segments_scanned_total 0
    assert_stat_equals invalid_recalc_entries_scanned_total 0
    assert_stat_equals last_invalid_recalc_segments 0
    assert_stat_equals last_invalid_recalc_entries 0
    assert_stat_equals level_compaction_input_entries_total 0
    assert_stat_equals level_compaction_input_entries_max 0
    assert_stat_equals last_level_compaction_input_entries 0
    assert_stat_equals level_compaction_output_entries_total 0
    assert_stat_equals level_compaction_output_entries_max 0
    assert_stat_equals last_level_compaction_output_entries 0
    assert_stat_equals level_compaction_coalesced_schedule_count 0
    assert_stat_equals level_compaction_no_work_run_count 0
    assert_stat_equals level_compaction_score_max 0
    assert_stat_equals last_level_compaction_evaluated_score 0
    assert_stat_equals last_level_compaction_schedule_score 0
    assert_stat_equals last_level_compaction_requeue_score 0
    for level in 0 1 2 3 4 5 6; do
        assert_stat_equals \
            "level${level}_compaction_input_entries_total" 0
        assert_stat_equals \
            "level${level}_compaction_input_entries_max" 0
        assert_stat_equals \
            "level${level}_compaction_output_entries_total" 0
        assert_stat_equals \
            "level${level}_compaction_output_entries_max" 0
    done
    assert_stat_string_equals level_size_unit logical_metadata_bytes
}

make_pattern_range()
{
    local seed="$1"
    local blocks="$2"
    local path="$3"

    perl -e '
        my ($seed, $blocks, $block_size) = @ARGV;
        for(my $i = 0; $i < $blocks; $i++){
            print chr((($seed + $i) % 251) + 1) x $block_size;
        }
    ' "${seed}" "${blocks}" "${BLOCK_SIZE}" > "${path}"
}

write_batch()
{
    local first_key="$1"
    local seed="$2"
    local blocks="${3:-${BATCH_BLOCKS}}"
    local pattern="${TMPDIR}/batch-${first_key}.bin"

    make_pattern_range "${seed}" "${blocks}" "${pattern}"
    log "write ${blocks} blocks at key=${first_key}"
    dd if="${pattern}" of="${DEVICE}" bs="${BLOCK_SIZE}" seek="${first_key}" \
        count="${blocks}" conv=notrunc oflag=direct status=none
    sync
    imr_lsm_test_wait_level_compaction_idle "${DEBUGFS}"
}

verify_block()
{
    local key="$1"
    local seed="$2"
    local expected="${TMPDIR}/expected-${key}.bin"
    local output="${TMPDIR}/read-${key}.bin"

    make_pattern_range "${seed}" 1 "${expected}"
    rm -f "${output}"
    dd if="${DEVICE}" of="${output}" bs="${BLOCK_SIZE}" skip="${key}" \
        count=1 iflag=direct status=none
    cmp -s "${expected}" "${output}" ||
        fail "payload mismatch for key=${key}"
    log "PASS: key=${key} readback matches"
}

assert_after_first_batch()
{
    assert_debug_value_equals sorted level_ratio "${LEVEL_MULTIPLIER}"
    assert_stat_equals active_write_level 0
    assert_stat_equals dynamic_base_level 5
    assert_stat_equals compaction_count 2
    assert_stat_equals level_compaction_pending 0
    assert_stat_equals level_compaction_running 0
    assert_stat_equals compaction_queue_depth 0
    assert_stat_equals compaction_queue_depth_max 1
    assert_stat_equals level_compaction_work_round_count 2
    assert_stat_equals level_compaction_time_count 2
    assert_stat_ge level_compaction_work_time_count 2
    assert_stat_ge level_compaction_queue_wait_count 2
    assert_stat_ge level_compaction_total_ns 1
    assert_stat_ge level_compaction_max_ns 1
    assert_stat_equals level0_compaction_time_count 2
    assert_stat_ge level0_compaction_total_ns 1
    assert_stat_ge level0_compaction_max_ns 1
    assert_stat_ge level_compaction_work_total_ns 1
    assert_stat_ge level_compaction_work_max_ns 1
    assert_stat_ge level_compaction_zone_lock_wait_count 2
    assert_stat_ge level_compaction_lsm_lock_wait_count 2
    assert_stat_ge level_compaction_zone_lock_hold_count 2
    assert_stat_ge level_compaction_zone_lock_hold_total_ns 1
    assert_stat_ge level_compaction_zone_lock_hold_max_ns 1
    assert_stat_ge last_level_compaction_zone_lock_hold_ns 1
    assert_stat_ge level_compaction_lsm_lock_hold_count 2
    assert_stat_ge level_compaction_lsm_lock_hold_total_ns 1
    assert_stat_ge level_compaction_lsm_lock_hold_max_ns 1
    assert_stat_ge last_level_compaction_lsm_lock_hold_ns 1
    assert_stat_ge level_compaction_post_round_count 2
    assert_stat_ge level_compaction_post_round_total_ns 1
    assert_stat_ge level_compaction_post_round_max_ns 1
    assert_stat_ge last_level_compaction_post_round_ns 1
    assert_stat_equals level_compaction_post_recalc_count 0
    assert_stat_equals level_compaction_post_recalc_total_ns 0
    assert_stat_equals level_compaction_post_recalc_max_ns 0
    assert_stat_equals last_level_compaction_post_recalc_ns 0
    assert_stat_equals invalid_recalc_count 2
    assert_stat_ge invalid_recalc_total_ns 1
    assert_stat_ge invalid_recalc_max_ns 1
    assert_stat_ge last_invalid_recalc_ns 1
    assert_stat_equals invalid_recalc_segments_scanned_total 3
    assert_stat_equals invalid_recalc_segments_scanned_max 2
    assert_stat_equals invalid_recalc_entries_scanned_total 48
    assert_stat_equals invalid_recalc_entries_scanned_max 32
    assert_stat_equals last_invalid_recalc_segments 2
    assert_stat_equals last_invalid_recalc_entries 32
    assert_stat_equals level_compaction_input_entries_total 32
    assert_stat_equals level_compaction_input_entries_max 16
    assert_stat_equals last_level_compaction_input_entries 16
    assert_stat_equals level_compaction_output_entries_total 32
    assert_stat_equals level_compaction_output_entries_max 16
    assert_stat_equals last_level_compaction_output_entries 16
    assert_stat_equals level0_compaction_input_entries_total 32
    assert_stat_equals level0_compaction_input_entries_max 16
    assert_stat_equals level0_compaction_output_entries_total 32
    assert_stat_equals level0_compaction_output_entries_max 16
    assert_stat_ge level_compaction_score_max 1000
    assert_stat_ge last_level_compaction_schedule_score 1000
    assert_stat_ge imr_lsm_lock_wait_count 2
    assert_stat_ge imr_lsm_lock_wait_total_ns 1
    assert_stat_string_equals last_compaction_from L0
    assert_stat_string_equals last_compaction_to L6
    assert_stat_equals last_compaction_input 16
    assert_stat_equals last_compaction_output_total 32
    assert_stat_equals last_compaction_input_bytes 512
    assert_stat_equals last_compaction_output_bytes 512
    assert_stat_equals metadata_compaction_input_bytes 1024
    assert_stat_equals metadata_compaction_output_bytes 1024

    assert_level_summary sorted 6 sorted_count count 32
    assert_level_summary sorted 6 sorted_count target 32
    assert_level_summary sorted 6 sorted_count segments 2
    assert_level_summary unsorted 6 unsorted_count count 0
    assert_level_summary unsorted 0 unsorted_count count 8
    assert_level_summary unsorted 0 unsorted_count target 16
    assert_level_byte_target 5 103
    assert_level_byte_target 6 1024
    assert_stat_equals level6_actual_bytes 1024
    assert_rocksdb_target_chain 5 6
}

assert_after_second_batch()
{
    assert_stat_equals active_write_level 0
    assert_stat_equals dynamic_base_level 5
    assert_stat_ge compaction_count 7
    assert_stat_equals level_compaction_pending 0
    assert_stat_equals level_compaction_running 0
    assert_stat_ge level_compaction_work_round_count 7
    assert_stat_equals_stat invalid_recalc_count \
        level_compaction_work_round_count
    assert_stat_equals level_compaction_post_recalc_count 0
    assert_stat_equals level_compaction_post_recalc_total_ns 0
    assert_stat_string_equals last_compaction_from L5
    assert_stat_string_equals last_compaction_to L6
    assert_stat_equals last_compaction_input 16
    assert_stat_equals last_compaction_output_total 80

    assert_level_summary sorted 6 sorted_count count 80
    assert_level_summary sorted 6 sorted_count target 80
    assert_level_summary unsorted 0 unsorted_count count 0
    assert_level_byte_target 5 256
    assert_level_byte_target 6 2560
    assert_rocksdb_target_chain 5 6
}

assert_after_deep_batch()
{
    assert_stat_equals active_write_level 0
    assert_stat_equals dynamic_base_level 4
    assert_stat_string_equals lowest_unnecessary_level -1
    assert_stat_ge compaction_count 20
    assert_stat_equals level_compaction_pending 0
    assert_stat_equals level_compaction_running 0
    assert_stat_equals level_compaction_work_error_count 0
    assert_stat_ge level_compaction_work_round_count 20
    assert_stat_equals_stat invalid_recalc_count \
        level_compaction_work_round_count
    assert_stat_equals level_compaction_post_recalc_count 0
    assert_stat_equals level_compaction_post_recalc_total_ns 0

    assert_level_summary sorted 6 sorted_count count 304
    assert_level_summary sorted 5 sorted_count count 16
    assert_level_summary unsorted 0 unsorted_count count 0
    assert_level_byte_target 4 98
    assert_level_byte_target 5 973
    assert_level_byte_target 6 9728
    assert_stat_equals level6_actual_bytes 9728
    assert_rocksdb_target_chain 4 5
    assert_rocksdb_target_chain 5 6
}

main()
{
    require_root
    require_device
    require_debugfs
    require_tools
    imr_lsm_test_safety_begin "${DEVICE}"
    require_range
    require_fresh_metadata
    configure_dynamic_options

    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "${TMPDIR}"' EXIT

    log "device=${DEVICE} debugfs=${DEBUGFS} base_key=${BASE_KEY}"
    log "validating RocksDB-style L0-to-dynamic-base byte targets"

    write_batch "${BASE_KEY}" 10
    assert_after_first_batch

    write_batch "$((BASE_KEY + BATCH_BLOCKS))" 90
    assert_after_second_batch

    write_batch "$((BASE_KEY + BATCH_BLOCKS * 2))" 130 \
        "${DEEP_BATCH_BLOCKS}"
    assert_after_deep_batch

    verify_block "${BASE_KEY}" 10
    verify_block "$((BASE_KEY + BATCH_BLOCKS - 1))" "$((10 + BATCH_BLOCKS - 1))"
    verify_block "$((BASE_KEY + BATCH_BLOCKS))" 90
    verify_block "$((BASE_KEY + BATCH_BLOCKS * 2))" 130
    verify_block "$((BASE_KEY + TOTAL_BLOCKS - 1))" \
        "$((130 + DEEP_BATCH_BLOCKS - 1))"

    log "PASS: L0 compacts to a byte-sized dynamic base with a 10x level fanout"
}

main "$@"
