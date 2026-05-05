#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# local_ci.sh — Run the full WebGPU test suite locally (mirrors CI + Safari)
#
# Usage:
#   ./webgpu_tests/local_ci.sh              # Run all tests (Chrome + Firefox + Safari)
#   ./webgpu_tests/local_ci.sh --no-safari  # Skip Safari (faster, no AppleScript needed)
#   ./webgpu_tests/local_ci.sh --quick      # Shader corpus + scene smoketest only
#
# Prerequisites:
#   - Node.js 20+
#   - glslangValidator (brew install glslang)
#   - Playwright browsers: npx playwright install chromium firefox
#   - Safari: Enable Develop → "Allow JavaScript from Apple Events" (macOS only)
#
# Exit codes:
#   0 = all tests pass
#   1 = one or more tests failed
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Parse args
NO_SAFARI=false
QUICK=false
for arg in "$@"; do
    [[ "$arg" == "--no-safari" ]] && NO_SAFARI=true
    [[ "$arg" == "--quick" ]] && QUICK=true
done

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

PASSED=0
FAILED=0
SKIPPED=0
RESULTS=()

run_test() {
    local name="$1"
    local dir="$2"
    shift 2
    local cmd=("$@")

    printf "${BOLD}▶ %-40s${NC}" "$name"

    if [[ ! -d "$dir" ]]; then
        printf "${YELLOW}SKIP${NC} (directory not found)\n"
        SKIPPED=$((SKIPPED + 1))
        RESULTS+=("SKIP  $name")
        return
    fi

    local output
    if output=$(cd "$dir" && "${cmd[@]}" 2>&1); then
        printf "${GREEN}PASS${NC}\n"
        PASSED=$((PASSED + 1))
        RESULTS+=("PASS  $name")
    else
        printf "${RED}FAIL${NC}\n"
        # Show last 10 lines of output on failure
        echo "$output" | tail -10 | sed 's/^/    /'
        FAILED=$((FAILED + 1))
        RESULTS+=("FAIL  $name")
    fi
}

# ──────────────────────────────────────────────────────────────────────────────
# Ensure playwright is installed where needed
# ──────────────────────────────────────────────────────────────────────────────
ensure_playwright() {
    local dir="$1"
    if [[ ! -d "$dir/node_modules/playwright" ]]; then
        echo "  [setup] Installing playwright in $dir..."
        (cd "$dir" && [[ ! -f package.json ]] && npm init -y > /dev/null 2>&1; npm install playwright > /dev/null 2>&1)
    fi
}

# ──────────────────────────────────────────────────────────────────────────────
echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║   WebGPU Local CI — Full Test Suite                      ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

if [[ "$QUICK" == true ]]; then
    echo "  Mode: --quick (shader corpus + scene smoketest only)"
else
    echo "  Mode: full"
fi
if [[ "$NO_SAFARI" == true ]]; then
    echo "  Safari: skipped (--no-safari)"
fi
echo ""

# ──────────────────────────────────────────────────────────────────────────────
# 1. Shader Corpus
# ──────────────────────────────────────────────────────────────────────────────
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Stage 1: Shader Corpus (SPIR-V → WGSL)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

run_test "Compile GLSL fixtures" \
    "$SCRIPT_DIR/shader_corpus" \
    bash ./compile_fixtures.sh

run_test "SPIR-V → WGSL validation" \
    "$SCRIPT_DIR/shader_corpus" \
    node run_tests.mjs

echo ""

# ──────────────────────────────────────────────────────────────────────────────
# 2. Unit Tests (naga-converter + driver)
# ──────────────────────────────────────────────────────────────────────────────
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Stage 2: Unit Tests"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

run_test "Naga-converter Rust unit tests (71)" \
    "$SCRIPT_DIR/../drivers/webgpu/naga-converter" \
    cargo test --lib

run_test "Driver unit tests (305)" \
    "$SCRIPT_DIR/driver_unit_tests" \
    node run_tests.mjs

echo ""

# ──────────────────────────────────────────────────────────────────────────────
# 3. Scene Smoketest (multi-browser)
# ──────────────────────────────────────────────────────────────────────────────
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Stage 3: Scene Smoketest (18 scenes × browsers)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

run_test "Scene smoketest — Chrome (18 scenes)" \
    "$SCRIPT_DIR/scene_smoketest" \
    node run_scenes.mjs --browser chrome --timeout 30000

run_test "Scene smoketest — Firefox (18 scenes)" \
    "$SCRIPT_DIR/scene_smoketest" \
    node run_scenes.mjs --browser firefox --timeout 30000

if [[ "$NO_SAFARI" == false && "$(uname)" == "Darwin" ]]; then
    run_test "Scene smoketest — Safari (18 scenes)" \
        "$SCRIPT_DIR/scene_smoketest" \
        node run_scenes.mjs --browser safari --timeout 30000
else
    printf "${BOLD}▶ %-40s${NC}${YELLOW}SKIP${NC} (--no-safari or not macOS)\n" "Scene smoketest — Safari (18 scenes)"
    SKIPPED=$((SKIPPED + 1))
    RESULTS+=("SKIP  Scene smoketest — Safari (18 scenes)")
fi

echo ""

if [[ "$QUICK" == true ]]; then
    # Skip remaining stages in quick mode
    echo "(--quick: skipping resource lifecycle, screenshot comparison, and smoke test)"
    echo ""
else

# ──────────────────────────────────────────────────────────────────────────────
# 4. Resource Lifecycle
# ──────────────────────────────────────────────────────────────────────────────
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Stage 4: Resource Lifecycle Stress Test"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

ensure_playwright "$SCRIPT_DIR/resource_lifecycle"

run_test "Resource lifecycle stress tests" \
    "$SCRIPT_DIR/resource_lifecycle" \
    node run_tests.mjs

echo ""

# ──────────────────────────────────────────────────────────────────────────────
# 5. Screenshot Comparison
# ──────────────────────────────────────────────────────────────────────────────
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Stage 5: Screenshot Comparison (Chrome + Firefox)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

ensure_playwright "$SCRIPT_DIR/screenshot_comparison"

run_test "Screenshot comparison" \
    "$SCRIPT_DIR/screenshot_comparison" \
    node screenshot_tests.mjs

echo ""

# ──────────────────────────────────────────────────────────────────────────────
# 6. Smoke Test (requires pre-exported test project)
# ──────────────────────────────────────────────────────────────────────────────
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Stage 6: Engine Smoke Test (test_project)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [[ -f "$SCRIPT_DIR/test_project/export/index.html" ]]; then
    run_test "Headless Chrome smoke test" \
        "$SCRIPT_DIR/test_project" \
        node smoke_test.mjs ./export/
else
    printf "${BOLD}▶ %-40s${NC}${YELLOW}SKIP${NC} (no export found — run with engine build)\n" "Headless Chrome smoke test"
    SKIPPED=$((SKIPPED + 1))
    RESULTS+=("SKIP  Headless Chrome smoke test")
fi

echo ""

fi  # end of non-quick block

# ──────────────────────────────────────────────────────────────────────────────
# Summary
# ──────────────────────────────────────────────────────────────────────────────
echo "═══════════════════════════════════════════════════════════════"
echo "  RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo ""
for r in "${RESULTS[@]}"; do
    case "$r" in
        PASS*) printf "  ${GREEN}✓${NC} %s\n" "${r#PASS  }" ;;
        FAIL*) printf "  ${RED}✗${NC} %s\n" "${r#FAIL  }" ;;
        SKIP*) printf "  ${YELLOW}○${NC} %s\n" "${r#SKIP  }" ;;
    esac
done
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
printf "  Total: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}, ${YELLOW}%d skipped${NC}\n" "$PASSED" "$FAILED" "$SKIPPED"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [[ $FAILED -gt 0 ]]; then
    exit 1
fi
