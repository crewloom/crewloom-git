#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
version=$(cat CREWLOOM_VERSION)
tag="crewloom-git-v$version"
repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
branch=$(git branch --show-current)
test "$branch" = crewloom-main || { echo >&2 'Release requires crewloom-main'; exit 1; }
test -z "$(git status --porcelain)" || { echo >&2 'Commit the selected verified sources first'; exit 1; }
git show-ref --verify --quiet "refs/tags/$tag" && { echo >&2 'Release tag already exists'; exit 1; }
./scripts/build-memory.sh
./scripts/test-memory.sh
sha=$(git rev-parse HEAD)
git push origin crewloom-main
# Every push is checked through official GitHub CLI, never a browser.
sleep 3
runs=$(gh run list --repo "$repo" --commit "$sha" --limit 30 --json databaseId --jq '.[].databaseId')
test -n "$runs" || { echo >&2 'No CI run was found; release not verified'; exit 1; }
for run_id in $runs
do
	if ! gh run watch "$run_id" --repo "$repo" --exit-status --interval 10
	then
		gh run view "$run_id" --repo "$repo" --log-failed || gh run view "$run_id" --repo "$repo"
		exit 1
	fi
done
git tag -a "$tag" -m "Native Git project memory $version"
git push origin "$tag"
sleep 3
runs=$(gh run list --repo "$repo" --commit "$sha" --limit 30 --json databaseId --jq '.[].databaseId')
for run_id in $runs
do
	if ! gh run watch "$run_id" --repo "$repo" --exit-status --interval 10
	then
		gh run view "$run_id" --repo "$repo" --log-failed || gh run view "$run_id" --repo "$repo"
		exit 1
	fi
done
gh release create "$tag" --repo "$repo" --target "$sha" \
	--title "Crewloom Git $version (Git 2.56.0)" --notes-file Documentation/crewloom/RELEASE-NOTES.md --latest=false
gh release view "$tag" --repo "$repo" --json tagName,url,isDraft
