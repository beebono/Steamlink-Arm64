#!/bin/bash

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

# Variables
GAMEDIR="/$directory/ports/steamlink"

# CD and set log
cd $GAMEDIR
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# Check if we need to download Steamlink
if [ "$DEVICE_ARCH" == "aarch64" ]; then
    LIBARCH="/usr/lib/"
    CDN_URL="http://cdn.origin.steamstatic.com/steamlink/rpi/bookworm/arm64/public_build.txt"
elif [ "$DEVICE_ARCH" == "armhf" ]; then
    LIBARCH="/usr/lib32/"
    echo "Armhf support is not yet implemented."
    exit 1
    #CDN_URL="http://cdn.origin.steamstatic.com/steamlink/rpi/bullseye/arm64/public_build.txt"
else
    echo "Unable to determine architecture!"
fi

# If fetching build info fails, check if we have an existing shell binary
if [[ -z "$CDN_URL" ]]; then
    if [[ -f "$GAMEDIR/bin/shell" ]]; then
        echo "No internet connection. Skipping update check."
    else
        echo "SteamLink requires an internet connection to download and use!"
        exit 1
    fi
else
    source download
fi

# Exports post-setup
QT_VERSION=$(ls -d $GAMEDIR/Qt-* 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$GAMEDIR/libs.${DEVICE_ARCH}/shell:$GAMEDIR/Qt-${QT_VERSION}/lib:$LD_LIBRARY_PATH"
export LD_PRELOAD="$GAMEDIR/libs.${DEVICE_ARCH}/libgpucompat.so"
export QT_QPA_PLATFORM_PLUGIN_PATH="$GAMEDIR/Qt-${QT_VERSION}/plugins"
export QT_QPA_PLATFORM="xcb"
export SDL_VIDEO_DRIVER="x11"

# Assign gptokeyb and load the game
pm_platform_helper "$GAMEDIR/bin/shell.${DEVICE_ARCH} " >/dev/null
./"bin/shell.${DEVICE_ARCH}"

# Cleanup
pm_finish
