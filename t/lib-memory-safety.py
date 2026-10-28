#!/usr/bin/env python3
"""Adversarial fixtures for the native memory storage and publication boundary."""

import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


spec = importlib.util.spec_from_file_location(
    "memory_acceptance", Path(__file__).with_name("lib-memory.py"))
common = importlib.util.module_from_spec(spec)
spec.loader.exec_module(common)
Fixture, ROOT, BINARY = common.Fixture, common.ROOT, common.BINARY
run, decoded = common.run, common.decoded
SNAPSHOTS = "refs/heads/snapshots"


def tip(fixture):
    return fixture.stored("rev-parse", SNAPSHOTS).stdout.strip()


def limit_cases(value, duplicate=False):
    for key in ("maxfile", "maxtotal", "maxfiles"):
        f = Fixture(key)
        f.init()
        f.capture()
        before = tip(f)
        operation = "--add" if duplicate else "--replace-all"
        f.git("config", "--file", str(f.store / "config"), operation,
              "memory." + key, value)
        state = f.state()
        f.git("memory", "status", ok=False)
        f.git("memory", "capture", ok=False)
        assert tip(f) == before
        assert f.state() == state


def negative_limits():
    limit_cases("-1")


def overflow_limits():
    limit_cases(str((1 << (8 * ctypes.sizeof(ctypes.c_ulong))) - 1))


def duplicate_limits():
    limit_cases("1024", duplicate=True)


def large_staged():
    f = Fixture()
    f.init()
    f.capture()
    before = tip(f)
    f.write("src/a.txt", b"x" * (1024 * 1024 + 1))
    f.git("add", "src/a.txt")
    oversized_oid = f.git("rev-parse", ":src/a.txt").stdout.decode().strip()
    f.write("src/a.txt", b"small raw worktree\n")
    state = f.state()
    f.git("memory", "capture", ok=False)
    assert tip(f) == before
    assert f.state() == state
    f.stored("cat-file", "-e", oversized_oid, ok=False)


def foreign_schema():
    f = Fixture()
    f.init()
    first = f.capture()["snapshot"]
    event = json.loads(f.stored("show", first + ":event.json").stdout)
    event["schema"] = 999
    blob = f.stored("hash-object", "-w", "--stdin",
                    data=json.dumps(event).encode()).stdout.strip()
    entries = f.stored("ls-tree", "-z", first).stdout.split(b"\0")
    replacement = b"100644 blob " + blob + b"\tevent.json"
    tree_data = b"\0".join(replacement if e.endswith(b"\tevent.json") else e
                            for e in entries)
    tree = f.stored("mktree", "-z", data=tree_data).stdout.decode().strip()
    foreign = f.stored("commit-tree", tree, "-p", first,
                       data=b"foreign memory schema\n").stdout.decode().strip()
    f.stored("update-ref", SNAPSHOTS, foreign, first)
    f.write("src/a.txt", b"changed after foreign snapshot\n")
    state = f.state()
    f.git("memory", "capture", ok=False)
    assert tip(f).decode() == foreign
    assert f.state() == state


def global_hooks():
    f = Fixture()
    home = ROOT / "isolated-home"
    hooks = home / "hooks"
    hooks.mkdir(parents=True)
    hook_marker, filter_marker = ROOT / "HOOK-RAN", ROOT / "FILTER-RAN"
    hook = hooks / "reference-transaction"
    hook.write_text("#!/bin/sh\nprintf hook >> " + shlex.quote(str(hook_marker)) + "\n")
    hook.chmod(0o755)
    config = home / ".gitconfig"
    run(ROOT, "config", "--file", str(config), "core.hooksPath", str(hooks))
    command = "printf filter >> " + shlex.quote(str(filter_marker)) + "; cat"
    for direction in ("clean", "smudge"):
        run(ROOT, "config", "--file", str(config), "filter.globaltrap." + direction, command)
    f.write(".gitattributes", b"src/* filter=globaltrap\n")
    f.write("src/a.txt", b"raw host-global filter bait\r\n")
    env = os.environ.copy()
    env.pop("GIT_CONFIG_GLOBAL", None)
    env["HOME"], env["XDG_CONFIG_HOME"] = str(home), str(home / "xdg")
    effective = f.git("config", "--get", "core.hooksPath", env=env).stdout
    assert effective.decode().strip() == str(hooks), "global fixture was not active"
    f.mem("init", "--worktree", "--", "src/", env=env)
    snapshot = f.mem("capture", env=env)["snapshot"]
    assert not hook_marker.exists() and not filter_marker.exists()
    actual = f.stored("show", snapshot + ":worktree/src/a.txt").stdout
    assert actual == b"raw host-global filter bait\r\n"


def concurrent():
    f = Fixture()
    f.init()
    first = f.capture()["snapshot"]
    f.write("src/a.txt", b"one shared concurrent generation\n")
    state = f.state()
    processes = []
    try:
        for _ in range(6):
            processes.append(subprocess.Popen([BINARY, "memory", "capture"], cwd=f.path,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        successes = []
        for process in processes:
            out, err = process.communicate(timeout=20)
            if process.returncode == 0:
                value = json.loads(out)
                assert value["schema"] == 1
                successes.append(value)
            else:
                assert b"LOCK:" in err, (process.returncode, out, err)
        assert successes, "all captures failed"
        assert sum(bool(s["changed"]) for s in successes) == 1
        latest = tip(f).decode()
        assert all(s["snapshot"] == latest for s in successes)
        parent = f.stored("rev-parse", latest + "^").stdout.decode().strip()
        assert parent == first
        assert f.stored("rev-list", "--count", SNAPSHOTS).stdout.strip() == b"2"
        assert f.state() == state
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.communicate()


def corrupt_blob():
    f = Fixture()
    f.write("src/a.txt", b"ONLY_PRIVATE_SOURCE_98351\n")
    f.init()
    first = f.capture()["snapshot"]
    oid = f.stored("rev-parse", first + ":worktree/src/a.txt").stdout.decode().strip()
    loose = f.store / "objects" / oid[:2] / oid[2:]
    assert loose.is_file(), "fixture requires a loose private source blob"
    loose.chmod(0o600)
    loose.write_bytes(b"intentionally corrupt zlib object")
    state = f.state()
    f.git("memory", "context", "--no-refresh", ok=False)
    f.git("memory", "capture", ok=False)
    assert tip(f).decode() == first
    assert f.state() == state


def partial_init():
    f = Fixture()
    f.store.mkdir(mode=0o700)
    marker = f.store / "PARTIAL-INIT-MARKER"
    marker.write_bytes(b"existing partial initialization must survive\n")
    marker.chmod(0o600)
    before = {str(p.relative_to(f.store)): p.read_bytes()
              for p in f.store.rglob("*") if p.is_file()}
    state = f.state()
    f.git("memory", "init", "--worktree", "--", "src/", ok=False)
    after = {str(p.relative_to(f.store)): p.read_bytes()
             for p in f.store.rglob("*") if p.is_file()}
    assert after == before
    assert f.state() == state
    f.git("memory", "status", ok=False)


globals()[sys.argv[2]]()
