#!/usr/bin/env bash
set -euo pipefail

readonly ZONE_BYTES=$((256 * 1024 * 1024))
readonly SECTOR_BYTES=512
readonly IO_BLOCK_BYTES=4096
readonly MIN_PERSISTENCE_BYTES=$((2 * 1024 * 1024))
readonly STATE_FIXED_BYTES=64
readonly ZONE_STATS_BYTES=20
readonly ZONE_STATUS_BYTES=553504
readonly ZONE_STATUS_ALIGNMENT=8
readonly STATE_TRAILER_BYTES=4
readonly MAX_PERSISTENT_ZONES=7759

show_zones=0
initialize_persistence=0
imr_device=""
persistence_query_zones=""
page_bytes=0
long_bits=0

usage()
{
    cat <<EOF
Usage:
  $0 [-z] [-i] -d device|partition|loop
  $0 -p zones

  -d DEVICE  Inspect DEVICE and print usable capacity in 512-byte sectors.
  -z         Print usable capacity in 256 MiB zones instead. This is read-only.
  -i         Zero the dynamically sized persistence range before printing sectors.
             This requires root and IMR_LSM_TEST_DESTRUCTIVE=1.
  -p ZONES   Print the required persistence reserve in bytes for ZONES.
             This calculation is read-only and does not accept -d, -z, or -i.

Without -i, this command is read-only. -z and -i cannot be combined.
EOF
}

fail()
{
    printf 'imr_format.sh: %s\n' "$*" >&2
    exit 1
}

require_command()
{
    command -v "$1" >/dev/null 2>&1 ||
        fail "missing required command: $1"
}

persistence_bytes_for_zones()
{
    local zones="$1"
    local status_offset
    local state_bytes
    local reserve_bytes

    [[ "${zones}" =~ ^[1-9][0-9]*$ ]] ||
        fail "zone count must be a positive integer: ${zones}"
    [[ "${zones}" -le "${MAX_PERSISTENT_ZONES}" ]] ||
        fail "zone count ${zones} exceeds persistent format maximum ${MAX_PERSISTENT_ZONES}"

    status_offset=$((STATE_FIXED_BYTES + ZONE_STATS_BYTES * zones))
    status_offset=$((
        (status_offset + ZONE_STATUS_ALIGNMENT - 1) /
        ZONE_STATUS_ALIGNMENT * ZONE_STATUS_ALIGNMENT
    ))
    state_bytes=$((
        status_offset + ZONE_STATUS_BYTES * zones + STATE_TRAILER_BYTES
    ))
    reserve_bytes=$((
        (state_bytes + page_bytes - 1) /
        page_bytes * page_bytes
    ))
    if [[ "${reserve_bytes}" -lt "${MIN_PERSISTENCE_BYTES}" ]]; then
        reserve_bytes="${MIN_PERSISTENCE_BYTES}"
    fi
    printf '%s\n' "${reserve_bytes}"
}

while getopts ":zip:d:h" opt; do
    case "${opt}" in
        d)
            imr_device="${OPTARG}"
            ;;
        z)
            show_zones=1
            ;;
        i)
            initialize_persistence=1
            ;;
        p)
            persistence_query_zones="${OPTARG}"
            ;;
        h)
            usage
            exit 0
            ;;
        :)
            fail "option -${OPTARG} requires an argument"
            ;;
        \?)
            fail "invalid option: -${OPTARG}"
            ;;
    esac
done
shift "$((OPTIND - 1))"

[[ "$#" -eq 0 ]] || fail "unexpected argument: $1"
require_command getconf
long_bits="$(getconf LONG_BIT)" ||
    fail "cannot determine userspace word size"
[[ "${long_bits}" == "64" ]] ||
    fail "unsupported word size: ${long_bits}; persistence layout requires 64-bit Linux"
page_bytes="$(getconf PAGE_SIZE)" ||
    fail "cannot determine kernel page size"
[[ "${page_bytes}" =~ ^[1-9][0-9]*$ &&
   "${page_bytes}" -ge "${IO_BLOCK_BYTES}" &&
   $((page_bytes % IO_BLOCK_BYTES)) -eq 0 &&
   $((page_bytes & (page_bytes - 1))) -eq 0 ]] ||
    fail "unsupported page size: ${page_bytes}"
if [[ -n "${persistence_query_zones}" ]]; then
    [[ -z "${imr_device}" && "${show_zones}" -eq 0 &&
       "${initialize_persistence}" -eq 0 ]] ||
        fail "-p cannot be combined with -d, -z, or -i"
    persistence_bytes_for_zones "${persistence_query_zones}"
    exit 0
fi
[[ -n "${imr_device}" ]] || {
    usage >&2
    exit 1
}
[[ "${show_zones}" -eq 0 || "${initialize_persistence}" -eq 0 ]] ||
    fail "-z is read-only and cannot be combined with -i"
[[ -b "${imr_device}" ]] ||
    fail "not a block device: ${imr_device}"

require_command blockdev

device_size_bytes="$(blockdev --getsize64 "${imr_device}")" ||
    fail "cannot read device size: ${imr_device}"
[[ "${device_size_bytes}" =~ ^[0-9]+$ ]] ||
    fail "invalid device size: ${device_size_bytes}"
[[ "${device_size_bytes}" -ge $((ZONE_BYTES + MIN_PERSISTENCE_BYTES)) ]] ||
    fail "device must contain at least one 256 MiB zone plus persistence"

zones=$((device_size_bytes / ZONE_BYTES))
if [[ "${zones}" -gt "${MAX_PERSISTENT_ZONES}" ]]; then
    zones="${MAX_PERSISTENT_ZONES}"
fi
while [[ "${zones}" -gt 0 ]]; do
    persistence_bytes="$(persistence_bytes_for_zones "${zones}")"
    if [[ $((zones * ZONE_BYTES + persistence_bytes)) -le "${device_size_bytes}" ]]; then
        break
    fi
    zones=$((zones - 1))
done
[[ "${zones}" -gt 0 ]] ||
    fail "device has no complete zone with enough persistence capacity"

usable_bytes=$((zones * ZONE_BYTES))
usable_sectors=$((usable_bytes / SECTOR_BYTES))
persistence_start_bytes=${usable_bytes}
persistence_end_bytes=$((persistence_start_bytes + persistence_bytes))

[[ "${persistence_end_bytes}" -ge "${persistence_start_bytes}" ]] ||
    fail "persistence byte range overflow"
[[ "${persistence_end_bytes}" -le "${device_size_bytes}" ]] ||
    fail "persistence range [${persistence_start_bytes}, ${persistence_end_bytes}) exceeds device size ${device_size_bytes}"
[[ $((persistence_start_bytes % IO_BLOCK_BYTES)) -eq 0 ]] ||
    fail "persistence start is not 4 KiB aligned: ${persistence_start_bytes}"

if [[ "${show_zones}" -eq 1 ]]; then
    printf '%s\n' "${zones}"
    exit 0
fi

if [[ "${initialize_persistence}" -eq 1 ]]; then
    [[ "${IMR_LSM_TEST_DESTRUCTIVE:-0}" == "1" ]] ||
        fail "refusing to initialize; set IMR_LSM_TEST_DESTRUCTIVE=1"
    [[ "${EUID}" -eq 0 ]] ||
        fail "initialization requires root (for example, sudo -E $0 -i -d ${imr_device})"

    for command_name in dd lsblk readlink; do
        require_command "${command_name}"
    done

    [[ "$(blockdev --getro "${imr_device}")" == "0" ]] ||
        fail "device is read-only: ${imr_device}"

    mount_output="$(lsblk -nrpo MOUNTPOINT "${imr_device}")" ||
        fail "cannot inspect mount state: ${imr_device}"
    [[ ! "${mount_output}" =~ [^[:space:]] ]] ||
        fail "device or a child device is mounted; refusing to initialize"

    descendant_devices="$(lsblk -nrpo NAME "${imr_device}")" ||
        fail "cannot inspect child devices: ${imr_device}"
    while read -r swap_device _; do
        [[ -n "${swap_device}" && "${swap_device}" != "Filename" ]] || continue
        swap_real="$(readlink -f "${swap_device}" 2>/dev/null || true)"

        while IFS= read -r descendant_device; do
            [[ -n "${descendant_device}" ]] || continue
            descendant_real="$(
                readlink -f "${descendant_device}" 2>/dev/null || true
            )"
            if [[ -n "${swap_real}" &&
                  "${swap_real}" == "${descendant_real}" ]]; then
                fail "device or a child device is active swap: ${swap_device}"
            fi
        done <<< "${descendant_devices}"
    done < /proc/swaps

    persistence_seek_blocks=$((persistence_start_bytes / IO_BLOCK_BYTES))
    persistence_block_count=$((persistence_bytes / IO_BLOCK_BYTES))

    dd if=/dev/zero of="${imr_device}" \
        bs="${IO_BLOCK_BYTES}" \
        seek="${persistence_seek_blocks}" \
        count="${persistence_block_count}" \
        conv=notrunc,fsync >/dev/null 2>&1
fi

printf '%s\n' "${usable_sectors}"
