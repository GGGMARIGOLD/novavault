#!/usr/bin/env bash
# Sets up Vespera on a fresh Ubuntu server so it runs 24/7 on your domain with HTTPS.
#   curl -fsSL https://raw.githubusercontent.com/GGGMARIGOLD/vespera/main/deploy/setup-server.sh | sudo bash -s yourdomain.com
# Safe to run again: it updates Vespera and keeps your data.
set -euo pipefail

DOMAIN="${1:-}"
REPO="${VESPERA_REPO:-https://github.com/GGGMARIGOLD/vespera}"
APP=/opt/vespera

if [ -z "$DOMAIN" ]; then echo "Usage: sudo bash setup-server.sh yourdomain.com"; exit 1; fi
if [ "$(id -u)" -ne 0 ]; then echo "Please run this with sudo."; exit 1; fi
say() { printf '\n\033[1;35m==> %s\033[0m\n' "$*"; }

say "Installing what Vespera needs"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q g++ make zlib1g-dev git curl gnupg debian-keyring debian-archive-keyring apt-transport-https

say "Downloading Vespera"
id vespera >/dev/null 2>&1 || useradd --system --home-dir "$APP" --shell /usr/sbin/nologin vespera
if [ -d "$APP/.git" ]; then git -C "$APP" pull --ff-only; else git clone --depth 1 "$REPO" "$APP"; fi
git config --global --add safe.directory "$APP" || true

say "Building Vespera (about a minute)"
make -C "$APP" vespera

say "Building the image shrinker (optional, Rust)"
if ! command -v cargo >/dev/null 2>&1 && [ ! -x /root/.cargo/bin/cargo ]; then
  curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal || true
fi
CARGO="$(command -v cargo || echo /root/.cargo/bin/cargo)"
if [ -x "$CARGO" ]; then "$CARGO" build --release -q --manifest-path "$APP/img/Cargo.toml" || echo "(skipped: images won't be shrunk)"; fi

mkdir -p "$APP/data"
chown -R vespera:vespera "$APP"

say "Making Vespera start automatically and restart if it ever stops"
cat > /etc/systemd/system/vespera.service <<UNIT
[Unit]
Description=Vespera game store
After=network-online.target
Wants=network-online.target

[Service]
User=vespera
WorkingDirectory=$APP
Environment=PORT=8080
Environment=HOST=127.0.0.1
ExecStart=$APP/vespera
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable vespera >/dev/null
systemctl restart vespera

say "Installing Caddy (gets the HTTPS padlock certificate automatically)"
if ! command -v caddy >/dev/null 2>&1; then
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' > /etc/apt/sources.list.d/caddy-stable.list
  apt-get update -q && apt-get install -y -q caddy
fi
cat > /etc/caddy/Caddyfile <<CADDY
$DOMAIN, www.$DOMAIN {
	encode gzip
	reverse_proxy 127.0.0.1:8080
}
CADDY
systemctl enable caddy >/dev/null
systemctl reload caddy || systemctl restart caddy

say "Opening the web ports (80 and 443) on this server"
if command -v iptables >/dev/null 2>&1; then
  for p in 80 443; do iptables -C INPUT -p tcp --dport $p -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport $p -j ACCEPT; done
  if command -v netfilter-persistent >/dev/null 2>&1; then netfilter-persistent save >/dev/null 2>&1 || true; fi
fi
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q active; then ufw allow 80/tcp >/dev/null; ufw allow 443/tcp >/dev/null; fi

say "Adding the vespera-update command"
cat > /usr/local/bin/vespera-update <<'UPD'
#!/usr/bin/env bash
# Pulls the newest Vespera from GitHub, rebuilds it and restarts. Your data is kept.
set -e
git -C /opt/vespera pull --ff-only
make -C /opt/vespera vespera
CARGO="$(command -v cargo || echo /root/.cargo/bin/cargo)"
[ -x "$CARGO" ] && "$CARGO" build --release -q --manifest-path /opt/vespera/img/Cargo.toml || true
chown -R vespera:vespera /opt/vespera
systemctl restart vespera
echo "Vespera updated and restarted."
UPD
chmod +x /usr/local/bin/vespera-update

IP="$(curl -fsS --max-time 5 https://api.ipify.org || echo 'this server')"
sleep 2
if systemctl is-active --quiet vespera; then STATUS="running"; else STATUS="NOT running (check: sudo journalctl -u vespera -n 30)"; fi
cat <<DONE

==============================================================
  Vespera is $STATUS on this server.

  Last step: in your domain's DNS settings, point
      $DOMAIN      ->  A record  ->  $IP
      www          ->  A record  ->  $IP
  Then open https://$DOMAIN (the padlock can take a few minutes).

  The first account created becomes the admin: make yours first.
  Update later with:   sudo vespera-update
==============================================================
DONE
