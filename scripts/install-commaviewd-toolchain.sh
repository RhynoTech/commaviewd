#!/usr/bin/env bash
set -euxo pipefail

sudo dpkg --add-architecture arm64

CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME}")"
sudo mv /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list.d/ubuntu.sources.bak 2>/dev/null || true
cat <<EOF | sudo tee /etc/apt/sources.list >/dev/null
deb [arch=amd64] http://archive.ubuntu.com/ubuntu ${CODENAME} main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu ${CODENAME}-updates main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu ${CODENAME}-backports main restricted universe multiverse
deb [arch=amd64] http://security.ubuntu.com/ubuntu ${CODENAME}-security main restricted universe multiverse

deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${CODENAME} main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${CODENAME}-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${CODENAME}-backports main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${CODENAME}-security main restricted universe multiverse
EOF
APT_CACHE_DIR="$HOME/.cache/commaview-apt/archives"
mkdir -p "$APT_CACHE_DIR/partial"

sudo apt-get -o Dir::Cache::archives="$APT_CACHE_DIR" update
sudo apt-get -o Dir::Cache::archives="$APT_CACHE_DIR" install -y --no-install-recommends \
  clang \
  g++-aarch64-linux-gnu \
  binutils-aarch64-linux-gnu \
  capnproto \
  python3 \
  libzmq3-dev \
  libcapnp-dev

capnp_pkg=""
for pkg in libcapnp-0.8.0 libcapnp-0.9.2 libcapnp-1.0.1 libcapnp-1.0.2; do
  if apt-cache show "${pkg}:arm64" >/dev/null 2>&1; then
    capnp_pkg="$pkg"
    break
  fi
done

if [[ -z "$capnp_pkg" ]]; then
  capnp_pkg="$(apt-cache search '^libcapnp-[0-9]' | awk '{print $1}' | sort -V | tail -n1 || true)"
fi

if [[ -z "$capnp_pkg" ]]; then
  echo "::error::No arm64 libcapnp runtime package found"
  exit 1
fi

sudo apt-get -o Dir::Cache::archives="$APT_CACHE_DIR" install -y --no-install-recommends \
  "${capnp_pkg}:arm64" \
  libzmq5:arm64

sudo chown -R "$USER":"$USER" "$APT_CACHE_DIR" || true

ARM_CAPNP_SO="$(ls -1 /usr/lib/aarch64-linux-gnu/libcapnp-*.so | head -n1 || true)"
ARM_KJ_SO="$(ls -1 /usr/lib/aarch64-linux-gnu/libkj-*.so | head -n1 || true)"
ARM_ZMQ_SO="$(ls -1 /usr/lib/aarch64-linux-gnu/libzmq.so.* | head -n1 || true)"

if [[ -z "$ARM_CAPNP_SO" || -z "$ARM_KJ_SO" || -z "$ARM_ZMQ_SO" ]]; then
  echo "::error::Failed to detect arm64 capnp/kj/zmq shared libraries"
  ls -la /usr/lib/aarch64-linux-gnu | head -n 200
  exit 1
fi

sudo ln -sf "$(basename "$ARM_CAPNP_SO")" /usr/lib/aarch64-linux-gnu/libcapnp.so
sudo ln -sf "$(basename "$ARM_KJ_SO")" /usr/lib/aarch64-linux-gnu/libkj.so
sudo ln -sf "$(basename "$ARM_ZMQ_SO")" /usr/lib/aarch64-linux-gnu/libzmq.so

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "arm_capnp_so=$ARM_CAPNP_SO" >> "$GITHUB_OUTPUT"
  echo "arm_kj_so=$ARM_KJ_SO" >> "$GITHUB_OUTPUT"
fi

echo "ARM_CAPNP_SO=$ARM_CAPNP_SO"
echo "ARM_KJ_SO=$ARM_KJ_SO"
