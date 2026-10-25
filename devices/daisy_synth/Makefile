# daisy_synth firmware (libDaisy). Built by ./build.sh; runs from internal flash (BOOT_NONE).
TARGET = daisy_synth

AIRMIDI_LIB = ../../lib
CPP_SOURCES = src/main.cpp engine/controls.cpp engine/synth.cpp engine/fx.cpp engine/device.cpp
C_SOURCES = $(AIRMIDI_LIB)/src/midici.c $(AIRMIDI_LIB)/src/midici_responder.c $(AIRMIDI_LIB)/src/json_scan.c
C_INCLUDES = -I$(AIRMIDI_LIB)/include
ifdef WHINE_TEST
C_DEFS = -DWHINE_TEST # the 1 kHz whine comparison in main.cpp
endif

LIBDAISY_DIR ?= $(HOME)/Developer/libDaisy
CPP_STANDARD = -std=gnu++17
OPT = -Os # 128 KB of internal flash, libDaisy takes most of it

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
