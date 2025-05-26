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

# Set config
if [ ! -f "$GAMEDIR/config/SteamLink.conf" ]; then
    bind_directories ~/".config/Valve Corporation" "$GAMEDIR/config"
fi

# Patcher GUI exports
export PATCHER_FILE="$GAMEDIR/config/download"
export PATCHER_GAME="$(basename "${0%.*}")" # This gets the current script filename without the extension
export PATCHER_TIME="a few minutes"
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$GAMEDIR/libs.${DEVICE_ARCH}"

# CD and set log
cd $GAMEDIR
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

run_patcher() {
    if [ -f "$controlfolder/utils/patcher.txt" ]; then
        source "$controlfolder/utils/patcher.txt"
        $ESUDO kill -9 $(pidof gptokeyb)
    else
        echo "This port requires the latest version of PortMaster."
    fi
}

# Check if we need to download Steamlink
if [ "$DEVICE_ARCH" == "aarch64" ]; then
    LIBARCH="/usr/lib/"
    CDN_URL="http://cdn.origin.steamstatic.com/steamlink/rpi/bookworm/arm64/public_build.txt"
    CDN_TXT=$(curl -s "$CDN_URL")
    PACKAGE_URL=$(echo "$CDN_TXT" | grep -o 'https://[^[:space:]]*')
    PACKAGE_VERSION=$(echo "$PACKAGE_URL" | grep 'steamlink-rpi-bookworm-arm64-[0-9]\+\.[0-9]\+\.[0-9]\+\(\.[0-9]\+\)\?' | sed -n 's/.*steamlink-rpi-bookworm-arm64-\([0-9.]*\).*/\1/p')
elif [ "$DEVICE_ARCH" == "armhf" ]; then
    LIBARCH="/usr/lib32/"
    echo "Armhf support is not yet implemented."
    exit 1
    #CDN_URL="http://cdn.origin.steamstatic.com/steamlink/rpi/bullseye/arm64/public_build.txt"
    #CDN_TXT=$(curl -s "$CDN_URL")
    #PACKAGE_URL=$(echo "$CDN_TXT" | grep -oP '(?<=https://)[^\s]*')
    #PACKAGE_VERSION=$(echo "$PACKAGE_URL" | grep -oP 'steamlink-rpi-bullseye-arm64-([0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?)' | cut -d'-' -f4)
else
    pm_message "Unable to determine architecture!"
fi

# If fetching build info fails, check if we have an existing shell binary
if [[ -z "$CDN_TXT" ]]; then
    if [[ -f "$GAMEDIR/bin/shell" ]]; then
        pm_message "No internet connection. Skipping update check."
    else
        pm_message "SteamLink requires an internet connection to download and use!"
        exit 1
    fi
# If we have an internet connection check the current version
elif [[ -f "$GAMEDIR/bin/version_${DEVICE_ARCH}.txt" ]]; then
    CURRENT_VERSION=$(grep 'steamlink-rpi-bookworm-arm64-[0-9]\+\.[0-9]\+\.[0-9]\+\(\.[0-9]\+\)\?' "$GAMEDIR/bin/version_${DEVICE_ARCH}.txt" | sed -n 's/.*steamlink-rpi-bookworm-arm64-\([0-9.]*\).*/\1/p')
    if [[ "$CURRENT_VERSION" != "$PACKAGE_VERSION" ]]; then
        run_patcher
    fi
else
    run_patcher
fi

# Mount Weston runtime
weston_dir=/tmp/weston
$ESUDO mkdir -p "${weston_dir}"
weston_runtime="weston_pkg_0.2"
if [ ! -f "$controlfolder/libs/${weston_runtime}.squashfs" ]; then
  if [ ! -f "$controlfolder/harbourmaster" ]; then
    pm_message "This port requires the latest PortMaster to run, please go to https://portmaster.games/ for more info."
    sleep 5
    exit 1
  fi
  $ESUDO $controlfolder/harbourmaster --quiet --no-check runtime_check "${weston_runtime}.squashfs"
fi
if [[ "$PM_CAN_MOUNT" != "N" ]]; then
    $ESUDO umount "${weston_dir}"
fi
$ESUDO mount "$controlfolder/libs/${weston_runtime}.squashfs" "${weston_dir}"

# Exports post-setup
QT_VERSION=$(ls -d $GAMEDIR/Qt-* 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
game_libs="$LD_LIBRARY_PATH:$GAMEDIR/Qt-${QT_VERSION}/lib"
game_preload="$GAMEDIR/libs.${DEVICE_ARCH}/libhandecoder.so"
game_executable="./"bin/shell.${DEVICE_ARCH}""
export QT_QPA_PLATFORM="eglfs"
export SDL_VIDEO_DRIVER="x11"
export SDL_VIDEO_FORCE_EGL="1"
export HDCD_RESOLUTION="${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}"

# Start the game
pm_platform_helper "$GAMEDIR/bin/shell.${DEVICE_ARCH} " >/dev/null
$ESUDO env WRAPPED_LIBRARY_PATH=$game_libs WRAPPED_PRELOAD=$game_preload \
$weston_dir/westonwrap.sh headless noop kiosk crusty_x11egl \
$GAMEDIR/$game_executable

# Cleanup
$ESUDO $weston_dir/westonwrap.sh cleanup
if [[ "$PM_CAN_MOUNT" != "N" ]]; then
    $ESUDO umount "${weston_dir}"
fi
pm_finish
