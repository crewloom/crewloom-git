#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(cat "$root/CREWLOOM_VERSION")
python_path=$(command -v python3)
exec make -C "$root" -j"${JOBS:-4}" \
	NO_RUST=YesPlease NO_GETTEXT=YesPlease NO_TCLTK=YesPlease \
	NO_PERL=YesPlease NO_CURL=YesPlease NO_OPENSSL=YesPlease \
	PYTHON_PATH="$python_path" GIT_VERSION="2.56.0.crewloom.$version" \
	"${@:-all}"
