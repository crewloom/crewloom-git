#!/bin/sh

test_description='native memory rejects unsafe limits, stores and publication'

. ./test-lib.sh

memory_safety_case () {
	"$PYTHON_PATH" "$TEST_DIRECTORY/lib-memory-safety.py" "$GIT_BINARY" "$1"
}

test_expect_success PYTHON 'negative limits are rejected without publishing' 'memory_safety_case negative_limits'
test_expect_success PYTHON 'ULONG_MAX limits are rejected without publishing' 'memory_safety_case overflow_limits'
test_expect_success PYTHON 'duplicate limits are rejected rather than overridden' 'memory_safety_case duplicate_limits'
test_expect_success PYTHON 'oversized staged blob is rejected before private object copy' 'memory_safety_case large_staged'
test_expect_success PYTHON 'foreign snapshot schema cannot become a valid capture ancestor' 'memory_safety_case foreign_schema'
test_expect_success PYTHON 'host global hooks and filters do not run during capture' 'memory_safety_case global_hooks'
test_expect_success PYTHON 'concurrent captures serialize or report LOCK without lost parents' 'memory_safety_case concurrent'
test_expect_success PYTHON 'corrupt private source blob is visible and never republished' 'memory_safety_case corrupt_blob'
test_expect_success PYTHON 'partial initialization is preserved and never overwritten' 'memory_safety_case partial_init'

test_done
