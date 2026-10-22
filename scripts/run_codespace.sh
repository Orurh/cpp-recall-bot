#!/usr/bin/env bash

set -euo pipefail

if [[ "${CODESPACES:-}" != "true" ]]; then
  echo "This script must be run inside GitHub Codespaces." >&2
  exit 1
fi

: "${TELEGRAM_BOT_TOKEN:?Add TELEGRAM_BOT_TOKEN to your GitHub Codespaces secrets and restart the codespace}"
: "${CODESPACE_NAME:?CODESPACE_NAME is not set}"
: "${GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN:?GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN is not set}"

export MINI_APP_URL="${MINI_APP_URL:-https://${CODESPACE_NAME}-8080.${GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN}/}"

docker compose up --build -d

if command -v gh >/dev/null 2>&1; then
  if ! gh codespace ports visibility 8080:public --codespace "${CODESPACE_NAME}"; then
    echo "Could not make port 8080 public automatically." >&2
    echo "Open PORTS, right-click 8080, then select Port Visibility -> Public." >&2
  fi
else
  echo "GitHub CLI is unavailable. Make port 8080 public in the PORTS panel." >&2
fi

docker compose ps
echo
echo "Mini App URL: ${MINI_APP_URL}"
echo "Logs: docker compose logs -f bot"
