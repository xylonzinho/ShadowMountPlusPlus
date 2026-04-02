#!/usr/bin/env bash
set -euo pipefail

# One-command local build wrapper for macOS/Linux hosts.
# Runs the same dependency flow as CI inside Ubuntu.

IMAGE="ubuntu:24.04"
WORKDIR="/work"
REPO_NAME="pacbrew-repo"

if ! command -v docker >/dev/null 2>&1; then
  echo "[build.sh] docker is required but not found." >&2
  echo "Install Docker Desktop (or Colima + docker CLI) and retry." >&2
  exit 1
fi

SCRIPT='set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt update
apt install -y \
  autoconf \
  automake \
  build-essential \
  clang-18 \
  curl \
  git \
  libarchive-tools \
  libtool \
  lld-18 \
  makepkg \
  meson \
  pacman-package-manager \
  pkg-config \
  xxd \
  zip

if [[ ! -d "'"${WORKDIR}"'"/"'"${REPO_NAME}"'" ]]; then
  git clone https://github.com/EchoStretch/pacbrew-repo "'"${WORKDIR}"'"/"'"${REPO_NAME}"'"
fi

cd "'"${WORKDIR}"'"/"'"${REPO_NAME}"'"/sdk
makepkg -c -f
pacman --noconfirm -U ./ps5-payload-*.pkg.tar.gz

cd "'"${WORKDIR}"'"/"'"${REPO_NAME}"'"/sqlite
makepkg -c -f
pacman --noconfirm -U ./ps5-payload-*.pkg.tar.gz

cd "'"${WORKDIR}"'"
make clean all

echo "[build.sh] build complete"
'

echo "[build.sh] starting containerized build..."
docker run --rm -t \
  -v "$PWD:${WORKDIR}" \
  -w "${WORKDIR}" \
  "${IMAGE}" \
  bash -lc "${SCRIPT}"
