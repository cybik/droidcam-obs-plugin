# Copyright (C) 2022 DEV47APPS, github.com/dev47apps

DATA_DIR  ?= data
BUILD_DIR ?= build
LIB_DLL   ?= $(BUILD_DIR)/droidcam-obs.so

RM       ?= rm -f
CXX      ?= g++
CXXFLAGS += -std=c++17 -Wall -fPIC -g
INCLUDES += -Isrc/
STATIC   +=
LDD_DIRS +=
LDD_LIBS +=
LDD_FLAG +=
SRC      += $(shell ls src/*.cc src/sys/unix/*.cc)

DEBUGGABLE  ?= yes

.PHONY: run clean

ifeq "$(DEBUGGABLE)" "yes"
CXXFLAGS += -g
endif

all: $(BUILD_DIR) $(LIB_DLL)
debug: CXXFLAGS += -DDEBUG
debug: all

UNAME := $(shell uname -s)

ifeq ($(UNAME),Linux)
include linux/linux.mk

run: debug
	obs
endif


ifeq ($(UNAME),Darwin)
include macos/macOS.mk

run: debug
	rm ~/Library/ApplicationSupport/obs-studio/logs/* && PATH='' /Applications/OBS.app/Contents/MacOS/OBS
endif


$(BUILD_DIR):
	@[ -d "$(BUILD_DIR)" ] || {\
		echo missing build directory: $(BUILD_DIR);\
		echo use: mkdir $(BUILD_DIR);\
		exit 1;\
	}

$(LIB_DLL): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDD_DIRS) $(LDD_LIBS) $(LDD_FLAG) $^ $(STATIC) -o $@

clean:
	$(RM) $(BUILD_DIR)/*.o $(BUILD_DIR)/*.so $(BUILD_DIR)/*.exe

adbz:
	$(CXX) $(CXXFLAGS) -o$(BUILD_DIR)/adbz.exe src/test/adbz.c

test: adbz
	$(CXX) $(CXXFLAGS) -o$(BUILD_DIR)/test.exe -DDEBUG -DTEST -Isrc/test/ $(INCLUDES) \
		src/net.c src/command.c src/sys/unix/cmd.c \
		src/test/main.c
	$(BUILD_DIR)/test.exe
