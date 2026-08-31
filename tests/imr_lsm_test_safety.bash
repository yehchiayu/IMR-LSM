# Shared preflight for destructive IMR-LSM runtime tests.
# The caller is expected to run with `set -euo pipefail`.

IMR_LSM_TEST_SAFETY_LOCK_HELD="${IMR_LSM_TEST_SAFETY_LOCK_HELD:-0}"
IMR_LSM_TEST_VALIDATED_BACKING_DEVICE=""

imr_lsm_test_safety_die()
{
    printf '[imr-lsm safety] FAIL: %s\n' "$*" >&2
    exit 1
}

imr_lsm_test_require_safety_tools()
{
    local tool

    for tool in awk dirname dmsetup findmnt flock lsblk readlink sleep tr; do
        command -v "${tool}" >/dev/null 2>&1 ||
            imr_lsm_test_safety_die "missing required safety tool: ${tool}"
    done
}

imr_lsm_test_wait_level_compaction_idle()
{
    local debugfs="${1:-/sys/kernel/debug/imrsim_lsm}"
    local attempts="${2:-600}"
    local interval="${3:-0.1}"
    local values
    local pending
    local running
    local error_count
    local last_error
    local work_run_count
    local work_time_count
    local attempt

    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_LEVEL_COMPACTION_WAIT_ATTEMPTS "${attempts}"
    [[ "${attempts}" -gt 0 ]] ||
        imr_lsm_test_safety_die \
            "level compaction wait attempts must be > 0"
    [[ -r "${debugfs}/stats" ]] ||
        imr_lsm_test_safety_die "missing readable ${debugfs}/stats"

    for ((attempt = 0; attempt < attempts; attempt++)); do
        values="$(
            awk -F': ' '
                $1 == "level_compaction_pending" { pending = $2 }
                $1 == "level_compaction_running" { running = $2 }
                $1 == "level_compaction_work_error_count" {
                    error_count = $2
                }
                $1 == "last_level_compaction_work_error" {
                    last_error = $2
                }
                $1 == "level_compaction_work_run_count" {
                    work_run_count = $2
                }
                $1 == "level_compaction_work_time_count" {
                    work_time_count = $2
                }
                END {
                    if(pending == "" || running == "" ||
                       error_count == "" || last_error == "" ||
                       work_run_count == "" || work_time_count == "")
                        exit 1
                    print pending, running, error_count, last_error, work_run_count, work_time_count
                }
            ' "${debugfs}/stats"
        )" || imr_lsm_test_safety_die \
            "background level compaction stats are unavailable"
        read -r pending running error_count last_error work_run_count \
            work_time_count <<< "${values}"
        [[ "${error_count}" -eq 0 && "${last_error}" -eq 0 ]] ||
            imr_lsm_test_safety_die \
                "background level compaction failed: count=${error_count} last=${last_error}"
        # running is cleared before the worker publishes its final hold/work
        # diagnostics.  The count relation closes that short visibility gap.
        if [[ "${pending}" -eq 0 && "${running}" -eq 0 &&
              "${work_time_count}" -ge "${work_run_count}" ]]; then
            return 0
        fi
        sleep "${interval}"
    done

    imr_lsm_test_safety_die \
        "background level compaction did not become idle after ${attempts} checks"
}

imr_lsm_test_wait_zone_compaction_idle()
{
    local debugfs="${1:-/sys/kernel/debug/imrsim_lsm}"
    local attempts="${2:-1800}"
    local interval="${3:-0.1}"
    local values
    local pending
    local running
    local auto_failed
    local zone_failed
    local last_error
    local attempt

    imr_lsm_test_require_nonnegative_integer \
        IMR_LSM_ZONE_COMPACTION_WAIT_ATTEMPTS "${attempts}"
    [[ "${attempts}" -gt 0 ]] ||
        imr_lsm_test_safety_die \
            "zone compaction wait attempts must be > 0"
    [[ -r "${debugfs}/stats" ]] ||
        imr_lsm_test_safety_die "missing readable ${debugfs}/stats"

    for ((attempt = 0; attempt < attempts; attempt++)); do
        values="$(
            awk -F': ' '
                $1 == "zone_compaction_auto_pending" { pending = $2 }
                $1 == "zone_compaction_auto_running" { running = $2 }
                $1 == "zone_compaction_auto_run_failed_count" {
                    auto_failed = $2
                }
                $1 == "zone_compaction_failed_count" {
                    zone_failed = $2
                }
                $1 == "last_zone_compaction_auto_run_error" {
                    last_error = $2
                }
                END {
                    if(pending == "" || running == "" ||
                       auto_failed == "" || zone_failed == "" ||
                       last_error == "")
                        exit 1
                    print pending, running, auto_failed, zone_failed, last_error
                }
            ' "${debugfs}/stats"
        )" || imr_lsm_test_safety_die \
            "background zone compaction stats are unavailable"
        read -r pending running auto_failed zone_failed last_error \
            <<< "${values}"
        [[ "${auto_failed}" -eq 0 && "${zone_failed}" -eq 0 &&
           "${last_error}" -eq 0 ]] ||
            imr_lsm_test_safety_die \
                "background zone compaction failed: auto=${auto_failed} zone=${zone_failed} last=${last_error}"
        if [[ "${pending}" -eq 0 && "${running}" -eq 0 ]]; then
            return 0
        fi
        sleep "${interval}"
    done

    imr_lsm_test_safety_die \
        "background zone compaction did not become idle after ${attempts} checks"
}

imr_lsm_test_require_nonnegative_integer()
{
    local label="$1"
    local value="$2"

    [[ "${value}" =~ ^(0|[1-9][0-9]*)$ && ${#value} -le 18 ]] ||
        imr_lsm_test_safety_die "${label} must be a non-negative integer: ${value}"
}

imr_lsm_test_acquire_lock()
{
    local lock_file="/run/lock/imr-lsm-tests.lock"
    local fd_target
    local lock_target

    if [[ "${IMR_LSM_TEST_SAFETY_LOCK_HELD}" -eq 1 ]]; then
        fd_target="$(readlink -f "/proc/${BASHPID}/fd/9" 2>/dev/null)" ||
            imr_lsm_test_safety_die \
                "inherited safety-lock fd 9 is unavailable"
        lock_target="$(readlink -f "${lock_file}" 2>/dev/null)" ||
            imr_lsm_test_safety_die \
                "cannot resolve inherited safety lock: ${lock_file}"
        [[ "${fd_target}" == "${lock_target}" ]] ||
            imr_lsm_test_safety_die \
                "inherited fd 9 does not reference ${lock_file}"
        flock -n 9 ||
            imr_lsm_test_safety_die \
                "inherited IMR-LSM safety lock is not held"
        export IMR_LSM_TEST_SAFETY_LOCK_HELD=1
        return
    fi
    [[ -d "$(dirname "${lock_file}")" ]] ||
        imr_lsm_test_safety_die "lock directory does not exist: $(dirname "${lock_file}")"
    if ! exec 9>"${lock_file}"; then
        imr_lsm_test_safety_die "cannot open safety lock: ${lock_file}"
    fi
    flock -n 9 ||
        imr_lsm_test_safety_die "another IMR-LSM destructive test is running"
    export IMR_LSM_TEST_SAFETY_LOCK_HELD=1
}

imr_lsm_test_validate_target()
{
    local device="$1"
    local table
    local row_count
    local target_type
    local backing_device

    [[ -b "${device}" ]] ||
        imr_lsm_test_safety_die "${device} is not a block device"
    table="$(dmsetup table "${device}" 2>/dev/null)" ||
        imr_lsm_test_safety_die "cannot read device-mapper table for ${device}"
    row_count="$(awk 'NF { rows++ } END { print rows + 0 }' <<< "${table}")"
    target_type="$(awk 'NF { print $3; exit }' <<< "${table}")"
    [[ "${row_count}" -eq 1 && "${target_type}" == "imrsim" ]] ||
        imr_lsm_test_safety_die \
            "${device} must contain exactly one imrsim target (rows=${row_count}, target=${target_type:-none})"

    backing_device="$(awk 'NF { print $4; exit }' <<< "${table}")"
    case "${backing_device}" in
        /*)
            ;;
        [0-9]*:[0-9]*)
            backing_device="/dev/block/${backing_device}"
            ;;
        ?*)
            backing_device="/dev/${backing_device}"
            ;;
        *)
            imr_lsm_test_safety_die \
                "cannot resolve backing device from dm table for ${device}"
            ;;
    esac
    backing_device="$(readlink -f "${backing_device}" 2>/dev/null)" ||
        imr_lsm_test_safety_die \
            "cannot canonicalize backing device from dm table for ${device}"
    [[ -b "${backing_device}" ]] ||
        imr_lsm_test_safety_die \
            "dm table backing path is not a block device: ${backing_device}"
    IMR_LSM_TEST_VALIDATED_BACKING_DEVICE="${backing_device}"
}

imr_lsm_test_require_unused()
{
    local device="$1"
    local protected_major_minors
    local backing_tree_major_minors
    local protected_major_minor
    local mounted_major_minors
    local swap_path
    local swap_major_minor

    protected_major_minors="$(lsblk -s -nrpo MAJ:MIN "${device}" 2>/dev/null)" ||
        imr_lsm_test_safety_die \
            "cannot resolve ${device} and its backing devices"
    [[ -n "${IMR_LSM_TEST_VALIDATED_BACKING_DEVICE}" ]] ||
        imr_lsm_test_safety_die \
            "target backing device was not validated for ${device}"
    backing_tree_major_minors="$(
        lsblk -nrpo MAJ:MIN "${IMR_LSM_TEST_VALIDATED_BACKING_DEVICE}" \
            2>/dev/null
    )" ||
        imr_lsm_test_safety_die \
            "cannot inspect backing tree ${IMR_LSM_TEST_VALIDATED_BACKING_DEVICE}"
    protected_major_minors="$(
        printf '%s\n%s\n' \
            "${protected_major_minors}" "${backing_tree_major_minors}" |
            awk 'NF && !seen[$1]++ { print $1 }'
    )"
    [[ -n "${protected_major_minors}" ]] ||
        imr_lsm_test_safety_die "no device numbers found for ${device}"

    mounted_major_minors="$(findmnt -rn -o MAJ:MIN)" ||
        imr_lsm_test_safety_die "cannot inspect mounted devices"
    while IFS= read -r protected_major_minor; do
        [[ "${protected_major_minor}" =~ ^[0-9]+:[0-9]+$ ]] ||
            imr_lsm_test_safety_die \
                "invalid device number below ${device}: ${protected_major_minor:-none}"
        if awk -v device="${protected_major_minor}" '
                $1 == device { mounted = 1 }
                END { exit mounted ? 0 : 1 }
            ' <<< "${mounted_major_minors}"; then
            imr_lsm_test_safety_die \
                "${device} or a backing block device is mounted (${protected_major_minor})"
        fi
    done <<< "${protected_major_minors}"

    while read -r swap_path _; do
        [[ "${swap_path}" == "Filename" ]] && continue
        swap_major_minor="$(lsblk -dnro MAJ:MIN "${swap_path}" 2>/dev/null |
            tr -d '[:space:]' || true)"
        [[ -n "${swap_major_minor}" ]] || continue
        if awk -v swap="${swap_major_minor}" '
                $1 == swap { active = 1 }
                END { exit active ? 0 : 1 }
            ' <<< "${protected_major_minors}"; then
            imr_lsm_test_safety_die \
                "${device} or a backing block device is active swap (${swap_path})"
        fi
    done < /proc/swaps
}

imr_lsm_test_safety_begin()
{
    local device="$1"
    local owned="${2:-0}"

    imr_lsm_test_require_safety_tools
    case "${owned}" in
        0)
            [[ "${IMR_LSM_TEST_DESTRUCTIVE:-0}" == "1" ]] ||
                imr_lsm_test_safety_die \
                    "refusing destructive I/O to ${device}; set IMR_LSM_TEST_DESTRUCTIVE=1"
            ;;
        1)
            ;;
        *)
            imr_lsm_test_safety_die "invalid owned-device flag: ${owned}"
            ;;
    esac

    imr_lsm_test_acquire_lock
    imr_lsm_test_validate_target "${device}"
    imr_lsm_test_require_unused "${device}"
}
