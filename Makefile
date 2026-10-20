# NVIDIA NVENC encoder for VLC 3. Separate from vlc-gpmf.
#
#   mingw32-make
#
# The VLC 3.0.23 plugin headers are the SDK already unpacked for the HUD
# plugin. Override VLC_INC if that tree is somewhere else. The DLL links the
# installed libvlccore and loads nvEncodeAPI64.dll from the NVIDIA driver
# at runtime.

# GNU make's built-in CC is "cc". ?= does not replace that default.
ifeq ($(origin CC),default)
  CC = gcc
endif
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
SRC      = src/nvenc.c

ifeq ($(OS),Windows_NT)
  OUT      := build/libnvenc_plugin.dll
  VLC_INC  ?= ../vlc-gpmf/third_party/vlc/vlc-3.0.23/sdk/include/vlc/plugins
  VLC_DLL  ?= C:/Program Files/VideoLAN/VLC/libvlccore.dll
  CFLAGS   += -I$(VLC_INC) -Ithird_party/nv-codec-headers/include \
              -DMODULE_STRING=\"nvenc\" -D__PLUGIN__ \
              -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE
  LDFLAGS  := -shared -static-libgcc
  LIBS     := "$(VLC_DLL)" -ld3d11 -ldxgi -lole32 -luuid
else
  OUT      := build/libnvenc_plugin.so
  CFLAGS   += -fPIC -fvisibility=hidden $(shell pkg-config --cflags vlc-plugin) \
              -Ithird_party/nv-codec-headers/include \
              -DMODULE_STRING=\"nvenc\" -D__PLUGIN__ -D_FILE_OFFSET_BITS=64
  LDFLAGS  := -shared -Wl,-no-undefined
  LIBS     := $(shell pkg-config --libs vlc-plugin) -ldl
endif

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC) | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LIBS)

ifeq ($(OS),Windows_NT)
build:
	if not exist build mkdir build
else
build:
	mkdir -p build
endif

clean:
	$(RM) $(OUT)
