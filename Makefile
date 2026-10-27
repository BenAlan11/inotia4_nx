# Inotia 4 for the Nintendo Switch -- the 32-bit wrapper.
#
# Builds <TARGET>.nsp (an ExeFS NSP: the program the launcher NRO installs) and
# <TARGET>.build. Needs the runtime at ./runtime and the AArch32 toolchain
# container: run ./build.sh, not make.

TARGET               := inotia4_nx
PORT_NPDM_PROGRAM_ID := 0x01000000000010a4
PORT_NPDM_VERSION    := 0.1.0

include runtime/runtime.mk

# stb_vorbis is included from inotia_audio.c (as .inc), so the warnings its
# older C style triggers are not the port's.
$(BUILD)/inotia_audio.o: CFLAGS += -Wno-unused-but-set-variable -Wno-sign-compare
