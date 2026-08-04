#!/usr/bin/env bash
#
# HMS-GW-S3 - Cloud MQTT broker setup
#
# Sets up Eclipse Mosquitto in Docker (TLS + password auth via Let's Encrypt)
# on a Debian/Ubuntu VPS, so the HMS-GW-S3 gateway (and a future mobile app)
# can reach it from outside the local network.
#
# Usage:
#   curl -fsSL https://github.com/danielguedel/HMS-GW-S3/releases/latest/download/getting-started.sh | bash
#
# Configuration (all via environment variables, all optional except MQTT_DOMAIN):
#   MQTT_DOMAIN   Domain name that already points at this server's IP (required
#                 for the Let's Encrypt certificate). Prompted for if not set
#                 and a terminal is available.
#   MQTT_USER     MQTT username (default: hmsgw)
#   MQTT_PASSWORD MQTT password (default: randomly generated and printed once)
#   INSTALL_DIR   Install directory (default: /opt/hms-gw-mqtt)
#
# Example:
#   MQTT_DOMAIN=mqtt.example.com curl -fsSL .../getting-started.sh | bash

set -euo pipefail

INSTALL_DIR="${INSTALL_DIR:-/opt/hms-gw-mqtt}"
MQTT_USER="${MQTT_USER:-hmsgw}"

log() { echo -e "\033[1;32m[hms-gw-mqtt]\033[0m $*"; }
err() { echo -e "\033[1;31m[hms-gw-mqtt]\033[0m $*" >&2; }

if [ "$(id -u)" -ne 0 ]; then
  err "Please run as root (e.g. via sudo)."
  exit 1
fi

# --- domain (required for the TLS certificate) ---
if [ -z "${MQTT_DOMAIN:-}" ] && [ -r /dev/tty ]; then
  read -rp "Domain name pointing at this server (for the TLS cert): " MQTT_DOMAIN < /dev/tty
fi
if [ -z "${MQTT_DOMAIN:-}" ]; then
  err "MQTT_DOMAIN is required, e.g.:"
  err "  MQTT_DOMAIN=mqtt.example.com curl -fsSL <url> | bash"
  exit 1
fi

# --- password ---
GENERATED_PASSWORD=0
if [ -z "${MQTT_PASSWORD:-}" ]; then
  MQTT_PASSWORD="$(openssl rand -base64 18)"
  GENERATED_PASSWORD=1
fi

log "Domain:  ${MQTT_DOMAIN}"
log "User:    ${MQTT_USER}"
log "Install: ${INSTALL_DIR}"

# --- dependencies ---
if ! command -v docker >/dev/null 2>&1; then
  log "Installing Docker..."
  curl -fsSL https://get.docker.com | sh
fi

if ! docker compose version >/dev/null 2>&1; then
  err "Docker Compose plugin not found even after installing Docker. Aborting."
  exit 1
fi

if ! command -v certbot >/dev/null 2>&1; then
  log "Installing certbot..."
  apt-get update -qq
  apt-get install -y -qq certbot
fi

mkdir -p "${INSTALL_DIR}"/mosquitto/{config,data,log}
cd "${INSTALL_DIR}"

# --- TLS certificate ---
if [ ! -d "/etc/letsencrypt/live/${MQTT_DOMAIN}" ]; then
  PORT80_UNIT=""
  PORT80_CONTAINER=""
  if ss -ltnp "( sport = :80 )" 2>/dev/null | grep -q LISTEN; then
    PORT80_PID="$(ss -ltnp "( sport = :80 )" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1)"
    PROC_NAME="$(ps -p "${PORT80_PID}" -o comm= 2>/dev/null || true)"

    if [ "${PROC_NAME}" = "docker-proxy" ] && command -v docker >/dev/null 2>&1; then
      # Port 80 is published by a container, not by the Docker daemon itself - stop only
      # that container. Never stop docker.service for this: that would take down every
      # other container on the host, not just the one on port 80.
      PORT80_CONTAINER="$(docker ps --filter "publish=80" --format '{{.ID}}' | head -1)"
      if [ -z "${PORT80_CONTAINER}" ]; then
        err "Port 80 is held by docker-proxy but no container with a published port 80 was found. Aborting."
        exit 1
      fi
      CONTAINER_NAME="$(docker ps --filter "id=${PORT80_CONTAINER}" --format '{{.Names}}')"
      log "Port 80 is published by Docker container '${CONTAINER_NAME}' - stopping just that container temporarily..."
      docker stop "${PORT80_CONTAINER}" >/dev/null
      # Restore it no matter what happens next, even if certbot fails and `set -e` exits the script.
      trap 'docker start "${PORT80_CONTAINER}" >/dev/null 2>&1 || true' EXIT
    else
      PORT80_UNIT="$(systemctl status "${PORT80_PID}" 2>/dev/null | grep -oP '^\W+\K\S+\.service' | head -1)"
      if [ -z "${PORT80_UNIT}" ] || [ "${PORT80_UNIT}" = "docker.service" ]; then
        err "Port 80 is in use by PID ${PORT80_PID:-unknown} (process: ${PROC_NAME:-unknown}), which isn't a single"
        err "service I can safely stop automatically (stopping docker.service would affect ALL containers on this"
        err "host). Stop the specific container/process manually and re-run this script."
        exit 1
      fi
      log "Port 80 is used by ${PORT80_UNIT} - stopping it temporarily to issue the certificate..."
      systemctl stop "${PORT80_UNIT}"
      trap 'systemctl start "${PORT80_UNIT}" 2>/dev/null || true' EXIT
    fi
  fi

  log "Requesting Let's Encrypt certificate for ${MQTT_DOMAIN}..."
  certbot certonly --standalone --non-interactive --agree-tos \
    -m "admin@${MQTT_DOMAIN}" -d "${MQTT_DOMAIN}"

  if [ -n "${PORT80_CONTAINER}" ]; then
    log "Restarting Docker container '${CONTAINER_NAME}'..."
    docker start "${PORT80_CONTAINER}" >/dev/null
    trap - EXIT
  elif [ -n "${PORT80_UNIT}" ]; then
    log "Restarting ${PORT80_UNIT}..."
    systemctl start "${PORT80_UNIT}"
    trap - EXIT
  fi
else
  log "Certificate for ${MQTT_DOMAIN} already exists, skipping issuance."
fi

# --- mosquitto config ---
cat > mosquitto/config/mosquitto.conf << EOF
listener 8883
protocol mqtt
cafile /mosquitto/certs/chain.pem
certfile /mosquitto/certs/cert.pem
keyfile /mosquitto/certs/privkey.pem

allow_anonymous false
password_file /mosquitto/config/passwd

persistence true
persistence_location /mosquitto/data/
log_dest file /mosquitto/log/mosquitto.log
EOF

# mosquitto_passwd -c refuses to overwrite an existing file (O_CREAT|O_EXCL), so drop
# it first - this script always (re)creates a fresh single-user password file.
rm -f mosquitto/config/passwd
docker run --rm -v "${INSTALL_DIR}/mosquitto/config:/mosquitto/config" \
  eclipse-mosquitto:2 mosquitto_passwd -b -c /mosquitto/config/passwd "${MQTT_USER}" "${MQTT_PASSWORD}"

# mosquitto/{config,data,log} were created by mkdir (as root) and the passwd file above
# runs as root too, but the broker itself drops privileges to the image's unprivileged
# "mosquitto" user - which then can't read the passwd file or write the log without this.
log "Fixing ownership for the container's mosquitto user..."
docker run --rm --user root -v "${INSTALL_DIR}/mosquitto:/mosquitto" \
  eclipse-mosquitto:2 chown -R mosquitto:mosquitto /mosquitto/config /mosquitto/data /mosquitto/log

# --- docker compose stack ---
cat > docker-compose.yml << EOF
services:
  mosquitto:
    image: eclipse-mosquitto:2
    container_name: hms-gw-mqtt
    restart: unless-stopped
    ports:
      - "8883:8883"
    volumes:
      - ./mosquitto/config:/mosquitto/config
      - ./mosquitto/data:/mosquitto/data
      - ./mosquitto/log:/mosquitto/log
      - /etc/letsencrypt/live/${MQTT_DOMAIN}:/mosquitto/certs:ro
EOF

log "Starting Mosquitto..."
docker compose up -d

# --- keep the broker in sync with certificate renewals ---
mkdir -p /etc/letsencrypt/renewal-hooks/deploy
cat > /etc/letsencrypt/renewal-hooks/deploy/hms-gw-mqtt-restart.sh << EOF
#!/bin/sh
cd ${INSTALL_DIR} && docker compose restart mosquitto
EOF
chmod +x /etc/letsencrypt/renewal-hooks/deploy/hms-gw-mqtt-restart.sh

# --- firewall (only touch it if ufw is already active) ---
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
  log "Opening port 8883/tcp in ufw..."
  ufw allow 8883/tcp >/dev/null
fi

echo
log "Done."
log "Broker:   mqtts://${MQTT_DOMAIN}:8883"
log "User:     ${MQTT_USER}"
if [ "${GENERATED_PASSWORD}" -eq 1 ]; then
  log "Password: ${MQTT_PASSWORD}  (generated - save it now, it is not stored in plaintext anywhere)"
else
  log "Password: (as provided via MQTT_PASSWORD)"
fi
echo
log "Point the HMS-GW-S3 gateway's MQTT settings at this broker (TLS enabled, port 8883)."
echo
log "Test it from any machine with mosquitto-clients installed:"
log "  mosquitto_sub -h ${MQTT_DOMAIN} -p 8883 --capath /etc/ssl/certs -u '${MQTT_USER}' -P '${MQTT_PASSWORD}' -t 'hms-gw/#' -v"
log "  mosquitto_pub -h ${MQTT_DOMAIN} -p 8883 --capath /etc/ssl/certs -u '${MQTT_USER}' -P '${MQTT_PASSWORD}' -t 'hms-gw/test' -m 'hello'"
log "No mosquitto-clients installed? Run the subscriber via Docker instead (mounting the host's"
log "CA bundle, since the eclipse-mosquitto image doesn't ship its own):"
log "  docker run --rm -v /etc/ssl/certs:/etc/ssl/certs:ro eclipse-mosquitto:2 mosquitto_sub -h ${MQTT_DOMAIN} -p 8883 --capath /etc/ssl/certs -u '${MQTT_USER}' -P '${MQTT_PASSWORD}' -t 'hms-gw/#' -v"
