#!/run/current-system/sw/bin/bash
# Binar test runner
# Each test is a .binar file. Exit code = expected exit code.
# Module tests: directories with binar.mod + main.binar
# Usage: ./tests/run_tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINAR="$SCRIPT_DIR/../build/binar"
TMP_DIR="/tmp/binar_tests"

mkdir -p "$TMP_DIR"

PASSED=0
FAILED=0
TOTAL=0

run_test() {
    local name="$1"
    local obj_file="$2"
    local exe_file="$3"
    local expected="$4"
    local test_file="$5"
    local expect_error="$6"

    TOTAL=$((TOTAL + 1))

    # Compile
    "$BINAR" -o "$obj_file" "$test_file" 2>"$TMP_DIR/$name.err"
    if [ $? -ne 0 ]; then
        if [ "$expect_error" = "1" ]; then
            echo "PASS $name (expected compile error)"
            PASSED=$((PASSED + 1))
        else
            echo "FAIL $name (compile error)"
            cat "$TMP_DIR/$name.err"
            FAILED=$((FAILED + 1))
        fi
        return
    fi

    if [ "$expect_error" = "1" ]; then
        echo "FAIL $name (expected compile error but compiled successfully)"
        FAILED=$((FAILED + 1))
        return
    fi

    # Link
    gcc -nostartfiles -o "$exe_file" "$obj_file" 2>"$TMP_DIR/$name.err"
    if [ $? -ne 0 ]; then
        echo "FAIL $name (link error)"
        cat "$TMP_DIR/$name.err"
        FAILED=$((FAILED + 1))
        return
    fi

    # Run
    "$exe_file" >/dev/null 2>&1
    actual=$?

    if [ "$actual" -eq "$expected" ]; then
        echo "PASS $name (exit=$actual)"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL $name (expected=$expected, actual=$actual)"
        FAILED=$((FAILED + 1))
    fi
}

# Regular .binar tests
for test_file in "$SCRIPT_DIR"/*.binar; do
    [ -f "$test_file" ] || continue
    name=$(basename "$test_file" .binar)
    expected=$(grep -m1 '// exit:' "$test_file" | sed 's/.*\/\/ exit: *//' | tr -d ' ')
    if [ -z "$expected" ]; then
        expected=0
    fi
    expect_error=0
    if grep -q '// error:' "$test_file"; then
        expect_error=1
    fi
    run_test "$name" "$TMP_DIR/$name.o" "$TMP_DIR/$name" "$expected" "$test_file" "$expect_error"
done

# Module tests: directories with binar.mod
for mod_dir in "$SCRIPT_DIR"/*/; do
    [ -f "${mod_dir}binar.mod" ] || continue
    [ -f "${mod_dir}main.binar" ] || continue
    name=$(basename "$mod_dir")
    expected=$(grep -m1 '// exit:' "${mod_dir}main.binar" | sed 's/.*\/\/ exit: *//' | tr -d ' ')
    if [ -z "$expected" ]; then
        expected=0
    fi
    expect_error=0
    if grep -q '// error:' "${mod_dir}main.binar"; then
        expect_error=1
    fi
    run_test "$name" "$TMP_DIR/$name.o" "$TMP_DIR/$name" "$expected" "${mod_dir}main.binar" "$expect_error"
done

echo ""
echo "Results: $PASSED/$TOTAL passed, $FAILED failed"
exit $FAILED
