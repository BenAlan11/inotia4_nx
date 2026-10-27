#!/usr/bin/env bash
# setup-env.sh -- one-time toolchain setup for inotia4_nx.
#
# Run inside WSL (Ubuntu) or any Linux with Docker. Not Git Bash: docker run -v
# needs the POSIX paths.
#
#   ./tools/setup-env.sh              # libnx32 + Mesa + portlibs32, then the port
#   ./tools/setup-env.sh --deps-only  # stop after portlibs32 (no build)
#   ./tools/setup-env.sh --rebuild    # re-run the three builds even if done
#
# It expects the layout the builds want:
#
#   <root>/inotia4_nx     this port
#   <root>/libnx32        built here if missing
#   <root>/mesa-switch32  built here if missing
#
# Everything heavy runs in ghcr.io/vita2hos/devcontainer/vita2hos, the same
# image runtime/tools/docker_build.sh uses. MIT.
set -euo pipefail

PORT="$(cd "$(dirname "$0")/.." && pwd -P)"
ROOT="$(dirname "$PORT")"
LIBNX="$ROOT/libnx32"
MESA="$ROOT/mesa-switch32"
IMAGE="ghcr.io/vita2hos/devcontainer/vita2hos:latest"

DEPS_ONLY=0
REBUILD=0
for a in "$@"; do
  case "$a" in
    --deps-only) DEPS_ONLY=1 ;;
    --rebuild)   REBUILD=1 ;;
    -h|--help)   sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

say "checking docker"
command -v docker >/dev/null 2>&1 || die "docker not found. Install Docker Desktop and turn on WSL integration for this distro."
docker info >/dev/null 2>&1 || die "the docker daemon is not reachable. Start Docker Desktop."
echo "ok: $(docker --version)"

say "pulling the toolchain image (first time: a few GB)"
docker pull "$IMAGE"

say "checking the tree"
[ -f "$PORT/Makefile" ] || die "$PORT does not look like the port (no Makefile)"
[ -d "$PORT/runtime" ] || die "$PORT/runtime is missing. Unzip android32 into runtime/ or add it as a submodule."
echo "port:  $PORT"
echo "root:  $ROOT"

# ---------------------------------------------------------------- libnx32
if [ ! -d "$LIBNX" ]; then
  say "cloning libnx32"
  git clone https://github.com/aks796/libnx32 "$LIBNX"
fi
if [ "$REBUILD" = 1 ] || [ ! -f "$LIBNX/prefix/lib/libnx.a" ]; then
  say "building libnx32 (container)"
  ( cd "$LIBNX" && ./build.sh )
else
  echo "libnx32: already built"
fi
[ -f "$LIBNX/prefix/lib/libnx.a" ] || die "libnx32 did not produce prefix/lib/libnx.a"

# ------------------------------------------------------------ mesa-switch32
if [ ! -d "$MESA" ]; then
  say "cloning mesa-switch32 (a Mesa fork: this one is big)"
  git clone https://github.com/aks796/mesa-switch32 "$MESA"
fi
if [ "$REBUILD" = 1 ] || [ ! -f "$MESA/build32/prefix/lib/libnouveau_horizon.a" ]; then
  say "building AArch32 Mesa (this takes a while: Mesa under x86 emulation)"
  ( cd "$MESA" && LIBNX32="$LIBNX/prefix" ./build32.sh )
else
  echo "mesa-switch32: already built"
fi
[ -f "$MESA/build32/prefix/lib/libGLESv1_CM.a" ] || die "Mesa did not produce libGLESv1_CM.a"
[ -f "$MESA/build32/prefix/lib/libnouveau_horizon.a" ] || die "Mesa did not produce libnouveau_horizon.a (runtime.mk needs it to pick 26.2)"

# ------------------------------------------------------------- portlibs32
say "assembling portlibs32"
mkdir -p "$PORT/portlibs32"
rm -rf "$PORT/portlibs32/lib" "$PORT/portlibs32/include"
cp -r "$MESA/build32/prefix/lib" "$PORT/portlibs32/"
cp -r "$MESA/build32/prefix/include" "$PORT/portlibs32/"
echo "ok: $PORT/portlibs32"

say "verifying the port tree"
( cd "$PORT" && ./tools/verify.sh ) || die "verify.sh failed"

if [ "$DEPS_ONLY" = 1 ]; then
  say "done (--deps-only)"
  exit 0
fi

say "building the 32-bit program"
( cd "$PORT" && ./build.sh )

say "building the 64-bit launcher"
( cd "$PORT/launcher" && ./build.sh )

say "done"
ls -la "$PORT"/*.nsp "$PORT"/*.build "$PORT"/launcher/*.nro 2>/dev/null || true
cat <<'EOF'

Next: copy to the SD card

  sd:/switch/inotia4_nx/inotia4_nx.nro
  sd:/switch/inotia4_nx/<your Inotia 4 1.3.7 APK>

then in sphaira: Homebrew -> Inotia 4 -> Install Forwarder.
EOF
