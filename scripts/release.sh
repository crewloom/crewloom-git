#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
version=$(cat CREWLOOM_VERSION)
tag="crewloom-git-v$version"
repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
branch=$(git branch --show-current)
# GitHub registers push runs a few seconds to minutes after the push, so
# poll for them. Filtering by ref keeps the tag check from reusing branch runs.
# Only the fork's workflow gates a release; upstream workflows report skipped,
# which gh run watch --exit-status treats as a failure.
release_workflow=crewloom-memory.yml
run_lookup_attempts=${CREWLOOM_RUN_LOOKUP_ATTEMPTS:-30}
run_lookup_delay=${CREWLOOM_RUN_LOOKUP_DELAY:-10}
find_runs () {
	attempt=1
	while :
	do
		runs=$(gh run list --repo "$repo" --workflow "$release_workflow" \
			--commit "$sha" --branch "$1" --limit 30 \
			--json databaseId --jq '.[].databaseId')
		test -n "$runs" && return 0
		test "$attempt" -lt "$run_lookup_attempts" || break
		attempt=$((attempt + 1))
		sleep "$run_lookup_delay"
	done
	echo >&2 "No CI run was found for $1 at $sha; release not verified"
	exit 1
}
wait_for_runs () {
	find_runs "$1"
	for run_id in $runs
	do
		if ! gh run watch "$run_id" --repo "$repo" --exit-status --interval 10
		then
			gh run view "$run_id" --repo "$repo" --log-failed || gh run view "$run_id" --repo "$repo"
			exit 1
		fi
	done
}
test "$branch" = crewloom-main || { echo >&2 'Release requires crewloom-main'; exit 1; }
test -z "$(git status --porcelain)" || { echo >&2 'Commit the selected verified sources first'; exit 1; }
git show-ref --verify --quiet "refs/tags/$tag" && { echo >&2 'Release tag already exists'; exit 1; }
./scripts/build-memory.sh
./scripts/test-memory.sh
sha=$(git rev-parse HEAD)
git push origin crewloom-main
# Every push is checked through official GitHub CLI, never a browser.
wait_for_runs crewloom-main
git tag -a "$tag" -m "Native Git project memory $version"
git push origin "$tag"
wait_for_runs "$tag"
gh release create "$tag" --repo "$repo" --target "$sha" \
	--title "Crewloom Git $version (Git 2.56.0)" --notes-file Documentation/crewloom/RELEASE-NOTES.md --latest=false
gh release view "$tag" --repo "$repo" --json tagName,url,isDraft
