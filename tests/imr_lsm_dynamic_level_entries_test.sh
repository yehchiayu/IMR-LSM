#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
BASE_KEY="${IMR_LSM_DYNAMIC_BASE_KEY:-0}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
BATCH_BLOCKS=40
TOTAL_BLOCKS=$((BATCH_BLOCKS * 2))

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
    for file in stats sorted unsorted segments; do
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
    local pattern="${TMPDIR}/batch-${first_key}.bin"

    make_pattern_range "${seed}" "${BATCH_BLOCKS}" "${pattern}"
    log "write ${BATCH_BLOCKS} blocks at key=${first_key}"
    dd if="${pattern}" of="${DEVICE}" bs="${BLOCK_SIZE}" seek="${first_key}" \
        count="${BATCH_BLOCKS}" conv=notrunc oflag=direct status=none
    sync
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
    assert_stat_equals active_write_level 5
    assert_stat_equals dynamic_base_level 5
    assert_stat_equals compaction_count 3
    assert_stat_string_equals last_compaction_from L5
    assert_stat_string_equals last_compaction_to L6
    assert_stat_equals last_compaction_input 16
    assert_stat_equals last_compaction_output_total 33

    assert_level_summary sorted 6 sorted_count count 33
    assert_level_summary sorted 6 sorted_count target 32
    assert_level_summary sorted 6 sorted_count segments 3
    assert_level_summary unsorted 6 unsorted_count count 0
    assert_level_summary unsorted 5 unsorted_count count 7
    assert_level_summary unsorted 5 unsorted_count target 16
}

assert_after_second_batch()
{
    assert_stat_equals active_write_level 4
    assert_stat_equals dynamic_base_level 4
    assert_stat_equals compaction_count 5
    assert_stat_string_equals last_compaction_from L4
    assert_stat_string_equals last_compaction_to L5
    assert_stat_equals last_compaction_input 16
    assert_stat_equals last_compaction_output_total 16

    assert_level_summary sorted 6 sorted_count count 49
    assert_level_summary sorted 6 sorted_count target 48
    assert_level_summary sorted 5 sorted_count count 16
    assert_level_summary sorted 5 sorted_count target 24
    assert_level_summary unsorted 4 unsorted_count count 15
    assert_level_summary unsorted 4 unsorted_count target 16
}

main()
{
    require_root
    require_device
    require_debugfs
    require_tools
    require_range
    require_fresh_metadata

    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "${TMPDIR}"' EXIT

    log "device=${DEVICE} debugfs=${DEBUGFS} base_key=${BASE_KEY}"
    log "validating RocksDB-style dynamic level bytes as dynamic level entries"

    write_batch "${BASE_KEY}" 10
    assert_after_first_batch

    write_batch "$((BASE_KEY + BATCH_BLOCKS))" 90
    assert_after_second_batch

    verify_block "${BASE_KEY}" 10
    verify_block "$((BASE_KEY + BATCH_BLOCKS - 1))" "$((10 + BATCH_BLOCKS - 1))"
    verify_block "$((BASE_KEY + BATCH_BLOCKS))" 90
    verify_block "$((BASE_KEY + TOTAL_BLOCKS - 1))" "$((90 + BATCH_BLOCKS - 1))"

    log "PASS: dynamic level entries move insert target L6 -> L5 -> L4 with expected metadata distribution"
}

main "$@"
