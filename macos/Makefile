# ============================================================================
# DF-OpenVCable macOS Build System
# ----------------------------------------------------------------------------
# Builds without Xcode.app (requires only macOS Command Line Tools):
#   - build/DF-OpenVCable.driver              (Universal arm64 + x86_64 HAL Plug-In)
#   - build/DF-OpenVCable Control Panel.app   (Native SwiftUI Control Panel)
#   - build/dfovc-ctl                         (Native CLI + 1kHz Loopback Tester)
#   - build/test_ring_buffer                  (Standalone C++ Ring Buffer Unit Tests)
# ============================================================================

CXX          := clang++
SWIFTC       := swiftc
LIPO         := lipo
CODESIGN     := codesign
SIGN_ID      ?= -
MIN_MACOS    := 13.0
VERSION      := 1.0.0

BUILD_DIR    := build
DRIVER_NAME  := DF-OpenVCable.driver
DRIVER_DIR   := $(BUILD_DIR)/$(DRIVER_NAME)
DRIVER_BIN   := $(DRIVER_DIR)/Contents/MacOS/DF-OpenVCable

APP_NAME     := DF-OpenVCable Control Panel.app
APP_DIR      := $(BUILD_DIR)/$(APP_NAME)
APP_BIN      := $(APP_DIR)/Contents/MacOS/DFOpenVCableControlPanel
APP_STAMP    := $(BUILD_DIR)/.app.stamp
MOD_CACHE    := $(BUILD_DIR)/module-cache

CLI_BIN      := $(BUILD_DIR)/dfovc-ctl
TEST_BIN     := $(BUILD_DIR)/test_ring_buffer
PLUGIN_TEST  := $(BUILD_DIR)/test_driver_plugin

CXXFLAGS_COMMON := -std=c++17 -O2 -Wall -Wextra -Wpedantic -fvisibility=hidden -mmacosx-version-min=$(MIN_MACOS)
SWIFTFLAGS      := -O -module-cache-path "$(MOD_CACHE)"
DRIVER_LDFLAGS  := -bundle -framework CoreAudio -framework CoreFoundation

# Set UNIVERSAL=0 to build only host architecture for faster local iteration
UNIVERSAL ?= 1
HOST_ARCH := $(shell uname -m)

.PHONY: all driver app cli test package clean install uninstall reload selftest

all: driver app cli

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

driver: $(DRIVER_BIN)

$(DRIVER_BIN): driver/DFOpenVCableDriver.cpp driver/ring_buffer.h driver/dfovc_protocol.h driver/Info.plist | $(BUILD_DIR)
	@echo "==> Building $(DRIVER_NAME)..."
	@mkdir -p "$(DRIVER_DIR)/Contents/MacOS"
	@cp driver/Info.plist "$(DRIVER_DIR)/Contents/Info.plist"
ifeq ($(UNIVERSAL),1)
	$(CXX) $(CXXFLAGS_COMMON) -arch arm64  driver/DFOpenVCableDriver.cpp $(DRIVER_LDFLAGS) -o "$(BUILD_DIR)/DF-OpenVCable.arm64"
	$(CXX) $(CXXFLAGS_COMMON) -arch x86_64 driver/DFOpenVCableDriver.cpp $(DRIVER_LDFLAGS) -o "$(BUILD_DIR)/DF-OpenVCable.x86_64"
	$(LIPO) -create "$(BUILD_DIR)/DF-OpenVCable.arm64" "$(BUILD_DIR)/DF-OpenVCable.x86_64" -output "$(DRIVER_BIN)"
	@rm -f "$(BUILD_DIR)/DF-OpenVCable.arm64" "$(BUILD_DIR)/DF-OpenVCable.x86_64"
else
	$(CXX) $(CXXFLAGS_COMMON) -arch $(HOST_ARCH) driver/DFOpenVCableDriver.cpp $(DRIVER_LDFLAGS) -o "$(DRIVER_BIN)"
endif
	$(CODESIGN) --force --deep --sign "$(SIGN_ID)" "$(DRIVER_DIR)"
	@echo "    Built & signed: $(DRIVER_DIR)"

app: $(APP_STAMP)

$(APP_STAMP): control_panel/DFOpenVCableControlPanel.swift control_panel/Info.plist | $(BUILD_DIR)
	@echo "==> Building $(APP_NAME)..."
	@mkdir -p "$(APP_DIR)/Contents/MacOS" "$(MOD_CACHE)"
	@cp control_panel/Info.plist "$(APP_DIR)/Contents/Info.plist"
ifeq ($(UNIVERSAL),1)
	$(SWIFTC) $(SWIFTFLAGS) -parse-as-library -target arm64-apple-macos$(MIN_MACOS)  control_panel/DFOpenVCableControlPanel.swift -framework SwiftUI -framework AppKit -framework CoreAudio -o "$(BUILD_DIR)/DFOpenVCableControlPanel.arm64"
	$(SWIFTC) $(SWIFTFLAGS) -parse-as-library -target x86_64-apple-macos$(MIN_MACOS) control_panel/DFOpenVCableControlPanel.swift -framework SwiftUI -framework AppKit -framework CoreAudio -o "$(BUILD_DIR)/DFOpenVCableControlPanel.x86_64"
	$(LIPO) -create "$(BUILD_DIR)/DFOpenVCableControlPanel.arm64" "$(BUILD_DIR)/DFOpenVCableControlPanel.x86_64" -output "$(APP_BIN)"
	@rm -f "$(BUILD_DIR)/DFOpenVCableControlPanel.arm64" "$(BUILD_DIR)/DFOpenVCableControlPanel.x86_64"
else
	$(SWIFTC) $(SWIFTFLAGS) -parse-as-library -target $(HOST_ARCH)-apple-macos$(MIN_MACOS) control_panel/DFOpenVCableControlPanel.swift -framework SwiftUI -framework AppKit -framework CoreAudio -o "$(APP_BIN)"
endif
	$(CODESIGN) --force --deep --sign "$(SIGN_ID)" "$(APP_DIR)"
	@touch "$(APP_STAMP)"
	@echo "    Built & signed: $(APP_DIR)"

cli: $(CLI_BIN)

$(CLI_BIN): cli/dfovc_ctl.swift | $(BUILD_DIR)
	@echo "==> Building dfovc-ctl..."
	@mkdir -p "$(MOD_CACHE)"
ifeq ($(UNIVERSAL),1)
	$(SWIFTC) $(SWIFTFLAGS) -target arm64-apple-macos$(MIN_MACOS)  cli/dfovc_ctl.swift -framework CoreAudio -framework Foundation -o "$(BUILD_DIR)/dfovc-ctl.arm64"
	$(SWIFTC) $(SWIFTFLAGS) -target x86_64-apple-macos$(MIN_MACOS) cli/dfovc_ctl.swift -framework CoreAudio -framework Foundation -o "$(BUILD_DIR)/dfovc-ctl.x86_64"
	$(LIPO) -create "$(BUILD_DIR)/dfovc-ctl.arm64" "$(BUILD_DIR)/dfovc-ctl.x86_64" -output "$(CLI_BIN)"
	@rm -f "$(BUILD_DIR)/dfovc-ctl.arm64" "$(BUILD_DIR)/dfovc-ctl.x86_64"
else
	$(SWIFTC) $(SWIFTFLAGS) -target $(HOST_ARCH)-apple-macos$(MIN_MACOS) cli/dfovc_ctl.swift -framework CoreAudio -framework Foundation -o "$(CLI_BIN)"
endif
	$(CODESIGN) --force --sign "$(SIGN_ID)" "$(CLI_BIN)"
	@echo "    Built & signed: $(CLI_BIN)"

test: $(TEST_BIN) $(PLUGIN_TEST) driver
	@echo "==> Running ring buffer unit tests..."
	@./$(TEST_BIN)
	@echo "==> Running AudioServerPlugIn integration tests..."
	@./$(PLUGIN_TEST) "$(DRIVER_BIN)"

$(TEST_BIN): tests/test_ring_buffer.cpp driver/ring_buffer.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_COMMON) tests/test_ring_buffer.cpp -o "$(TEST_BIN)"

$(PLUGIN_TEST): tests/test_driver_plugin.cpp driver/dfovc_protocol.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_COMMON) tests/test_driver_plugin.cpp -framework CoreFoundation -o "$(PLUGIN_TEST)"

package: all test
	@echo "==> Packaging macOS release bundle..."
	@rm -rf "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal" "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal.zip"
	@mkdir -p "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal/scripts"
	@cp -R "$(DRIVER_DIR)" "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal/"
	@cp -R "$(APP_DIR)" "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal/"
	@cp "$(CLI_BIN)" "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal/"
	@cp scripts/*.sh "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal/scripts/"
	@chmod +x "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal/scripts/"*.sh
	@cp ../README.md ../docs/VALIDATION_MACOS.md "$(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal/"
	@cd "$(BUILD_DIR)" && ditto -c -k --sequesterRsrc --keepParent "DF-OpenVCable-v$(VERSION)-macOS-universal" "DF-OpenVCable-v$(VERSION)-macOS-universal.zip"
	@echo "    Created: $(BUILD_DIR)/DF-OpenVCable-v$(VERSION)-macOS-universal.zip"

install: all
	@./scripts/install.sh

uninstall:
	@./scripts/uninstall.sh

reload:
	@./scripts/reload.sh

selftest:
	@./scripts/selftest.sh

clean:
	@rm -rf "$(BUILD_DIR)"
