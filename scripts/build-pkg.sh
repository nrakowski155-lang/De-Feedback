#!/usr/bin/env bash
#
# De-Feedback – build a macOS installer package (.pkg) and a matching
# uninstall package from an existing build.
#
#   ./scripts/build-pkg.sh
#   ./scripts/build-pkg.sh --from build/DeFeedback_artefacts/Release --out dist
#   ./scripts/build-pkg.sh --sign "Developer ID Installer: Your Name (TEAMID)"
#
# The installer package places the AU in /Library/Audio/Plug-Ins/Components
# and the VST3 in /Library/Audio/Plug-Ins/VST3. The uninstall package is
# payload-free and removes both bundles again.

set -euo pipefail

PLUGIN_NAME="De-Feedback"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VERSION="$(sed -n 's/^project(DeFeedback VERSION \([0-9.]*\).*/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -n 1)"
if [ -z "${VERSION}" ]; then
    VERSION="1.1.0"
fi

SOURCE=""
OUT_DIR="${REPO_ROOT}/dist"
INSTALLER_SIGN=""
BUILD_UNINSTALLER=1

usage() {
    cat <<EOF
De-Feedback Installer-Paket bauen

Verwendung:
  $(basename "$0") [Optionen]

Optionen:
  --from <Pfad>   Ordner mit De-Feedback.component/.vst3
                  (Default: build/DeFeedback_artefacts/Release)
  --out <Pfad>    Zielordner für die .pkg-Dateien (Default: dist/)
  --sign <ID>     Installer-Zertifikat, z. B.
                  "Developer ID Installer: Your Name (TEAMID)"
  --no-uninstaller  Nur das Installer-Paket bauen
  -h, --help      Diese Hilfe

Ergebnis:
  dist/De-Feedback-${VERSION}.pkg           (Installer)
  dist/De-Feedback-Uninstall-${VERSION}.pkg (Deinstaller)
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
        --out)
            [ $# -ge 2 ] || die "--out braucht einen Pfad"
            OUT_DIR="$2"
            shift 2
            ;;
        --sign)
            [ $# -ge 2 ] || die "--sign braucht eine Identität"
            INSTALLER_SIGN="$2"
            shift 2
            ;;
        --no-uninstaller)
            BUILD_UNINSTALLER=0
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

[ "$(uname -s)" = "Darwin" ] || die "pkgbuild gibt es nur unter macOS."

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

if [ -z "${SOURCE}" ]; then
    for candidate in \
        "${REPO_ROOT}/build/DeFeedback_artefacts/Release" \
        "${REPO_ROOT}/build/DeFeedback_artefacts"; do
        if [ -d "${candidate}" ]; then
            SOURCE="${candidate}"
            break
        fi
    done
fi

[ -n "${SOURCE}" ] || die "Kein Build gefunden. Erst bauen oder --from <Ordner> angeben."
[ -d "${SOURCE}" ] || die "Quelle ist kein Ordner: ${SOURCE}"

AU_SRC="$(find_bundle "${SOURCE}" "${PLUGIN_NAME}.component" || true)"
VST_SRC="$(find_bundle "${SOURCE}" "${PLUGIN_NAME}.vst3" || true)"

[ -n "${AU_SRC}" ] || die "Audio Unit ${PLUGIN_NAME}.component nicht gefunden in ${SOURCE}"

STAGE="$(mktemp -d)"
SCRIPTS_INSTALL="$(mktemp -d)"
SCRIPTS_UNINSTALL="$(mktemp -d)"

cleanup() {
    rm -rf "${STAGE}" "${SCRIPTS_INSTALL}" "${SCRIPTS_UNINSTALL}"
}
trap cleanup EXIT

AU_STAGE_DIR="${STAGE}/Library/Audio/Plug-Ins/Components"
VST_STAGE_DIR="${STAGE}/Library/Audio/Plug-Ins/VST3"

mkdir -p "${AU_STAGE_DIR}" "${VST_STAGE_DIR}" "${OUT_DIR}"

ditto "${AU_SRC}" "${AU_STAGE_DIR}/${PLUGIN_NAME}.component"
codesign --force --deep --sign - "${AU_STAGE_DIR}/${PLUGIN_NAME}.component"
codesign --verify --deep --strict "${AU_STAGE_DIR}/${PLUGIN_NAME}.component"

if [ -n "${VST_SRC}" ]; then
    ditto "${VST_SRC}" "${VST_STAGE_DIR}/${PLUGIN_NAME}.vst3"
    codesign --force --deep --sign - "${VST_STAGE_DIR}/${PLUGIN_NAME}.vst3"
    codesign --verify --deep --strict "${VST_STAGE_DIR}/${PLUGIN_NAME}.vst3"
else
    rmdir "${VST_STAGE_DIR}"
    printf 'Hinweis: kein VST3 gefunden, das Paket enthält nur das AU.\n'
fi

xattr -cr "${STAGE}" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Installer package
# ---------------------------------------------------------------------------
cat > "${SCRIPTS_INSTALL}/postinstall" <<'POSTINSTALL'
#!/bin/sh
# Restart the AU registry so Logic sees the freshly installed bundle.
killall -9 AudioComponentRegistrar 2>/dev/null || true
exit 0
POSTINSTALL
chmod +x "${SCRIPTS_INSTALL}/postinstall"

INSTALLER_PKG="${OUT_DIR}/${PLUGIN_NAME}-${VERSION}.pkg"

if [ -n "${INSTALLER_SIGN}" ]; then
    pkgbuild --root "${STAGE}" \
        --identifier "com.defeedback.plugin.installer" \
        --version "${VERSION}" \
        --install-location "/" \
        --ownership recommended \
        --scripts "${SCRIPTS_INSTALL}" \
        --sign "${INSTALLER_SIGN}" \
        "${INSTALLER_PKG}"
else
    pkgbuild --root "${STAGE}" \
        --identifier "com.defeedback.plugin.installer" \
        --version "${VERSION}" \
        --install-location "/" \
        --ownership recommended \
        --scripts "${SCRIPTS_INSTALL}" \
        "${INSTALLER_PKG}"
fi

printf '\nInstaller:  %s\n' "${INSTALLER_PKG}"

# ---------------------------------------------------------------------------
# Uninstall package (payload-free)
# ---------------------------------------------------------------------------
if [ "${BUILD_UNINSTALLER}" -eq 1 ]; then
    cat > "${SCRIPTS_UNINSTALL}/postinstall" <<'POSTUNINSTALL'
#!/bin/sh
rm -rf "/Library/Audio/Plug-Ins/Components/De-Feedback.component"
rm -rf "/Library/Audio/Plug-Ins/VST3/De-Feedback.vst3"
killall -9 AudioComponentRegistrar 2>/dev/null || true
exit 0
POSTUNINSTALL
    chmod +x "${SCRIPTS_UNINSTALL}/postinstall"

    UNINSTALLER_PKG="${OUT_DIR}/${PLUGIN_NAME}-Uninstall-${VERSION}.pkg"

    if [ -n "${INSTALLER_SIGN}" ]; then
        pkgbuild --nopayload \
            --identifier "com.defeedback.plugin.uninstaller" \
            --version "${VERSION}" \
            --scripts "${SCRIPTS_UNINSTALL}" \
            --sign "${INSTALLER_SIGN}" \
            "${UNINSTALLER_PKG}"
    else
        pkgbuild --nopayload \
            --identifier "com.defeedback.plugin.uninstaller" \
            --version "${VERSION}" \
            --scripts "${SCRIPTS_UNINSTALL}" \
            "${UNINSTALLER_PKG}"
    fi

    printf 'Deinstaller: %s\n' "${UNINSTALLER_PKG}"
fi

printf '\nInstallieren:\n'
printf '  sudo installer -pkg "%s" -target /\n' "${INSTALLER_PKG}"

if [ "${BUILD_UNINSTALLER}" -eq 1 ]; then
    printf 'Deinstallieren:\n'
    printf '  sudo installer -pkg "%s" -target /\n' "${UNINSTALLER_PKG}"
fi
