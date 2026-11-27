#!/usr/bin/env bash
#
# Build De-Feedback-Installer.pkg: a tiny package whose postinstall script
# downloads the current AU/VST3 bundles from the latest GitHub release and
# installs them into /Library/Audio/Plug-Ins.
#
# The package contains no plug-ins itself, so it stays small and always
# installs the newest published build. It needs anonymous access to the
# release assets, i.e. the repository (or at least its releases) must be
# public.
#
#   ./scripts/build-bootstrap-pkg.sh
#   ./scripts/build-bootstrap-pkg.sh --repo owner/name --out artifacts
#   ./scripts/build-bootstrap-pkg.sh --sign "Developer ID Installer: …"

set -euo pipefail

REPO="nrakowski155-lang/De-Feedback"
OUT_DIR="dist"
SIGN_ID=""

usage() {
    cat <<EOF
De-Feedback Bootstrap-Installer bauen

Verwendung:
  $(basename "$0") [Optionen]

Optionen:
  --repo <owner/repo>  Repository, aus dem der Installer lädt
  --out <dir>          Ausgabeverzeichnis (Default: dist)
  --sign <identity>    Installer-Zertifikat, z. B. "Developer ID Installer: …"
  -h, --help           Diese Hilfe

Ergebnis:
  <out>/De-Feedback-Installer.pkg
EOF
}

die() {
    printf 'Fehler: %s\n' "$*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --repo)
            [ $# -ge 2 ] || die "--repo braucht owner/repo"
            REPO="$2"
            shift 2
            ;;
        --out)
            [ $# -ge 2 ] || die "--out braucht einen Pfad"
            OUT_DIR="$2"
            shift 2
            ;;
        --sign)
            [ $# -ge 2 ] || die "--sign braucht eine Identität"
            SIGN_ID="$2"
            shift 2
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

printf '%s' "${REPO}" | grep -Eq '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' \
    || die "Ungültiges Repository: ${REPO}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "${ROOT_DIR}/CMakeLists.txt" ] || die "CMakeLists.txt nicht gefunden."

VERSION="$(sed -n 's/^project(DeFeedback VERSION \([0-9.]*\).*/\1/p' "${ROOT_DIR}/CMakeLists.txt" | head -n 1)"
[ -n "${VERSION}" ] || die "Version konnte nicht aus CMakeLists.txt gelesen werden."

TMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

mkdir -p "${TMP_DIR}/scripts"

cat > "${TMP_DIR}/scripts/postinstall" <<'POSTINSTALL'
#!/bin/bash
#
# De-Feedback bootstrap postinstall: fetch the current AU/VST3 bundles from
# the latest GitHub release and install them. Runs as root from Installer.
#
set -u

REPO="__REPO__"
BASE="https://github.com/${REPO}/releases/latest/download"
LOG="/var/log/defeedback-bootstrap.log"
AU_DEST="/Library/Audio/Plug-Ins/Components/De-Feedback.component"
VST_DEST="/Library/Audio/Plug-Ins/VST3/De-Feedback.vst3"

exec >>"${LOG}" 2>&1
printf '\n=== %s De-Feedback bootstrap ===\n' "$(/bin/date -u '+%Y-%m-%dT%H:%M:%SZ')"

TMP_DIR="$(/usr/bin/mktemp -d /tmp/defeedback-bootstrap.XXXXXX)" || exit 1
cleanup() {
    /bin/rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

fetch() {
    /usr/bin/curl -fsSL --retry 3 --connect-timeout 20 \
        -o "${TMP_DIR}/$1" "${BASE}/$1"
}

printf 'Lade %s\n' "${BASE}"

if ! fetch "De-Feedback-AU-macOS-universal.zip"; then
    printf 'FEHLER: Download fehlgeschlagen. Ist das Repository öffentlich?\n'
    exit 1
fi
if ! fetch "De-Feedback-VST3-macOS-universal.zip"; then
    printf 'FEHLER: VST3-Download fehlgeschlagen.\n'
    exit 1
fi

if fetch "SHA256SUMS"; then
    if ! ( cd "${TMP_DIR}" \
        && /usr/bin/grep -E ' De-Feedback-(AU|VST3)-macOS-universal\.zip$' SHA256SUMS > SHA256SUMS.plugins \
        && /usr/bin/shasum -a 256 -c SHA256SUMS.plugins ); then
        printf 'FEHLER: SHA-256-Prüfung fehlgeschlagen.\n'
        exit 1
    fi
else
    printf 'Hinweis: keine SHA256SUMS im Release, Prüfsumme übersprungen.\n'
fi

/bin/mkdir -p "${TMP_DIR}/au" "${TMP_DIR}/vst3" || exit 1
/usr/bin/ditto -x -k "${TMP_DIR}/De-Feedback-AU-macOS-universal.zip" "${TMP_DIR}/au" || exit 1
/usr/bin/ditto -x -k "${TMP_DIR}/De-Feedback-VST3-macOS-universal.zip" "${TMP_DIR}/vst3" || exit 1

if [ ! -d "${TMP_DIR}/au/De-Feedback.component" ]; then
    printf 'FEHLER: AU-Bundle fehlt im ZIP.\n'
    exit 1
fi
if [ ! -d "${TMP_DIR}/vst3/De-Feedback.vst3" ]; then
    printf 'FEHLER: VST3-Bundle fehlt im ZIP.\n'
    exit 1
fi

/bin/mkdir -p "$(/usr/bin/dirname "${AU_DEST}")" "$(/usr/bin/dirname "${VST_DEST}")" || exit 1
/bin/rm -rf "${AU_DEST}" "${VST_DEST}" || exit 1
/usr/bin/ditto "${TMP_DIR}/au/De-Feedback.component" "${AU_DEST}" || exit 1
/usr/bin/ditto "${TMP_DIR}/vst3/De-Feedback.vst3" "${VST_DEST}" || exit 1

/usr/bin/xattr -dr com.apple.quarantine "${AU_DEST}" "${VST_DEST}" 2>/dev/null || true
/usr/bin/codesign --force --deep --sign - "${AU_DEST}" || exit 1
/usr/bin/codesign --verify --deep --strict "${AU_DEST}" || exit 1
/usr/bin/codesign --force --deep --sign - "${VST_DEST}" || exit 1

/usr/bin/killall -9 AudioComponentRegistrar 2>/dev/null || true

printf 'Installiert: %s\n' "${AU_DEST}"
printf 'Installiert: %s\n' "${VST_DEST}"
exit 0
POSTINSTALL

sed "s|__REPO__|${REPO}|g" "${TMP_DIR}/scripts/postinstall" > "${TMP_DIR}/scripts/postinstall.new"
mv "${TMP_DIR}/scripts/postinstall.new" "${TMP_DIR}/scripts/postinstall"
chmod +x "${TMP_DIR}/scripts/postinstall"
bash -n "${TMP_DIR}/scripts/postinstall" || die "Postinstall-Skript ist syntaktisch falsch."

mkdir -p "${OUT_DIR}"
PKG="${OUT_DIR}/De-Feedback-Installer.pkg"

if [ -n "${SIGN_ID}" ]; then
    pkgbuild --nopayload \
        --identifier com.defeedback.bootstrap \
        --version "${VERSION}" \
        --scripts "${TMP_DIR}/scripts" \
        --sign "${SIGN_ID}" \
        "${PKG}"
else
    pkgbuild --nopayload \
        --identifier com.defeedback.bootstrap \
        --version "${VERSION}" \
        --scripts "${TMP_DIR}/scripts" \
        "${PKG}"
fi

printf '\nBootstrap-Installer: %s\n' "${PKG}"
printf 'Beim Installieren lädt er das neueste Release von %s.\n' "${REPO}"
