#!/usr/bin/env bash
set -euo pipefail

readonly ZONE_BYTES=$((256 * 1024 * 1024))

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
FORMAT_SCRIPT="${SCRIPT_DIR}/imrsim_util/imr_format.sh"
ZONE_COUNT="${IMR_LSM_BUILD_ZONES:-4}"
MAPPER_NAME="${IMR_LSM_BUILD_MAPPER:-imrsim}"

WORK_DIR=""
BACKING_FILE=""
LOOP_DEVICE=""
MAPPER_OWNED=0
COMPLETED=0

usage()
{
    cat <<EOF
Usage: $0 [loop] [--zones COUNT] [--mapper NAME]

Build and install dm-imrsim, then create a mapper backed by a new temporary
sparse file and a loop device owned by this script.

  loop             Accepted as a legacy no-op; temporary loop mode is default.
  --zones COUNT    Number of 256 MiB zones to create (default: ${ZONE_COUNT}).
  --mapper NAME    Device-mapper name (default: ${MAPPER_NAME}).
  -h, --help       Show this help.

This script intentionally does not accept an existing block device.
EOF
}

fail()
{
    printf 'build.sh: %s\n' "$*" >&2
    exit 1
}

require_command()
{
    command -v "$1" >/dev/null 2>&1 ||
        fail "missing required command: $1"
}

as_root()
{
    if [[ "${EUID}" -eq 0 ]]; then
        "$@"
    else
        sudo "$@"
    fi
}

cleanup_owned_resources()
{
    local cleanup_failed=0
    local backing_can_be_removed=1

    if [[ "${MAPPER_OWNED}" -eq 1 ]]; then
        if ! as_root dmsetup remove "${MAPPER_NAME}" >/dev/null 2>&1; then
            cleanup_failed=1
            backing_can_be_removed=0
        else
            MAPPER_OWNED=0
        fi
    fi

    if [[ "${backing_can_be_removed}" -eq 1 &&
          -n "${LOOP_DEVICE}" ]]; then
        if ! as_root losetup -d "${LOOP_DEVICE}" >/dev/null 2>&1; then
            cleanup_failed=1
            backing_can_be_removed=0
        else
            LOOP_DEVICE=""
        fi
    fi

    if [[ "${backing_can_be_removed}" -eq 1 &&
          -n "${BACKING_FILE}" && -e "${BACKING_FILE}" ]]; then
        rm -f -- "${BACKING_FILE}" || cleanup_failed=1
        BACKING_FILE=""
    fi

    if [[ "${backing_can_be_removed}" -eq 1 &&
          -n "${WORK_DIR}" && -d "${WORK_DIR}" ]]; then
        rmdir -- "${WORK_DIR}" >/dev/null 2>&1 || cleanup_failed=1
        WORK_DIR=""
    fi

    return "${cleanup_failed}"
}

cleanup_on_exit()
{
    local status=$?

    if [[ "${COMPLETED}" -eq 0 ]]; then
        set +e
        if ! cleanup_owned_resources; then
            printf 'build.sh: cleanup incomplete; retained mapper=%s loop=%s backing=%s\n' \
                "${MAPPER_NAME}" "${LOOP_DEVICE:-none}" \
                "${BACKING_FILE:-none}" >&2
        fi
    fi

    exit "${status}"
}

trap cleanup_on_exit EXIT
trap 'exit 130' HUP INT TERM

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        loop)
            shift
            ;;
        --zones)
            [[ "$#" -ge 2 ]] || fail "--zones requires a value"
            ZONE_COUNT="$2"
            shift 2
            ;;
        --zones=*)
            ZONE_COUNT="${1#*=}"
            shift
            ;;
        --mapper)
            [[ "$#" -ge 2 ]] || fail "--mapper requires a value"
            MAPPER_NAME="$2"
            shift 2
            ;;
        --mapper=*)
            MAPPER_NAME="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

[[ "${ZONE_COUNT}" =~ ^[1-9][0-9]*$ ]] ||
    fail "zone count must be a positive integer: ${ZONE_COUNT}"
[[ "${MAPPER_NAME}" =~ ^[A-Za-z0-9+_.-]+$ ]] ||
    fail "invalid mapper name: ${MAPPER_NAME}"
[[ -f "${FORMAT_SCRIPT}" ]] ||
    fail "missing formatter: ${FORMAT_SCRIPT}"

for command_name in bash env make truncate losetup dmsetup depmod modprobe mktemp; do
    require_command "${command_name}"
done
if [[ "${EUID}" -ne 0 ]]; then
    require_command sudo
fi

cd -- "${SCRIPT_DIR}"

make
as_root make install
as_root depmod --quick
as_root modprobe dm-imrsim

if as_root dmsetup info "${MAPPER_NAME}" >/dev/null 2>&1; then
    fail "refusing to replace existing mapper: ${MAPPER_NAME}"
fi

TEMP_BASE="${TMPDIR:-/tmp}"
[[ -d "${TEMP_BASE}" && -w "${TEMP_BASE}" ]] ||
    fail "temporary directory is not writable: ${TEMP_BASE}"

WORK_DIR="$(mktemp -d "${TEMP_BASE%/}/imrsim-build.XXXXXX")"
BACKING_FILE="${WORK_DIR}/imrsim.img"
PERSISTENCE_BYTES="$(bash "${FORMAT_SCRIPT}" -p "${ZONE_COUNT}")"
[[ "${PERSISTENCE_BYTES}" =~ ^[1-9][0-9]*$ ]] ||
    fail "formatter returned invalid persistence size: ${PERSISTENCE_BYTES}"
truncate -s "$((ZONE_COUNT * ZONE_BYTES + PERSISTENCE_BYTES))" \
    "${BACKING_FILE}"

LOOP_DEVICE="$(as_root losetup --find --show "${BACKING_FILE}")"
[[ -b "${LOOP_DEVICE}" ]] ||
    fail "losetup did not return a block device: ${LOOP_DEVICE}"

USABLE_SECTORS="$(
    as_root env IMR_LSM_TEST_DESTRUCTIVE=1 \
        bash "${FORMAT_SCRIPT}" -i -d "${LOOP_DEVICE}"
)"
[[ "${USABLE_SECTORS}" =~ ^[1-9][0-9]*$ ]] ||
    fail "formatter returned invalid sector count: ${USABLE_SECTORS}"

as_root dmsetup create "${MAPPER_NAME}" \
    --table "0 ${USABLE_SECTORS} imrsim ${LOOP_DEVICE} 0"
MAPPER_OWNED=1
COMPLETED=1

printf 'Created /dev/mapper/%s\n' "${MAPPER_NAME}"
printf '  backing file: %s\n' "${BACKING_FILE}"
printf '  loop device:  %s\n' "${LOOP_DEVICE}"
printf '  zones:        %s\n' "${ZONE_COUNT}"
printf '  persistence:  %s bytes\n' "${PERSISTENCE_BYTES}"
printf '\nCleanup when finished:\n'
printf '  sudo dmsetup remove %q\n' "${MAPPER_NAME}"
printf '  sudo losetup -d %q\n' "${LOOP_DEVICE}"
printf '  rm -f %q && rmdir %q\n' "${BACKING_FILE}" "${WORK_DIR}"
