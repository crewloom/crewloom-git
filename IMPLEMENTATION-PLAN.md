# Crewloom change memory: implementation plan

Status: direction changed on 2026-10-29. The native builtin released as
`crewloom-git-v0.1.0` is frozen as a reference prototype. New work is a change-memory
layer on top of an unmodified, installed Git. No layer code exists yet.

## Decision

Git already stores versions, computes differences and retrieves blobs. Extending a C fork
of Git duplicates mature work, and every upstream Git release would need a merge, conflict
resolution and the compatibility suite again. Users would also need a second Git binary.
The prototype has not shown a context or quota benefit that requires changing Git itself.

The unproven value is selecting the changes relevant to a task and keeping that selection
current, not storing bytes. That work belongs in a layer that reads Git, so effort moves there.

## What happens to the prototype

- `crewloom-git-v0.1.0` and its tag stay published and are not deleted or rewritten.
- `crewloom-main` receives no new features. It may receive security or correctness fixes
  only if a user depends on the prototype; otherwise it is archived after the layer ships.
- The prototype's acceptance gates and test cases are the reference behavior for the layer:
  staged versus working bytes, exact byte offsets, private store excluded from clone,
  mirror push and bundle, declared versus unknown attribution, explicit omissions.

## Layer design (proposed, not built)

- Runs the user's installed `git` through its plumbing commands (`ls-files -s`,
  `cat-file --batch`, `hash-object -w --stdin`, `mktree`, `commit-tree`, `update-ref`,
  `diff`, `log`). No changes to Git, its index, HEAD, hooks or the source object database.
- Keeps snapshots in a separate owner-only bare repository under the worktree's Git
  directory, as the prototype did, so clone, mirror push and bundle never carry them.
- Captures staged content from the index and raw working content from files as separate
  trees, publishing each snapshot with an expected-old-OID ref update.
- Watches files with the operating system's file events, with polling as the fallback.
- Builds task context from commits, diffs and snapshots: which paths changed, by whom
  (declared), and the smallest source-linked excerpts inside a strict byte budget.
- Attribution fields remain host declarations, never authenticated identity.
- Placement, language and package owner are an open decision. The layer is expected to
  live with the Crewloom agents that consume it, not in this fork.

## Measurement gate (first, before building the layer)

1. Freeze a set of real Crewloom tasks with known relevant files.
2. Baseline: the context an agent gets today from plain `git status`, `git diff` and `git log`.
3. Candidate: the same tasks with the layer's selected context.
4. Measure input tokens, cost, task success and missed relevant files for both.

Build the full layer only if the candidate reduces tokens or cost without lowering task
success. Report the measured numbers as they are, including a negative result.

## Archived: 0.1.0 native prototype plan

The plan below governed the frozen prototype. It is kept unchanged for reference.


Status: scope frozen after the scientific council; first implementation is built and its local acceptance suite passed. Remote release remains pending CI.
Upstream: Git v2.56.0, commit a018953688f1b10bddf91bff8747068f5f4746a4.
Branch: crewloom-main. The system Git executable is not modified or installed over.

### Council and decision

Independent architecture and adversarial reviews examined the upstream source. A separate
Claude Code call returned canonicalModel=claude-sonnet-5-5 and provider=firstParty, with
tools disabled; that reviewer assessed the supplied design rather than inspecting code.
All review opinions are design input, not experimental validation. Decision: CONDITIONAL GO
for a native, bounded first release; no quota, intelligence, uniqueness or semantic-completeness claim.
Private review transcripts are not part of this public repository.

The user requires memory inside Git's native implementation, automatic observed-change
history, explicit attribution and retrieval that avoids sending every source to a model.
This is not a shell wrapper, model service, or a new file/object format.

### Placement and ownership

- New native C modules own private storage, capture, retrieval, notes, and diagnostics.
- builtin/memory.c is a thin native interface registered in Git's builtin dispatcher.
- Makefile, meson.build, builtin.h and command-list.txt receive minimal additive wiring.
- Existing Git object types, index behavior, worktree and HEAD semantics stay unchanged.
- No Crewloom proprietary code, prompts, account identifiers or provider dependencies enter this fork.
- Existing Crewloom Python indexing/context/lessons are not duplicated or integrated by this release.

### Durable storage and privacy

Memory is off until explicit initialization with literal repository-relative tracked paths
or directory prefixes. The user explicitly selects whether raw worktree versions are retained.
Untracked, ignored-only, sparse, conflicted index and gitlink inputs are not silently ingested;
unsupported selected inputs fail explicitly. Binary and symlink handling must be tested or rejected.

Use a separate owner-only bare Git repository under the current worktree's own Git directory.
Its config is created narrowly without init_db, templates, copied remotes, hooks, alternates
or environment redirected object/index stores. Source and store object databases stay separate.
Bind the store to canonical worktree and gitdir identities. A custom ref in the source ODB
is not private: source push --mirror, clone and bundle must not export memory snapshots.
Backups of .git may include the store; linked-worktree removal can remove its memory.

Store standard blobs/trees/commits with actual reachability links. Each capture preserves
selected staged content and raw working content separately, plus declared provenance.
Publish only after all objects are written, using a store operation lock and expected-old-OID
ref update. Preserve old reachable generations. Do not claim atomicity against external writers.
Check index/HEAD drift and per-file stability. Lock contention or stale generations are explicit errors.
A failed or interrupted capture never modifies source index, HEAD or worktree.

### Interface and automatic operation

Native builtin operations: init, capture, status, history, context, diff, note and watch.
All structured output uses a versioned JSON schema, deterministic ordering, exact source IDs,
explicit omissions and strict byte limits. Human diagnostics support English and Arabic.
Context refreshes by default; --no-refresh intentionally reads retained data. No claim that
previously delivered context is retained inside a model. Retrieval remains source-linked.

The foreground watch operation performs bounded local polling without AI and captures saved
changes automatically while running; a watcher is not a daemon or every-keystroke recorder.
No hooks into add/commit in this release: their lock/failure semantics require a separate change.
No user has to issue capture for each edit while watch is running. Stop by normal process signals.
All attribution fields supplied by a host are declared, not authenticated identity; missing origin
is unknown. Observation time is distinct from actual modification time.

Notes are untrusted candidate data attached to recorded source versions. Source equality is
not semantic truth or dependency completeness. No note becomes executable instructions.
First release extraction is content/Git facts, bounded source excerpts and byte spans;
Tree-sitter semantic indexing and a Crewloom host adapter are deferred explicitly.

### Ordered task DAG

1. Build unchanged upstream baseline; record exact tools, flags and environment limitations.
2. Implement independent private-store opening, scope, locks and object/ref publication.
3. Implement raw/staged snapshot capture with drift checks and no source mutations.
4. Implement bounded retrieval, history, deterministic byte-span differences and candidate notes.
5. Wire the native builtin, English/Arabic diagnostics and foreground automatic watcher.
6. Run independent acceptance and applicable upstream compatibility suites; fix demonstrated defects.
7. Document exact support and exclusions in English/Arabic, changelog and reproducible build guide.
8. Commit focused changes, publish a semantic release, then verify GitHub Actions using gh.

Steps 2 and independent test fixtures may run in parallel after this plan; integration follows
shared interfaces with exclusive file ownership. No UI/site change is required for this native release.

### Acceptance gates

- Real compiled Git builtin, with no shell/git subprocess in memory modules.
- Off has no memory artifacts; existing init/add/status/worktree behavior retains its contract.
- Selected staged vs dirty data stays distinct; source HEAD/index/worktree/ODB unchanged.
- Historical exact-byte retrieval and a one-byte change with accurate offsets.
- Duplicate content reuses objects and does not append an unchanged capture.
- Explicit scope, missing/unsupported inputs, UTF-8/control-character paths and bounded JSON.
- Declared vs unknown attribution, untrusted notes and conservative source freshness.
- Concurrent capture serializes or fails without lost parent; crash leaves a valid reachable history.
- SHA-1/SHA-256 and independent linked-worktree stores; foreign/symlink store rejection.
- No filter/hook/network execution during capture; raw vs staged OIDs distinguished.
- Private-store fsck/repack/gc retains referenced snapshots; source mirror/clone/bundle excludes them.
- Native watcher captures a saved edit and stops cleanly; gaps/transient-edit limits documented.
- Complete output bytes measured only for a frozen illustrative retrieval task, not claimed quota savings.

Tests not run or unavailable sanitizer/build variants must remain explicit in release notes.
No release with known failing acceptance checks or failing remote CI is considered complete.

### Implementation result (2026-10-28)

The native builtin and documentation are implemented in this fork. The local
`DEVELOPER=1` build passed; `scripts/test-memory.sh` passed all 23 behavior tests,
9 safety tests, and 674 selected upstream compatibility tests (706 total). `git
diff --check` passed. Local sanitizer and Windows runs were not performed. Release
CI must pass and be checked through `gh` before calling this implementation released.
The first release makes no measured quota-saving, intelligence, tokenization, or
semantic-dependency claim. See `Documentation/crewloom/RELEASE-NOTES.md` for exact
scope and verification.
