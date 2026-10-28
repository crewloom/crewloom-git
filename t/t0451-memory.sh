#!/bin/sh

test_description='native memory snapshots, retrieval and private storage'

. ./test-lib.sh

memory_case () {
	"$PYTHON_PATH" "$TEST_DIRECTORY/lib-memory.py" "$GIT_BINARY" "$1"
}

test_expect_success PYTHON 'disabled memory creates no artifacts' 'memory_case disabled'
test_expect_success PYTHON 'raw capture requires explicit consent and literal tracked scope' 'memory_case consent'
test_expect_success PYTHON 'capture separates staged and raw bytes without source mutations' 'memory_case isolated'
test_expect_success PYTHON 'capture never runs configured filters or hooks' 'memory_case filters'
test_expect_success PYTHON 'unchanged capture deduplicates and attribution stays declared' 'memory_case history'
test_expect_success PYTHON 'context is valid bounded JSON with explicit omissions' 'memory_case bounds'
test_expect_success PYTHON 'context refresh and retained context have distinct semantics' 'memory_case refresh'
test_expect_success PYTHON 'diff reports exact replacement byte spans' 'memory_case spans'
test_expect_success PYTHON 'notes retain all dependencies and report stale evidence' 'memory_case notes'
test_expect_success PYTHON 'source clone mirror and bundle exclude private content' 'memory_case privacy'
test_expect_success PYTHON 'private fsck and gc retain snapshot and note sources' 'memory_case gc'
test_expect_success PYTHON 'SHA-256 source uses valid independent memory objects' 'memory_case sha256'
test_expect_success PYTHON 'linked worktrees use independent bound stores' 'memory_case worktrees'
test_expect_success PYTHON 'binary bytes are retained but omitted from textual context' 'memory_case binary'
test_expect_success PYTHON 'Unicode and control-character paths round trip through JSON' 'memory_case unicode'
test_expect_success PYTHON 'conflicted index is rejected without changing source state' 'memory_case conflict'
test_expect_success PYTHON 'sparse checkout and gitlink scopes fail explicitly' 'memory_case unsupported'
test_expect_success PYTHON 'foreign store is rejected' 'memory_case foreign'
test_expect_success PYTHON,SYMLINKS 'store symlinks are rejected and source symlinks are not followed' 'memory_case symlinks'
test_expect_success PYTHON 'foreground watch captures saved edits and terminates' 'memory_case watch'
test_expect_success PYTHON 'English and Arabic diagnostics preserve failure semantics' 'memory_case language'
test_expect_success PYTHON,!MINGW 'invalid UTF-8 selected paths fail explicitly' 'memory_case invalid_utf8'
test_expect_success PYTHON 'store lock contention does not publish partial state' 'memory_case lock'

test_done
