#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_SWEEP_ZONE:-2}"
KEY_OFFSET="${IMR_LSM_SWEEP_KEY_OFFSET:-32768}"
READ_TREE_LIMITS="${IMR_LSM_SWEEP_READ_TREE_LIMITS:-1 2 8 0}"
WRITE_SIZES="${IMR_LSM_SWEEP_WRITE_SIZES:-4096 8192 16384 65536}"
COMPACTION_THRESHOLDS="${IMR_LSM_SWEEP_COMPACTION_THRESHOLDS:-8 16 32}"
BLOOM_BITS_PER_KEY_VALUES="${IMR_LSM_SWEEP_BLOOM_BITS_PER_KEY:-4 10 16}"
DEVICE_ZONE_COUNTS="${IMR_LSM_SWEEP_DEVICE_ZONES:-}"
MAPPER_PREFIX="${IMR_LSM_SWEEP_MAPPER_PREFIX:-imrsim_sweep}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
TOTAL_ITEMS=65536
ZONE_BYTES=$((256 * 1024 * 1024))
PSTORE_BYTES=$((2 * 1024 * 1024))

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT=""
TMPDIR=""
ACTIVE_DEVICE=""
ACTIVE_ZONE=0
NEXT_OFFSET=0
ALLOCATED_FIRST_KEY=0
TEMP_MAPPERS=()
TEMP_LOOPS=()

log()
{
    printf '[imr-lsm sweep] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm sweep] FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    local idx
    local file

    if [[ -d "${DEBUGFS}" ]]; then
        for file in read_tree_limit compaction_threshold bloom_bits_per_key; do
            if [[ -e "${DEBUGFS}/${file}" ]]; then
                printf '0\n' > "${DEBUGFS}/${file}" 2>/dev/null || true
            fi
        done
    fi

    for ((idx = ${#TEMP_MAPPERS[@]} - 1; idx >= 0; idx--)); do
        dmsetup remove "${TEMP_MAPPERS[idx]}" >/dev/null 2>&1 || true
    done
    for ((idx = ${#TEMP_LOOPS[@]} - 1; idx >= 0; idx--)); do
        losetup -d "${TEMP_LOOPS[idx]}" >/dev/null 2>&1 || true
    done
    if [[ -n "${TMP_ROOT}" ]]; then
        rm -rf "${TMP_ROOT}"
    fi
}

require_root()
{
    if [[ "${EUID}" -ne 0 ]]; then
        fail "run as root, for example: sudo $0 ${DEVICE}"
    fi
}

require_tools()
{
    local tool

    for tool in awk blockdev cmp dd perl mktemp sync tr; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
}

require_device_sweep_tools()
{
    local tool

    for tool in bc dmsetup losetup truncate; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing device sweep tool: ${tool}"
    done
    [[ -x "${REPO_ROOT}/imrsim_util/imr_format.sh" ]] ||
        fail "missing executable ${REPO_ROOT}/imrsim_util/imr_format.sh"
}

require_debugfs()
{
    local file

    [[ -d "${DEBUGFS}" ]] ||
        fail "missing ${DEBUGFS}; load dm-imrsim and mount debugfs"
    for file in stats segments block_table clear_read_tree read_tree read_tree_limit; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

require_device()
{
    local device="$1"

    [[ -b "${device}" ]] || fail "${device} is not a block device"

    case "${device}" in
        /dev/mapper/imrsim|/dev/mapper/imrsim[0-9]*|/dev/mapper/${MAPPER_PREFIX}*)
            ;;
        *)
            if [[ "${IMR_LSM_TEST_ALLOW_ANY_DEVICE:-0}" != "1" ]]; then
                fail "refusing to write ${device}; set IMR_LSM_TEST_ALLOW_ANY_DEVICE=1 to override"
            fi
            ;;
    esac
}

device_zone_count()
{
    local device="$1"
    local sectors

    sectors="$(blockdev --getsz "${device}")" ||
        fail "cannot read sector count for ${device}"
    printf '%u\n' $((sectors / SECTORS_PER_BLOCK / TOTAL_ITEMS))
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

next_segment_id()
{
    awk -F': ' '$1 == "next_segment_id" { print $2; found = 1; exit }
        END { if(!found) exit 1 }' "${DEBUGFS}/segments" ||
        fail "missing next_segment_id"
}

debugfs_number()
{
    local file="$1"
    local value

    value="$(tr -d '[:space:]' < "${DEBUGFS}/${file}")" ||
        fail "cannot read ${DEBUGFS}/${file}"
    case "${value}" in
        ''|*[!0-9]*)
            fail "${file} is not numeric: ${value}"
            ;;
    esac
    printf '%s\n' "${value}"
}

set_debugfs_number()
{
    local file="$1"
    local value="$2"

    printf '%s\n' "${value}" > "${DEBUGFS}/${file}"
}

have_debugfs_file()
{
    [[ -e "${DEBUGFS}/$1" ]]
}

assert_counter_delta_eq()
{
    local key="$1"
    local before="$2"
    local expected="$3"
    local label="$4"
    local after
    local delta

    after="$(stat_number "${key}")"
    delta=$((after - before))
    [[ "${delta}" -eq "${expected}" ]] ||
        fail "${label}: ${key} delta expected ${expected}, got ${delta}"
    log "PASS: ${label}: ${key} +${delta}"
}

assert_counter_delta_ge()
{
    local key="$1"
    local before="$2"
    local minimum="$3"
    local label="$4"
    local after
    local delta

    after="$(stat_number "${key}")"
    delta=$((after - before))
    [[ "${delta}" -ge "${minimum}" ]] ||
        fail "${label}: ${key} delta expected >= ${minimum}, got ${delta}"
    log "PASS: ${label}: ${key} +${delta} >= ${minimum}"
}

clear_read_tree()
{
    set_debugfs_number clear_read_tree 1
}

compact_active_level()
{
    local level

    if [[ ! -e "${DEBUGFS}/compact" ]]; then
        return
    fi
    level="$(stat_number active_write_level)"
    set_debugfs_number compact "${level}" || true
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

write_payload_once()
{
    local first_key="$1"
    local bytes="$2"
    local pattern="$3"
    local seek_units

    [[ $(((first_key * BLOCK_SIZE) % bytes)) -eq 0 ]] ||
        fail "key ${first_key} is not aligned for ${bytes}-byte write"
    seek_units=$(((first_key * BLOCK_SIZE) / bytes))
    dd if="${pattern}" of="${ACTIVE_DEVICE}" bs="${bytes}" seek="${seek_units}" \
        count=1 conv=notrunc oflag=direct status=none
}

write_4k_blocks()
{
    local first_key="$1"
    local blocks="$2"
    local seed="$3"
    local i

    for ((i = 0; i < blocks; i++)); do
        local pattern="${TMPDIR}/write-${first_key}-${i}.bin"

        make_pattern_range "$((seed + i))" 1 "${pattern}"
        dd if="${pattern}" of="${ACTIVE_DEVICE}" bs="${BLOCK_SIZE}" \
            seek="$((first_key + i))" count=1 conv=notrunc oflag=direct \
            status=none
    done
}

read_block()
{
    local key="$1"
    local output="$2"

    rm -f "${output}"
    dd if="${ACTIVE_DEVICE}" of="${output}" bs="${BLOCK_SIZE}" skip="${key}" \
        count=1 iflag=direct status=none
}

verify_payload_range()
{
    local first_key="$1"
    local blocks="$2"
    local pattern="$3"
    local label="$4"
    local before_fallback
    local i

    clear_read_tree
    before_fallback="$(stat_number fallback_count)"
    for ((i = 0; i < blocks; i++)); do
        local expected="${TMPDIR}/expected-${first_key}-${i}.bin"
        local output="${TMPDIR}/read-${first_key}-${i}.bin"

        dd if="${pattern}" of="${expected}" bs="${BLOCK_SIZE}" skip="${i}" \
            count=1 status=none
        read_block "$((first_key + i))" "${output}"
        cmp -s "${expected}" "${output}" ||
            fail "${label}: payload mismatch at key=$((first_key + i))"
    done
    assert_counter_delta_eq fallback_count "${before_fallback}" 0 \
        "${label}: no stale fallback"
}

block_table_entries_in_range()
{
    local first_key="$1"
    local count="$2"

    awk -v first="${first_key}" -v count="${count}" '
        $1 ~ /^[0-9]+$/ && $5 >= first && $5 < first + count { hits++ }
        END { print hits + 0 }
    ' "${DEBUGFS}/block_table"
}

new_segment_blooms_after_id()
{
    local first_segment_id="$1"

    awk -v first_id="${first_segment_id}" '
        $1 ~ /^[0-9]+$/ && $1 >= first_id {
            print $1, $11, $12, $14
            found = 1
        }
        END { if(!found) exit 1 }
    ' "${DEBUGFS}/segments"
}

next_power_of_two()
{
    local value="$1"
    local result=1

    while [[ "${result}" -lt "${value}" ]]; do
        result=$((result * 2))
    done
    printf '%u\n' "${result}"
}

expected_bloom_bits()
{
    local key_count="$1"
    local bits_per_key="$2"
    local target=$((key_count * bits_per_key))

    [[ "${target}" -lt 256 ]] && target=256
    [[ "${target}" -gt 16384 ]] && target=16384
    target="$(next_power_of_two "${target}")"
    [[ "${target}" -lt 256 ]] && target=256
    [[ "${target}" -gt 16384 ]] && target=16384
    printf '%u\n' "${target}"
}

alloc_key_range()
{
    local blocks="$1"
    local align="${2:-16}"
    local offset

    offset=$((((NEXT_OFFSET + align - 1) / align) * align))
    [[ $((offset + blocks)) -le "${TOTAL_ITEMS}" ]] ||
        fail "not enough key space in zone ${ACTIVE_ZONE}: offset=${offset} blocks=${blocks}"
    NEXT_OFFSET=$((offset + blocks + align))
    ALLOCATED_FIRST_KEY=$((ACTIVE_ZONE * TOTAL_ITEMS + offset))
}

test_write_sizes()
{
    local size

    log "sweep write size: ${WRITE_SIZES}"
    for size in ${WRITE_SIZES}; do
        [[ $((size % BLOCK_SIZE)) -eq 0 ]] ||
            fail "write size ${size} is not 4K aligned"

        local blocks=$((size / BLOCK_SIZE))
        local first_key
        local pattern
        local before_insert

        alloc_key_range "${blocks}" "${blocks}"
        first_key="${ALLOCATED_FIRST_KEY}"
        pattern="${TMPDIR}/write-size-${size}.bin"
        make_pattern_range "${size}" "${blocks}" "${pattern}"

        before_insert="$(stat_number lsm_record_insert_count)"
        write_payload_once "${first_key}" "${size}" "${pattern}"
        sync

        assert_counter_delta_eq lsm_record_insert_count "${before_insert}" \
            "${blocks}" "${size}-byte write records ${blocks} keys"
        verify_payload_range "${first_key}" "${blocks}" "${pattern}" \
            "${size}-byte write readback"
    done
}

test_read_tree_limits()
{
    local limit

    if ! have_debugfs_file read_tree_limit; then
        log "SKIP: read_tree_limit debugfs knob not present"
        return
    fi

    log "sweep read_tree_limit: ${READ_TREE_LIMITS}"
    for limit in ${READ_TREE_LIMITS}; do
        local actual_limit
        local exercise_count
        local first_key
        local pattern
        local before_evict
        local tree_size
        local i

        set_debugfs_number read_tree_limit "${limit}"
        actual_limit="$(debugfs_number read_tree_limit)"
        if [[ "${actual_limit}" -gt 32 ]]; then
            exercise_count=3
        else
            exercise_count=$((actual_limit + 2))
        fi

        alloc_key_range "${exercise_count}" 16
        first_key="${ALLOCATED_FIRST_KEY}"
        pattern="${TMPDIR}/read-tree-limit-${limit}.bin"
        make_pattern_range "$((1000 + limit))" "${exercise_count}" "${pattern}"
        write_4k_blocks "${first_key}" "${exercise_count}" "$((1000 + limit))"

        clear_read_tree
        before_evict="$(stat_number read_tree_evict_count)"
        for ((i = 0; i < exercise_count; i++)); do
            local expected="${TMPDIR}/rtl-${limit}-${i}.expected"
            local output="${TMPDIR}/rtl-${limit}-${i}.out"

            dd if="${pattern}" of="${expected}" bs="${BLOCK_SIZE}" skip="${i}" \
                count=1 status=none
            read_block "$((first_key + i))" "${output}"
            cmp -s "${expected}" "${output}" ||
                fail "read_tree_limit=${actual_limit}: payload mismatch key=$((first_key + i))"
        done

        tree_size="$(stat_number read_tree_size)"
        [[ "${tree_size}" -le "${actual_limit}" ]] ||
            fail "read_tree_limit=${actual_limit}: tree size=${tree_size}"
        if [[ "${exercise_count}" -gt "${actual_limit}" ]]; then
            assert_counter_delta_ge read_tree_evict_count "${before_evict}" 1 \
                "read_tree_limit=${actual_limit} eviction"
        fi
        log "PASS: read_tree_limit=${actual_limit} tree_size=${tree_size}"
    done
    set_debugfs_number read_tree_limit 0
}

test_compaction_thresholds()
{
    local threshold

    if ! have_debugfs_file compaction_threshold; then
        log "SKIP: compaction_threshold debugfs knob not present"
        return
    fi

    log "sweep compaction_threshold: ${COMPACTION_THRESHOLDS}"
    for threshold in ${COMPACTION_THRESHOLDS}; do
        local write_count=$((threshold + 2))
        local first_key
        local pattern
        local before_insert
        local before_compaction
        local before_segment_count
        local after_segment_count
        local hits

        compact_active_level
        set_debugfs_number compaction_threshold "${threshold}"
        [[ "$(debugfs_number compaction_threshold)" -eq "${threshold}" ]] ||
            fail "compaction_threshold did not apply: ${threshold}"

        alloc_key_range "${write_count}" 16
        first_key="${ALLOCATED_FIRST_KEY}"
        pattern="${TMPDIR}/threshold-${threshold}.bin"
        make_pattern_range "$((2000 + threshold))" "${write_count}" "${pattern}"

        before_insert="$(stat_number lsm_record_insert_count)"
        before_compaction="$(stat_number compaction_count)"
        before_segment_count="$(segment_count)"
        write_4k_blocks "${first_key}" "${write_count}" "$((2000 + threshold))"
        sync

        assert_counter_delta_eq lsm_record_insert_count "${before_insert}" \
            "${write_count}" "threshold=${threshold} insert accounting"
        assert_counter_delta_ge compaction_count "${before_compaction}" 1 \
            "threshold=${threshold} auto compaction"
        after_segment_count="$(segment_count)"
        [[ "${after_segment_count}" -gt "${before_segment_count}" ]] ||
            fail "threshold=${threshold}: segment_count did not increase"
        hits="$(block_table_entries_in_range "${first_key}" "${write_count}")"
        [[ "${hits}" -gt 0 ]] ||
            fail "threshold=${threshold}: no block_table entries in range"

        make_pattern_range "$((2000 + threshold))" "${write_count}" "${pattern}"
        verify_payload_range "${first_key}" "${write_count}" "${pattern}" \
            "threshold=${threshold} readback"
    done
    set_debugfs_number compaction_threshold 0
}

test_bloom_bits_per_key()
{
    local bits_per_key

    if ! have_debugfs_file bloom_bits_per_key; then
        log "SKIP: bloom_bits_per_key debugfs knob not present"
        return
    fi
    if ! have_debugfs_file compaction_threshold; then
        log "SKIP: bloom sweep needs compaction_threshold debugfs knob"
        return
    fi

    log "sweep bloom_bits_per_key: ${BLOOM_BITS_PER_KEY_VALUES}"
    set_debugfs_number compaction_threshold 40
    for bits_per_key in ${BLOOM_BITS_PER_KEY_VALUES}; do
        local write_count=40
        local first_key
        local pattern
        local before_compaction
        local before_next_segment_id
        local blooms
        local new_segment_count=0
        local actual_id
        local actual_keys
        local actual_bits
        local actual_hashes
        local expected_bits

        compact_active_level
        set_debugfs_number bloom_bits_per_key "${bits_per_key}"
        [[ "$(debugfs_number bloom_bits_per_key)" -eq "${bits_per_key}" ]] ||
            fail "bloom_bits_per_key did not apply: ${bits_per_key}"

        alloc_key_range "${write_count}" 16
        first_key="${ALLOCATED_FIRST_KEY}"
        pattern="${TMPDIR}/bloom-${bits_per_key}.bin"
        make_pattern_range "$((3000 + bits_per_key))" "${write_count}" \
            "${pattern}"

        before_compaction="$(stat_number compaction_count)"
        before_next_segment_id="$(next_segment_id)"
        write_4k_blocks "${first_key}" "${write_count}" \
            "$((3000 + bits_per_key))"
        sync

        assert_counter_delta_ge compaction_count "${before_compaction}" 1 \
            "bloom_bits_per_key=${bits_per_key} segment build"
        blooms="$(new_segment_blooms_after_id "${before_next_segment_id}")" ||
            fail "bloom_bits_per_key=${bits_per_key}: no new segment from id ${before_next_segment_id}"
        while read -r actual_id actual_keys actual_bits actual_hashes; do
            [[ -n "${actual_id}" ]] || continue
            new_segment_count=$((new_segment_count + 1))
            expected_bits="$(expected_bloom_bits "${actual_keys}" "${bits_per_key}")"
            [[ "${actual_bits}" -eq "${expected_bits}" ]] ||
                fail "bloom_bits_per_key=${bits_per_key}: segment=${actual_id} bloom_bits expected ${expected_bits}, got ${actual_bits}"
            [[ "${actual_hashes}" -ge 3 && "${actual_hashes}" -le 7 ]] ||
                fail "bloom_bits_per_key=${bits_per_key}: segment=${actual_id} bloom_hashes=${actual_hashes}"
        done <<< "${blooms}"
        [[ "${new_segment_count}" -gt 0 ]] ||
            fail "bloom_bits_per_key=${bits_per_key}: no new segment rows parsed"

        verify_payload_range "${first_key}" "${write_count}" "${pattern}" \
            "bloom_bits_per_key=${bits_per_key} readback"
        log "PASS: bloom_bits_per_key=${bits_per_key} new_segments=${new_segment_count}"
    done
    set_debugfs_number bloom_bits_per_key 0
    set_debugfs_number compaction_threshold 0
}

run_sweep_for_device()
{
    local device="$1"
    local zone_count

    require_device "${device}"
    require_debugfs
    zone_count="$(device_zone_count "${device}")"
    [[ "${zone_count}" -gt 0 ]] ||
        fail "${device} has no complete IMR zones"

    ACTIVE_DEVICE="${device}"
    if [[ "${ZONE}" -lt "${zone_count}" ]]; then
        ACTIVE_ZONE="${ZONE}"
    else
        ACTIVE_ZONE=$((zone_count - 1))
    fi
    NEXT_OFFSET="${KEY_OFFSET}"
    TMPDIR="$(mktemp -d "${TMP_ROOT}/case.XXXXXX")"

    log "device=${ACTIVE_DEVICE} zones=${zone_count} test_zone=${ACTIVE_ZONE} key_offset=${KEY_OFFSET}"
    test_write_sizes
    test_read_tree_limits
    test_compaction_thresholds
    test_bloom_bits_per_key

    rm -rf "${TMPDIR}"
    TMPDIR=""
    log "PASS: parameter sweep completed for ${ACTIVE_DEVICE}"
}

run_temp_device_sweep()
{
    local zones="$1"
    local backing="${TMP_ROOT}/imrsim-${zones}.img"
    local mapper="${MAPPER_PREFIX}_${zones}_$$"
    local loop
    local sectors

    [[ "${zones}" -gt 0 ]] || fail "invalid zone count: ${zones}"
    truncate -s $((zones * ZONE_BYTES + PSTORE_BYTES)) "${backing}"
    loop="$(losetup --find --show "${backing}")"
    TEMP_LOOPS+=("${loop}")

    sectors="$("${REPO_ROOT}/imrsim_util/imr_format.sh" -d "${loop}")"
    dmsetup create "${mapper}" --table "0 ${sectors} imrsim ${loop} 0" ||
        fail "cannot create dm target ${mapper}; remove any active imrsim target and retry"
    TEMP_MAPPERS+=("${mapper}")

    run_sweep_for_device "/dev/mapper/${mapper}"

    dmsetup remove "${mapper}"
    TEMP_MAPPERS=("${TEMP_MAPPERS[@]:0:${#TEMP_MAPPERS[@]}-1}")
    losetup -d "${loop}"
    TEMP_LOOPS=("${TEMP_LOOPS[@]:0:${#TEMP_LOOPS[@]}-1}")
}

main()
{
    require_root
    require_tools
    trap cleanup EXIT
    TMP_ROOT="$(mktemp -d)"

    if [[ -n "${DEVICE_ZONE_COUNTS}" ]]; then
        require_device_sweep_tools
        log "device zone-count sweep: ${DEVICE_ZONE_COUNTS}"
        for zones in ${DEVICE_ZONE_COUNTS}; do
            run_temp_device_sweep "${zones}"
        done
    else
        run_sweep_for_device "${DEVICE}"
    fi

    log "PASS: all parameter sweep cases completed"
}

main "$@"
