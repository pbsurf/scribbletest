# nvgtest - headless nanovgXC render test and benchmark (see nvgtest.c)
# make [NVGSRC=<nanovgXC src dir>] [BUILDDIR=<dir>] [DEBUG=1] [GLES=1 | ANDROID=1]
# - NVGSRC selects the nanovgXC sources under test (ab.sh uses this to build a baseline); everything else (glad,
#  stb, font) always comes from the working tree nanovgXC
# - use a separate BUILDDIR for each NVGSRC and platform
# - GLES=1: GLES 3 build for this machine (run w/ EGL_PLATFORM=surfaceless if there's no display)
# - ANDROID=1: GLES 3 native executable for arm64 Android (run via adb shell, see ab.sh -a); needs NDK
#   (ANDROID_NDK, default: newest under ~/android-sdk/ndk)

NVGXC := $(abspath ../../nanovgXC)
NVGSRC ?= $(NVGXC)/src
DEBUG ?= 0
GLES ?= 0
ANDROID ?= 0
ifneq ($(DEBUG), 0)
  OPT = -g -O0
  BUILDTYPE = Debug
else
  OPT = -g -O2
  BUILDTYPE = Release
endif

CC = gcc
DEFS = -DNVGTEST_FONT='"$(NVGXC)/example/fonts/Roboto-Regular.ttf"'
LIBS = -lEGL -ldl -lm
GLAD = $(NVGXC)/glad/glad.c
ifneq ($(ANDROID), 0)
  ANDROID_NDK ?= $(lastword $(sort $(wildcard $(HOME)/android-sdk/ndk/*)))
  CC = $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang
  # font is pushed alongside the executable by ab.sh
  DEFS = -DNVGTEST_GLES -DNVGTEST_FONT='"/data/local/tmp/nvgtest/Roboto-Regular.ttf"'
  LIBS = -lEGL -lGLESv3 -lm
  GLAD =
  BUILDDIR ?= Android$(BUILDTYPE)
  # clang-only warnings for code that is fine (NVG_LOG of shader source, arg parsing loop)
  WARN_EXTRA = -Wno-format-security -Wno-for-loop-analysis
else ifneq ($(GLES), 0)
  DEFS += -DNVGTEST_GLES
  LIBS = -lEGL -lGLESv2 -lm
  GLAD =
  BUILDDIR ?= GLES$(BUILDTYPE)
endif
BUILDDIR ?= $(BUILDTYPE)

TARGET = $(BUILDDIR)/nvgtest
CFLAGS = $(OPT) -std=gnu99 -MMD -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable $(WARN_EXTRA)
CPPFLAGS = -I$(NVGSRC) -I$(NVGXC)/glad -isystem $(NVGXC)/example/stb $(DEFS)

OBJS = $(BUILDDIR)/nvgtest.o $(BUILDDIR)/nanovg.o $(if $(GLAD),$(BUILDDIR)/glad.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

$(BUILDDIR)/nvgtest.o: nvgtest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR)/nanovg.o: $(NVGSRC)/nanovg.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR)/glad.o: $(NVGXC)/glad/glad.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean

-include $(OBJS:.o=.d)
