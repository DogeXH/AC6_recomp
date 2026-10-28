#!/bin/sh
# AC6 Recomp Online (Linux, Steam Deck): start the game through its bundled glibc 2.44.
# The game reads ac6recomp.toml and finds the .iso and assets/ in the folder of the program the
# system started, which is the bundled loader beside this script: keep everything in this folder.
here="$(cd "$(dirname "$0")" && pwd)" || exit 1
cd "$here" || exit 1

# Steam adds these for non-Steam games; they are built for the system's libraries or hide the
# Deck's controller from the game (same list as the launcher the Deck tests used).
unset LD_PRELOAD LD_LIBRARY_PATH ENABLE_VK_LAYER_VALVE_steam_overlay_1 \
  ENABLE_VK_LAYER_VALVE_steam_fossilize_1 SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT \
  SDL_GAMECONTROLLER_IGNORE_DEVICES SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD
export DISABLE_VULKAN_RENDERDOC_CAPTURE_1_36=1 VK_LOADER_LAYERS_DISABLE=VK_LAYER_RENDERDOC_Capture

exec "$here/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/ac6recomp" "$@"
