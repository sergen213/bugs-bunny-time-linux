#!/bin/sh
# Validation-clean smoke of the Vulkan backend through the real app: menus + 3 LiT + 3 TB levels with
# --vk-validation (Khronos layer + sync validation), incl. Enhanced sun shadows as a shadow map and with ray tracing
# forced off (fallback), ray-traced reflections (water L02A1, ice VICASG01) and FSR upscaling / frame generation. Fails on any "VK-VALIDATION" line or missing summary.
# usage: validation_levels.sh BBTIME_BINARY OUTDIR   (needs BBTIME_DATA; exit 77 = skipped)
bin=$1
out=$2
if [ -z "$BBTIME_DATA" ] || [ ! -d "$BBTIME_DATA/lit" ] || [ ! -d "$BBTIME_DATA/tb" ]; then
    echo "skip: BBTIME_DATA must contain lit/ and tb/"
    exit 77
fi
mkdir -p "$out"
fail=0
run() {
    name=$1
    shift
    log="$out/$name.log"
    "$bin" --renderer vk --vk-validation --hidden --mute --frames 20 --screenshot "$out/$name.png" "$@" >"$log" 2>&1
    rc=$?
    if [ $rc -ne 0 ] && grep -Eq 'SDL_Init|cannot create Vulkan window|no Vulkan 1.3 device' "$log"; then
        echo "skip: no display / Vulkan device ($name)"
        exit 77
    elif [ $rc -ne 0 ]; then
        echo "FAIL $name: exit $rc"; tail -5 "$log"; fail=1
    elif grep -q 'VK-VALIDATION' "$log"; then
        echo "FAIL $name: $(grep -c 'VK-VALIDATION' "$log") validation messages"; grep 'VK-VALIDATION' "$log" | head -5; fail=1
    elif ! grep -q 'vk: 0 validation messages' "$log" || ! grep -q 'screenshot .* written' "$log"; then
        echo "FAIL $name: no validation summary or screenshot"; tail -5 "$log"; fail=1
    else
        echo "ok $name"
    fi
}
run menu
run lit_L01A --game lit --level L01A
run lit_L02A1 --game lit --level L02A1
run lit_CC3A --game lit --level CC3A
run tb_ARCA01 --game tb --level ARCA01
run tb_TRCA01 --game tb --level TRCA01
run tb_VICA01 --game tb --level VICA01
run lit_L01A_original --game lit --level L01A --fidelity original
run tb_ARCA01_shadowmap --game tb --level ARCA01 --shadows map
run lit_L02A1_fsr_fg --game lit --level L02A1 --upscaling fsr_quality --frame-generation on --autoplay 'U*20'
run tb_ARCA01_fsr_perf --game tb --level ARCA01 --upscaling fsr_performance --shadows map
run tb_VICASG01_native_aa --game tb --level VICASG01 --upscaling native_aa --frame-generation on
export BBTIME_VK_NO_RT=1 BBTIME_VK_NO_FSR=1
run lit_L01A_no_rt --game lit --level L01A --shadows rt_ao --upscaling fsr_quality --frame-generation on
exit $fail
