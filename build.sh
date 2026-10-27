#!/bin/sh
# Builds the 32-bit wrapper (inotia4_nx.nsp) in the AArch32 toolchain container.
# Arguments go to make: ./build.sh clean, ./build.sh rt-files, ./build.sh DCR_GL_MESA=0
exec "$(dirname "$0")/runtime/tools/docker_build.sh" "$@"
