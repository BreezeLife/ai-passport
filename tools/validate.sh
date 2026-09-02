#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    python3 tests/run_host_tests.py
    PYTHONWARNINGS=error::ResourceWarning \
        python3 -m unittest discover -s tools/workbuddy_gateway/tests -v
    python3 tests/test_verify_firmware.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir
    local live_build_dir

    require_config() {
        local config_file="$1"
        local expected_line="$2"
        local profile="$3"
        if ! grep -Fqx -- "${expected_line}" "${config_file}"; then
            echo "ERROR: ${profile} profile missing generated setting: ${expected_line}" >&2
            return 1
        fi
    }

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    live_build_dir="$(mktemp -d /tmp/ai-passport-firmware-live.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac; case "${live_build_dir}" in /tmp/ai-passport-firmware-live.*) rm -rf -- "${live_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    require_config "${validation_build_dir}/sdkconfig" \
        "CONFIG_WB_DEMO_MODE=y" "demo"
    require_config "${validation_build_dir}/sdkconfig" \
        "# CONFIG_BT_ENABLED is not set" "demo"
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults;${repo_root}/tests/sdkconfig.live.defaults" \
        idf.py -B "${live_build_dir}" \
        -D "SDKCONFIG=${live_build_dir}/sdkconfig" build
    require_config "${live_build_dir}/sdkconfig" \
        "# CONFIG_WB_DEMO_MODE is not set" "live"
    require_config "${live_build_dir}/sdkconfig" \
        "# CONFIG_WB_ALLOW_INSECURE_HTTP is not set" "live"
    require_config "${live_build_dir}/sdkconfig" \
        "# CONFIG_BT_ENABLED is not set" "live"
    require_config "${live_build_dir}/sdkconfig" \
        "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y" "live"
    require_config "${live_build_dir}/sdkconfig" \
        "CONFIG_MBEDTLS_HAVE_TIME_DATE=y" "live"
    idf.py -B "${live_build_dir}" merge-bin \
        -o "${live_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${live_build_dir}"
    echo "Firmware builds (demo + live compile profile): PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
