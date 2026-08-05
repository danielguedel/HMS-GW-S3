#!/usr/bin/env bash
#
# HMS-GW-S3 - Cloud MQTT broker uninstall
#
# Reverses what getting-started.sh did:
#   - Stops and removes the Mosquitto Docker container
#   - Deletes the install directory (config, data, logs, certs)
#   - Removes the certbot renewal hook
#   - Closes ports 8883 and 9001 in ufw (if ufw is active)
#
# What this script does NOT touch:
#   - Docker itself (may be used by other services)
#   - certbot / the Let's Encrypt certificate (use --delete-cert to also
#     remove the certificate, e.g. when decommissioning the domain entirely)
#
# Usage:
#   curl -fsSL https://github.com/danielguedel/HMS-GW-S3/releases/latest/download/uninstall.sh | bash
#
# Options (environment variables):
#   INSTALL_DIR    Install directory used during setup (default: /opt/hms-gw-mqtt)
#   MQTT_DOMAIN    Domain name of the certificate to delete (required with --delete-cert)
#   DELETE_CERT    Set to "1" to also delete the Let's Encrypt certificate
#
# Example - full removal including certificate:
#   DELETE_CERT=1 MQTT_DOMAIN=mqtt.example.com \
#     curl -fsSL .../uninstall.sh | bash

set -euo pipefail

INSTALL_DIR="${INSTALL_DIR:-/opt/hms-gw-mqtt}"
DELETE_CERT="${DELETE_CERT:-0}"
RENEWAL_HOOK="/etc/letsencrypt/renewal-hooks/deploy/hms-gw-mqtt-restart.sh"

log() { echo -e "\033[1;32m[hms-gw-mqtt]\033[0m $*"; }
err() { echo -e "\033[1;31m[hms-gw-mqtt]\033[0m $*" >&2; }

if [ "$(id -u)" -ne 0 ]; then
  err "Please run as root (e.g. via sudo)."
  exit 1
fi

# --- confirmation prompt (skipped when piped / non-interactive) ---
if [ -t 0 ]; then
  echo
  log "This will permanently remove the HMS-GW-S3 MQTT broker."
  log "Install directory : ${INSTALL_DIR}"
  log "Renewal hook      : ${RENEWAL_HOOK}"
  [ "${DELETE_CERT}" = "1" ] && log "Certificate       : ${MQTT_DOMAIN:-<MQTT_DOMAIN not set>} (will be deleted)"
  echo
  read -rp "Continue? [y/N] " CONFIRM < /dev/tty
  if [ "${CONFIRM}" != "y" ] && [ "${CONFIRM}" != "Y" ]; then
    log "Aborted."
    exit 0
  fi
fi

# --- stop and remove the Docker container ---
if [ -f "${INSTALL_DIR}/docker-compose.yml" ]; then
  log "Stopping Mosquitto container..."
  cd "${INSTALL_DIR}"
  docker compose down --volumes 2>/dev/null || true
  cd /
else
  log "docker-compose.yml not found in ${INSTALL_DIR} - skipping container teardown."
fi

# --- remove the install directory ---
if [ -d "${INSTALL_DIR}" ]; then
  log "Removing ${INSTALL_DIR}..."
  rm -rf "${INSTALL_DIR}"
else
  log "${INSTALL_DIR} does not exist - nothing to remove."
fi

# --- remove the certbot renewal hook ---
if [ -f "${RENEWAL_HOOK}" ]; then
  log "Removing certbot renewal hook..."
  rm -f "${RENEWAL_HOOK}"
else
  log "Renewal hook not found - skipping."
fi

# --- close ufw ports ---
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
  log "Closing ports 8883/tcp and 9001/tcp in ufw..."
  ufw delete allow 8883/tcp >/dev/null 2>&1 || true
  ufw delete allow 9001/tcp >/dev/null 2>&1 || true
fi

# --- optionally delete the Let's Encrypt certificate ---
if [ "${DELETE_CERT}" = "1" ]; then
  if [ -z "${MQTT_DOMAIN:-}" ]; then
    err "DELETE_CERT=1 requires MQTT_DOMAIN to be set. Certificate not deleted."
  elif command -v certbot >/dev/null 2>&1; then
    log "Deleting Let's Encrypt certificate for ${MQTT_DOMAIN}..."
    certbot delete --cert-name "${MQTT_DOMAIN}" --non-interactive
  else
    err "certbot not found - certificate not deleted."
  fi
fi

echo
log "Done. Docker and certbot were left in place."
[ "${DELETE_CERT}" != "1" ] && log "The Let's Encrypt certificate was kept. To delete it:"
[ "${DELETE_CERT}" != "1" ] && log "  certbot delete --cert-name <your-domain>"
