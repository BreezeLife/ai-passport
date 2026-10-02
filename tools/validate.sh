#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

# Public source distributions omit restricted development narration. A complete
# local bank still receives every source/hash/decoder check; partial resources
# must fail rather than silently taking the omitted-audio profile.
run_dino_audio_checks() {
    local audio_dir="assets/audio/dinobook40"
    local audio_source="${audio_dir}/audio.bin"
    if [[ -e "${audio_source}" || -L "${audio_source}" ]]; then
        PYTHONDONTWRITEBYTECODE=1 python3 tools/generate_dino_audio.py --verify
        echo "Audio asset checks: PASS"
    elif compgen -G "${audio_dir}/*.wav" >/dev/null || \
         compgen -G "${audio_dir}/*.adpcm" >/dev/null; then
        echo "ERROR: Incomplete DinoBook narration resources: audio.bin is missing; restore the complete compatible bank or omit all narration binaries." >&2
        return 1
    else
        echo "Audio asset checks: NOT RUN (public source profile; narration binaries omitted)"
    fi
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local gc_sections_flag="-Wl,--gc-sections"
    local asset_python="${DINO_ASSET_PYTHON:-python3}"
    if ! "${asset_python}" -c 'import PIL' >/dev/null 2>&1; then
        echo "ERROR: Dino animation checks need Pillow. Install tools/requirements-dino-assets.txt in a virtual environment and set DINO_ASSET_PYTHON to its Python." >&2
        return 1
    fi
    if [[ "$(uname -s)" == "Darwin" ]]; then
        gc_sections_flag="-Wl,-dead_strip"
    fi

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
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${gc_sections_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/run_dino_tests.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/run_dino_animation_tests.py
    PYTHONDONTWRITEBYTECODE=1 "${asset_python}" tests/test_generate_dino_animation.py
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_dino_adpcm.c main/dino_adpcm.c \
        -o "${test_dir}/test_dino_adpcm"
    "${test_dir}/test_dino_adpcm"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
        -Itests/dino_stubs -Imain tests/test_dino_runtime.c \
        main/dino_model.c main/dino_catalog.c main/dino_adpcm.c \
        -o "${test_dir}/test_dino_runtime"
    "${test_dir}/test_dino_runtime"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/generate_dino_fonts.py --check
    PYTHONDONTWRITEBYTECODE=1 python3 tools/generate_dino_catalog.py --verify
    run_dino_audio_checks
    PYTHONDONTWRITEBYTECODE=1 "${asset_python}" tools/generate_dino_animation.py --verify
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
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
