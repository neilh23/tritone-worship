# PLATFORM is linux, windows or macos, detected from the build machine.
# make PLATFORM=windows on Linux cross-compiles with MinGW-w64; make cross-windows does the same
# in a container, so no compiler needs installing.
ifeq ($(OS),Windows_NT)
PLATFORM ?= windows
else ifeq ($(shell uname -s),Darwin)
PLATFORM ?= macos
else
PLATFORM ?= linux
endif

CXX ?= c++
CFLAGS ?= -O2 -Wall
CXXFLAGS ?= -O2 -Wall
CFLAGS += -Isrc -Ithird_party

# make EQ_TUNING=1 exposes the 20 Blacken EQ bands as controls
ifdef EQ_TUNING
CFLAGS += -DTW_EQ_TUNING
CXXFLAGS += -DTW_EQ_TUNING
BUILD = build/$(PLATFORM)-eq-tuning
EQ_SEE_ALSO = -e 's|<tritone_worship.ttl> .|<tritone_worship.ttl>, <eq_tuning.ttl> .|'
else
BUILD = build/$(PLATFORM)
endif

BUNDLE = $(BUILD)/tritone_worship.lv2
VST3_BUNDLE = $(BUILD)/tritone_worship.vst3
CLAP_BUNDLE = $(BUILD)/tritone_worship.clap

ifeq ($(PLATFORM),linux)
CFLAGS += -fPIC
SHARED = -shared -fvisibility=hidden
SO = .so
GUI_SRC = src/tw_gui_x11.c
GUI_LIBS = -lX11
TEST_LIBS = -lX11 -ldl
CLAP_PLUGIN = $(CLAP_BUNDLE)
VST3_PLUGIN = $(VST3_BUNDLE)/Contents/x86_64-linux/tritone_worship.so
LV2_DIR ?= $(HOME)/.lv2
CLAP_DIR ?= $(HOME)/.clap
VST3_DIR ?= $(HOME)/.vst3
endif

ifeq ($(PLATFORM),windows)
ifeq ($(OS),Windows_NT)
ifeq ($(origin CC),default)
CC = gcc
endif
else
ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif
# Tests run under Wine in a prefix of their own; no Mono or Gecko needed
RUN ?= env WINEPREFIX="$(CURDIR)/build/wine" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" wine
endif
SHARED = -shared -static-libgcc
SO = .dll
EXE = .exe
GUI_SRC = src/tw_gui_win32.c
GUI_LIBS = -lgdi32 -luser32
TEST_LIBS = -static-libgcc -luser32
CLAP_PLUGIN = $(CLAP_BUNDLE)
VST3_PLUGIN = $(VST3_BUNDLE)/Contents/x86_64-win/tritone_worship.vst3
LOCAL_APP_DATA = $(subst \,/,$(LOCALAPPDATA))
LV2_DIR ?= $(subst \,/,$(APPDATA))/LV2
CLAP_DIR ?= $(LOCAL_APP_DATA)/Programs/Common/CLAP
VST3_DIR ?= $(LOCAL_APP_DATA)/Programs/Common/VST3
endif

ifeq ($(PLATFORM),macos)
# Universal binaries for Intel and Apple Silicon
CFLAGS += -fPIC -arch x86_64 -arch arm64 -mmacosx-version-min=10.13
SHARED = -bundle -fvisibility=hidden
SO = .dylib
GUI_SRC = src/tw_gui_cocoa.m
GUI_LIBS = -fobjc-arc -framework Cocoa
CLAP_PLUGIN = $(CLAP_BUNDLE)/Contents/MacOS/tritone_worship
VST3_PLUGIN = $(VST3_BUNDLE)/Contents/MacOS/tritone_worship
LV2_DIR ?= $(HOME)/Library/Audio/Plug-Ins/LV2
CLAP_DIR ?= $(HOME)/Library/Audio/Plug-Ins/CLAP
VST3_DIR ?= $(HOME)/Library/Audio/Plug-Ins/VST3
# $(1): bundle directory, $(2): bundle identifier. Ad-hoc signed so Apple Silicon will load it.
mac_bundle = sed 's|@IDENTIFIER@|$(2)|' macos/Info.plist.in > $(1)/Contents/Info.plist && \
	codesign --force --sign - $(1)
endif

LMMS_PLUGIN = $(BUILD)/libtritoneworship.so
LMMS_PLUGIN_DIR ?= $(HOME)/lmms/plugins
QT = third_party/qt5/include
LMMS_CXXFLAGS = -fPIC -std=c++14 -fvisibility=hidden -DQT_NO_VERSION_TAGGING \
	-Isrc -Isrc/lmms -Ithird_party/lmms/include \
	-I$(QT) -I$(QT)/QtCore -I$(QT)/QtGui -I$(QT)/QtWidgets -I$(QT)/QtXml

DSP = src/tw_dsp.c src/tw_dsp.h
UI = src/tw_ui.c src/tw_ui.h
GUI = $(GUI_SRC) src/tw_gui.h

all: lv2 clap vst3

lv2: $(BUNDLE)/tritone_worship$(SO) $(BUNDLE)/manifest.ttl $(BUNDLE)/tritone_worship.ttl \
	$(if $(EQ_TUNING),$(BUNDLE)/eq_tuning.ttl)

$(BUNDLE)/tritone_worship$(SO): src/tw_lv2.c $(DSP)
	mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) $(subst -bundle,-dynamiclib,$(SHARED)) -o $@ src/tw_lv2.c src/tw_dsp.c -lm

$(BUNDLE)/manifest.ttl: lv2/tritone_worship.lv2/manifest.ttl
	mkdir -p $(BUNDLE)
	sed -e 's|<tritone_worship.so>|<tritone_worship$(SO)>|' $(EQ_SEE_ALSO) $< > $@

$(BUNDLE)/%.ttl: lv2/tritone_worship.lv2/%.ttl
	mkdir -p $(BUNDLE)
	cp $< $@

clap: $(CLAP_PLUGIN)

$(CLAP_PLUGIN): src/tw_clap.c $(GUI) $(UI) $(DSP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/clap -DTW_COCOA_VIEW=TritoneWorshipClapView $(SHARED) -o $@ \
		src/tw_clap.c $(GUI_SRC) src/tw_ui.c src/tw_dsp.c $(GUI_LIBS) -lm
	$(call mac_bundle,$(CLAP_BUNDLE),org.tritoneworship.tritoneworship.clap)

vst3: $(VST3_PLUGIN)

$(VST3_PLUGIN): src/tw_vst3.c $(GUI) $(UI) $(DSP)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/vst3 -DTW_COCOA_VIEW=TritoneWorshipVst3View $(SHARED) -o $@ \
		src/tw_vst3.c $(GUI_SRC) src/tw_ui.c src/tw_dsp.c $(GUI_LIBS) -lm
	$(call mac_bundle,$(VST3_BUNDLE),org.tritoneworship.tritoneworship.vst3)

# The native LMMS plugin is Linux only
lmms: $(LMMS_PLUGIN)

third_party/lmms/include/Effect.h:
	./tools/fetch_lmms_deps.sh

$(LMMS_PLUGIN): src/lmms/tritone_worship_lmms.cpp $(DSP) $(UI) third_party/lmms/include/Effect.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fvisibility=hidden -c -o $(BUILD)/tw_dsp.o src/tw_dsp.c
	$(CC) $(CFLAGS) -fvisibility=hidden -c -o $(BUILD)/tw_ui.o src/tw_ui.c
	$(CXX) $(CXXFLAGS) $(LMMS_CXXFLAGS) -shared -o $@ src/lmms/tritone_worship_lmms.cpp \
		$(BUILD)/tw_dsp.o $(BUILD)/tw_ui.o -lm

# ---- tests ----

TESTS = $(BUILD)/test_dsp$(EXE) $(BUILD)/test_ui$(EXE) $(BUILD)/test_clap$(EXE) $(BUILD)/test_vst3$(EXE)

test-programs: $(TESTS)

$(BUILD)/test_dsp$(EXE): test/test_dsp.c $(DSP)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/test_dsp.c src/tw_dsp.c -lm

$(BUILD)/test_ui$(EXE): test/test_ui.c $(UI) $(DSP)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/test_ui.c src/tw_ui.c src/tw_dsp.c -lm

$(BUILD)/test_clap$(EXE): test/test_clap.c test/test_host.h $(UI) $(DSP)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ithird_party/clap -o $@ test/test_clap.c src/tw_ui.c src/tw_dsp.c $(TEST_LIBS) -lm

$(BUILD)/test_vst3$(EXE): test/test_vst3.c test/test_host.h $(UI) $(DSP) src/tw_gui.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ithird_party/vst3 -o $@ test/test_vst3.c src/tw_ui.c src/tw_dsp.c $(TEST_LIBS) -lm

test: all test-programs
	$(MAKE) run-tests

# Runs already-built tests: under Wine when they were cross-compiled for Windows
run-tests:
	$(RUN) ./$(BUILD)/test_dsp$(EXE)
	$(RUN) ./$(BUILD)/test_ui$(EXE)
	$(RUN) ./$(BUILD)/test_clap$(EXE) $(CLAP_PLUGIN)
	$(RUN) ./$(BUILD)/test_vst3$(EXE) $(VST3_PLUGIN)

# ---- Windows cross-compile in a container ----

CONTAINER ?= $(shell command -v podman || command -v docker)
MINGW_IMAGE = tritone-worship-mingw
ifneq ($(findstring podman,$(CONTAINER)),)
CONTAINER_USER = --userns=keep-id
else
CONTAINER_USER = --user $(shell id -u):$(shell id -g)
endif

cross-windows:
	$(CONTAINER) build -q -t $(MINGW_IMAGE) -f tools/mingw.Containerfile tools
	$(CONTAINER) run --rm $(CONTAINER_USER) -v "$(CURDIR)":/src:Z -w /src $(MINGW_IMAGE) \
		make PLATFORM=windows all test-programs
	if command -v wine >/dev/null; then $(MAKE) PLATFORM=windows run-tests; \
	else echo "wine not found: Windows tests not run"; fi

# ---- release binaries, built on Linux ----

binaries:
	$(MAKE) PLATFORM=linux test lmms
	$(MAKE) cross-windows
	rm -rf binaries
	mkdir -p binaries/linux-x86_64 binaries/windows-x86_64
	cp -r build/linux/tritone_worship.lv2 build/linux/tritone_worship.clap build/linux/tritone_worship.vst3 \
		build/linux/libtritoneworship.so binaries/linux-x86_64/
	cp -r build/windows/tritone_worship.lv2 build/windows/tritone_worship.clap build/windows/tritone_worship.vst3 \
		binaries/windows-x86_64/
	cd binaries && zip -qr tritone-worship-linux-x86_64.zip linux-x86_64 && \
		zip -qr tritone-worship-windows-x86_64.zip windows-x86_64

screenshot: docs/screenshot.png

docs/screenshot.png: tools/screenshot.c $(UI) $(DSP)
	mkdir -p $(BUILD) docs
	$(CC) $(CFLAGS) -o $(BUILD)/screenshot tools/screenshot.c src/tw_ui.c src/tw_dsp.c -lz -lm
	./$(BUILD)/screenshot $@

# ---- install ----

install: install-lmms

install-lv2: lv2
	mkdir -p "$(LV2_DIR)"
	rm -rf "$(LV2_DIR)/tritone_worship.lv2"
	cp -r $(BUNDLE) "$(LV2_DIR)/"

install-lmms: lmms
	mkdir -p $(LMMS_PLUGIN_DIR)
	cp $(LMMS_PLUGIN) $(LMMS_PLUGIN_DIR)/

install-clap: clap
	mkdir -p "$(CLAP_DIR)"
	rm -rf "$(CLAP_DIR)/tritone_worship.clap"
	cp -r $(CLAP_BUNDLE) "$(CLAP_DIR)/"

install-vst3: vst3
	mkdir -p "$(VST3_DIR)"
	rm -rf "$(VST3_DIR)/tritone_worship.vst3"
	cp -r $(VST3_BUNDLE) "$(VST3_DIR)/"

clean:
	rm -rf build

.PHONY: all lv2 lmms clap vst3 test test-programs run-tests cross-windows binaries screenshot \
	install install-lv2 install-lmms install-clap install-vst3 clean
