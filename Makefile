BUILD_DIR ?= build
PREFIX ?= /usr
CMAKE ?= /usr/bin/cmake
APT_PACKAGES ?= build-essential cmake pkg-config obs-studio libsimde-dev
OBS_USER_PLUGIN_DIR ?= $(HOME)/.config/obs-studio/plugins/audio-lightbar/bin/64bit
CMAKE_ARGS ?=

ifdef SIMDE_INCLUDE_DIR
CMAKE_ARGS += -DSIMDE_INCLUDE_DIR=$(SIMDE_INCLUDE_DIR)
endif

.DEFAULT_GOAL := build

.PHONY: help deps configure build install install-user install-system clean

help:
	@printf '%s\n' \
		'Targets:' \
		'  make deps           Install Debian/Ubuntu/Pop!_OS build dependencies' \
		'  make build          Configure and build build/audio-lightbar.so' \
		'  make install        Build and install to the current OBS user plugin directory' \
		'  make install-user   Install to the current OBS user plugin directory' \
		'  make install-system Install system-wide with sudo cmake --install' \
		'  make clean          Remove the build directory'

deps:
	sudo apt-get update
	sudo apt-get install -y $(APT_PACKAGES)

configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" $(CMAKE_ARGS)

build: configure
	$(CMAKE) --build "$(BUILD_DIR)"

install: install-user

install-user: build
	install -Dm755 "$(BUILD_DIR)/audio-lightbar.so" "$(OBS_USER_PLUGIN_DIR)/audio-lightbar.so"

install-system: build
	sudo $(CMAKE) --install "$(BUILD_DIR)" --prefix "$(PREFIX)"

clean:
	$(CMAKE) -E rm -rf "$(BUILD_DIR)"
