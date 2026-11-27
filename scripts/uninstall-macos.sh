#!/usr/bin/env bash
#
# De-Feedback – macOS uninstaller.
#
#   ./scripts/uninstall-macos.sh            # remove the user installation
#   ./scripts/uninstall-macos.sh --system   # remove the /Library installation
#   ./scripts/uninstall-macos.sh --all      # remove both
#
# The trained AI model under ~/Library/Application Support/ResonanceAI/ is
# user data and is deliberately kept. Delete it manually if you want it gone.

set -euo pipefail

PLUGIN_NAME="De-Feedback"

SYSTEM_WIDE=0
REMOVE_ALL=0
KEEP_VST3=0

usage() {
    cat <<EOF
De-Feedback von macOS entfernen

Verwendung:
  $(basename "$0") [Optionen]

Optionen:
  --system      Nur die systemweite Installation in /Library entfernen (sudo)
  --all         Benutzer- und Systeminstallation entfernen
  --keep-vst3   VST3 behalten, nur das AU entfernen
  -h, --help    Diese Hilfe

Ohne Optionen wird nur die Benutzerinstallation entfernt.
EOF
}

die() {
    printf 'Fehler: %s\n' "$*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --system)
            SYSTEM_WIDE=1
            shift
            ;;
        --all)
            SYSTEM_WIDE=1
            REMOVE_ALL=1
            shift
            ;;
        --keep-vst3)
            KEEP_VST3=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unbekannte Option: $1 (siehe --help)"
            ;;
    esac
done

[ "$(uname -s)" = "Darwin" ] || die "Dieses Skript entfernt macOS-Plugins und läuft nur unter macOS."

USER_AU="${HOME}/Library/Audio/Plug-Ins/Components/${PLUGIN_NAME}.component"
USER_VST="${HOME}/Library/Audio/Plug-Ins/VST3/${PLUGIN_NAME}.vst3"
SYSTEM_AU="/Library/Audio/Plug-Ins/Components/${PLUGIN_NAME}.component"
SYSTEM_VST="/Library/Audio/Plug-Ins/VST3/${PLUGIN_NAME}.vst3"

remove_one() {
    local path="$1"
    local use_sudo="$2"

    if [ -e "${path}" ] || [ -L "${path}" ]; then
        if [ "${use_sudo}" -eq 1 ]; then
            sudo rm -rf "${path}"
        else
            rm -rf "${path}"
        fi
        printf '  entfernt: %s\n' "${path}"
    else
        printf '  nicht vorhanden: %s\n' "${path}"
    fi
}

printf 'De-Feedback entfernen\n'

if [ "${SYSTEM_WIDE}" -eq 1 ]; then
    remove_one "${SYSTEM_AU}" 1
    if [ "${KEEP_VST3}" -eq 0 ]; then
        remove_one "${SYSTEM_VST}" 1
    fi
fi

if [ "${SYSTEM_WIDE}" -eq 0 ] || [ "${REMOVE_ALL}" -eq 1 ]; then
    remove_one "${USER_AU}" 0
    if [ "${KEEP_VST3}" -eq 0 ]; then
        remove_one "${USER_VST}" 0
    fi
fi

killall -9 AudioComponentRegistrar 2>/dev/null || true

printf '\nFertig. Logic Pro neu starten.\n'
printf 'Ein trainiertes Modell unter ~/Library/Application Support/ResonanceAI/ bleibt erhalten.\n'
