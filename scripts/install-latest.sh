#!/usr/bin/env bash
#
# De-Feedback – download and install the latest macOS release from GitHub.
#
#   ./scripts/install-latest.sh
#   ./scripts/install-latest.sh --tag v1.1.0
#   ./scripts/install-latest.sh --print-url
#
# The release assets are built by .github/workflows/build-macos.yml when a tag
# like v1.1.0 is pushed. The installer package contains both the AU and VST3.
#
# Public repository: works with curl out of the box.
# Private repository: run `gh auth login` once (the script then uses the
# GitHub CLI), or set GITHUB_TOKEN/GH_TOKEN with release read access.

set -euo pipefail

REPO="${DEFB_REPO:-nrakowski155-lang/De-Feedback}"
TAG="latest"
RUN_AUVAL=1
PRINT_URL_ONLY=0

usage() {
    cat <<EOF
De-Feedback aus einem GitHub-Release installieren

Verwendung:
  $(basename "$0") [Optionen]

Optionen:
  --tag <tag>     Release-Tag, z. B. v1.1.0 (Default: neuestes Release)
  --repo <owner/repo>  Abweichendes Repository
  --no-auval      auval nach der Installation nicht ausführen
  --print-url     Nur die Download-URL ausgeben, nichts installieren
  -h, --help      Diese Hilfe

Voraussetzungen:
  macOS, Administratorrechte für den Installer.
  Privates Repository: 'gh auth login' oder GITHUB_TOKEN mit Release-Lesezugriff.
EOF
}

die() {
    printf 'Fehler: %s\n' "$*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)
            [ $# -ge 2 ] || die "--tag braucht einen Wert"
            TAG="$2"
            shift 2
            ;;
        --repo)
            [ $# -ge 2 ] || die "--repo braucht owner/repo"
            REPO="$2"
            shift 2
            ;;
        --no-auval)
            RUN_AUVAL=0
            shift
            ;;
        --print-url)
            PRINT_URL_ONLY=1
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

asset_url() {
    local name="$1"

    if [ "${TAG}" = "latest" ]; then
        printf 'https://github.com/%s/releases/latest/download/%s\n' "${REPO}" "${name}"
    else
        printf 'https://github.com/%s/releases/download/%s/%s\n' "${REPO}" "${TAG}" "${name}"
    fi
}

if [ "${PRINT_URL_ONLY}" -eq 1 ]; then
    asset_url "De-Feedback.pkg"
    exit 0
fi

[ "$(uname -s)" = "Darwin" ] || die "Dieses Skript installiert macOS-Plugins und läuft nur unter macOS."

TMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

GH=""
if command -v gh >/dev/null 2>&1; then
    GH="$(command -v gh)"
fi

have_gh_auth() {
    [ -n "${GH}" ] && "${GH}" auth status --hostname github.com >/dev/null 2>&1
}

download_asset() {
    local name="$1"

    if have_gh_auth; then
        if [ "${TAG}" = "latest" ]; then
            "${GH}" release download --repo "${REPO}" --pattern "${name}" \
                --dir "${TMP_DIR}" --clobber
        else
            "${GH}" release download "${TAG}" --repo "${REPO}" --pattern "${name}" \
                --dir "${TMP_DIR}" --clobber
        fi
        return 0
    fi

    local url
    url="$(asset_url "${name}")"
    local token="${GITHUB_TOKEN:-${GH_TOKEN:-}}"

    if [ -n "${token}" ]; then
        curl -fsSL -H "Authorization: Bearer ${token}" \
            -o "${TMP_DIR}/${name}" "${url}"
    else
        curl -fsSL -o "${TMP_DIR}/${name}" "${url}"
    fi
}

printf 'Lade De-Feedback von GitHub (%s, %s) …\n' "${REPO}" "${TAG}"

if ! download_asset "De-Feedback.pkg"; then
    cat >&2 <<EOF

Download fehlgeschlagen.

Mögliche Ursachen:
  * Es gibt noch kein Release (Tag pushen: git tag v1.1.0 && git push origin v1.1.0).
  * Das Repository ist privat und du bist nicht angemeldet.

Für ein privates Repository:
  gh auth login
oder
  export GITHUB_TOKEN=<Token mit Release-Lesezugriff>

Release-Seite:
  https://github.com/${REPO}/releases
EOF
    exit 1
fi

PKG="${TMP_DIR}/De-Feedback.pkg"
[ -s "${PKG}" ] || die "Das heruntergeladene Paket ist leer."

# Verify the checksum when the release ships one.
if download_asset "SHA256SUMS" 2>/dev/null && [ -s "${TMP_DIR}/SHA256SUMS" ]; then
    if ( cd "${TMP_DIR}" && grep ' De-Feedback\.pkg$' SHA256SUMS | shasum -a 256 -c - ); then
        printf 'SHA-256 geprüft.\n'
    else
        die "SHA-256-Prüfsumme stimmt nicht – Download abgebrochen."
    fi
else
    printf 'Hinweis: kein SHA256SUMS im Release, überspringe Prüfsumme.\n'
fi

printf 'Installiere (Administratorpasswort erforderlich) …\n'
sudo installer -pkg "${PKG}" -target /

killall -9 AudioComponentRegistrar 2>/dev/null || true
sleep 2

if [ "${RUN_AUVAL}" -eq 1 ]; then
    printf '\nAU-Validierung: auval -v aufx Dfb1 Dfbk\n'

    if auval -v aufx Dfb1 Dfbk; then
        printf '\nAU-Validierung erfolgreich.\n'
    else
        die "AU-Validierung fehlgeschlagen. Prüfe die Installation unter /Library/Audio/Plug-Ins/."
    fi
fi

printf '\nFertig. Logic Pro neu starten und das Plugin unter\n'
printf 'Audio FX → Audio Units → De-Feedback Audio → De-Feedback auswählen.\n'
