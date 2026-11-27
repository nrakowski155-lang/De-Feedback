#!/usr/bin/env bash
#
# De-Feedback – macOS installer for the Audio Unit and VST3 bundles.
#
#   ./scripts/install-macos.sh
#   ./scripts/install-macos.sh --from ~/Downloads/De-Feedback-AU-macOS-universal.zip
#   ./scripts/install-macos.sh --system
#   ./scripts/install-macos.sh --sign "Developer ID Application: Your Name (TEAMID)"
#
# Default target is the current user's plugin folders, so no sudo is needed.
# --system installs into /Library and asks for the administrator password.
#
# The script copies the bundles, removes the quarantine flag, signs them
# (ad-hoc by default), restarts the AudioComponentRegistrar and runs auval.

set -euo pipefail

PLUGIN_NAME="De-Feedback"
AU_TYPE="aufx"
AU_SUBTYPE="Dfb1"
AU_MANUFACTURER="Dfbk"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SOURCE=""
SYSTEM_WIDE=0
SIGN_IDENTITY="-"
RUN_AUVAL=1

usage() {
    cat <<EOF
De-Feedback für macOS installieren

Verwendung:
  $(basename "$0") [Optionen]

Optionen:
  --from <Pfad>    Ordner mit De-Feedback.component/.vst3 oder ein ZIP
                   (Default: build/DeFeedback_artefacts/Release, sonst dist/)
  --system         Nach /Library/Audio/Plug-Ins installieren (sudo)
  --sign <ID>      Signatur-Identität, z. B. "Developer ID Application: …"
                   Default: "-" (ad-hoc). "none" überspringt das Signieren.
  --skip-auval     auval nicht ausführen
  -h, --help       Diese Hilfe

Beispiele:
  ./scripts/install-macos.sh
  ./scripts/install-macos.sh --from ~/Downloads/De-Feedback-AU-macOS-universal.zip
  ./scripts/install-macos.sh --system
EOF
}

die() {
    printf 'Fehler: %s\n' "$*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --from)
            [ $# -ge 2 ] || die "--from braucht einen Pfad"
            SOURCE="$2"
            shift 2
            ;;
        --system)
            SYSTEM_WIDE=1
            shift
            ;;
        --sign)
            [ $# -ge 2 ] || die "--sign braucht eine Identität"
            SIGN_IDENTITY="$2"
            shift 2
            ;;
        --skip-auval)
            RUN_AUVAL=0
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

[ "$(uname -s)" = "Darwin" ] || die "Dieses Skript installiert macOS-Plugins und läuft nur unter macOS."

TMP_DIR=""
cleanup() {
    if [ -n "${TMP_DIR}" ]; then
        rm -rf "${TMP_DIR}"
    fi
}
trap cleanup EXIT

find_bundle() {
    local dir="$1"
    local name="$2"
    local found=""

    if [ -d "${dir}/${name}" ]; then
        printf '%s\n' "${dir}/${name}"
        return 0
    fi

    # BSD find has no -quit: read the stream and keep the first match
    found="$(find "${dir}" -name "${name}" -type d -print 2>/dev/null | sed -n '1p' || true)"

    if [ -n "${found}" ]; then
        printf '%s\n' "${found}"
        return 0
    fi

    return 1
}

# ---------------------------------------------------------------------------
# Locate the bundles (build output, extracted zip, or an explicit path)
# ---------------------------------------------------------------------------
if [ -z "${SOURCE}" ]; then
    for candidate in \
        "${REPO_ROOT}/build/DeFeedback_artefacts/Release" \
        "${REPO_ROOT}/build/DeFeedback_artefacts" \
        "${REPO_ROOT}/dist"; do
        if [ -d "${candidate}" ]; then
            SOURCE="${candidate}"
            break
        fi
    done
fi

[ -n "${SOURCE}" ] || die "Kein Build gefunden. Erst bauen oder --from <Ordner|Zip> angeben."

case "${SOURCE}" in
    *.zip)
        TMP_DIR="$(mktemp -d)"
        printf 'Entpacke %s …\n' "${SOURCE}"
        ditto -x -k "${SOURCE}" "${TMP_DIR}"
        SOURCE="${TMP_DIR}"
        ;;
esac

[ -d "${SOURCE}" ] || die "Quelle ist kein Ordner: ${SOURCE}"

# The GitHub artifact is a zip that contains the AU and VST3 zips: unpack one
# level deeper when no bundle is visible yet.
if [ -z "$(find_bundle "${SOURCE}" "${PLUGIN_NAME}.component" || true)" ]; then
    for nested in "${SOURCE}"/*.zip; do
        [ -e "${nested}" ] || continue
        nested_dir="${SOURCE}/$(basename "${nested}" .zip)"
        mkdir -p "${nested_dir}"
        printf 'Entpacke %s …\n' "$(basename "${nested}")"
        ditto -x -k "${nested}" "${nested_dir}"
    done
fi

AU_SRC="$(find_bundle "${SOURCE}" "${PLUGIN_NAME}.component" || true)"
VST_SRC="$(find_bundle "${SOURCE}" "${PLUGIN_NAME}.vst3" || true)"

[ -n "${AU_SRC}" ] || die "Audio Unit ${PLUGIN_NAME}.component nicht gefunden in ${SOURCE}"

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
if [ "${SYSTEM_WIDE}" -eq 1 ]; then
    AU_DEST_DIR="/Library/Audio/Plug-Ins/Components"
    VST_DEST_DIR="/Library/Audio/Plug-Ins/VST3"
else
    AU_DEST_DIR="${HOME}/Library/Audio/Plug-Ins/Components"
    VST_DEST_DIR="${HOME}/Library/Audio/Plug-Ins/VST3"
fi

run() {
    if [ "${SYSTEM_WIDE}" -eq 1 ]; then
        sudo "$@"
    else
        "$@"
    fi
}

printf 'De-Feedback installieren\n'
printf '  Quelle: %s\n' "${SOURCE}"
printf '  Ziel:   %s\n' "${AU_DEST_DIR}"
printf '          %s\n\n' "${VST_DEST_DIR}"

run mkdir -p "${AU_DEST_DIR}" "${VST_DEST_DIR}"

install_bundle() {
    local src="$1"
    local dest="$2"

    run rm -rf "${dest}"
    run ditto "${src}" "${dest}"
    run xattr -dr com.apple.quarantine "${dest}" 2>/dev/null || true

    if [ "${SIGN_IDENTITY}" != "none" ]; then
        run codesign --force --deep --sign "${SIGN_IDENTITY}" "${dest}"
        run codesign --verify --deep --strict "${dest}"
    fi
}

AU_DEST="${AU_DEST_DIR}/${PLUGIN_NAME}.component"
install_bundle "${AU_SRC}" "${AU_DEST}"
printf '  AU   installiert: %s\n' "${AU_DEST}"

if [ -n "${VST_SRC}" ]; then
    VST_DEST="${VST_DEST_DIR}/${PLUGIN_NAME}.vst3"
    install_bundle "${VST_SRC}" "${VST_DEST}"
    printf '  VST3 installiert: %s\n' "${VST_DEST}"
else
    printf '  VST3 nicht im Build gefunden – nur AU installiert.\n'
fi

killall -9 AudioComponentRegistrar 2>/dev/null || true
sleep 2

if [ "${RUN_AUVAL}" -eq 1 ]; then
    printf '\nAU-Validierung: auval -v %s %s %s\n' "${AU_TYPE}" "${AU_SUBTYPE}" "${AU_MANUFACTURER}"

    if auval -v "${AU_TYPE}" "${AU_SUBTYPE}" "${AU_MANUFACTURER}"; then
        printf '\nAU-Validierung erfolgreich.\n'
    else
        printf '\nAU-Validierung fehlgeschlagen.\n' >&2
        printf 'Prüfe Quarantäne und Signatur:\n' >&2
        printf '  xattr -dr com.apple.quarantine "%s"\n' "${AU_DEST}" >&2
        printf '  codesign --verify --deep --strict "%s"\n' "${AU_DEST}" >&2
        exit 1
    fi
fi

printf '\nFertig. Logic Pro neu starten und das Plugin unter\n'
printf 'Audio FX → Audio Units → De-Feedback Audio → De-Feedback auswählen.\n'
