# hypr-awesome — top-level Makefile
#
# Targets:
#   make          → build the plugin .so
#   make reload   → build + live-reload into the running Hyprland session
#   make clean    → remove build artefacts

PLUGIN_SO = plugin/build/hawesome.so

.PHONY: all build reload clean

all: build

build:
	$(MAKE) -C plugin/src

reload: build
	@echo "Unloading old hawesome plugin (if loaded)..."
	-hyprctl plugin unload $(CURDIR)/$(PLUGIN_SO) 2>/dev/null || true
	@echo "Loading new build..."
	hyprctl plugin load $(CURDIR)/$(PLUGIN_SO)
	@echo "Done. Verify with: hyprctl plugins list"

clean:
	$(MAKE) -C plugin/src clean
