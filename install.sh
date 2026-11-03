#!/bin/bash
# Ubuntu 22.04 / 24.04 서버용 원클릭 설치 (Docker 필요 없음)
# 사용법:  sudo bash install.sh chess.ericlee.dev
set -euo pipefail
DOMAIN="${1:?사용법: sudo bash install.sh chess.ericlee.dev}"
[ "$(id -u)" = 0 ] || { echo "sudo 로 실행하세요."; exit 1; }
SRC="$(cd "$(dirname "$0")" && pwd)"
APP=/opt/lunaris-web
export DEBIAN_FRONTEND=noninteractive

echo "==> 1/6 기본 패키지"
apt-get update -y
apt-get install -y build-essential curl ca-certificates gnupg rsync debian-keyring debian-archive-keyring apt-transport-https

echo "==> 2/6 Node.js"
if ! command -v node >/dev/null || [ "$(node -v | sed 's/v//;s/\..*//')" -lt 20 ]; then
  curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
  apt-get install -y nodejs
fi

echo "==> 3/6 Caddy (HTTPS 자동)"
if ! command -v caddy >/dev/null; then
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' > /etc/apt/sources.list.d/caddy-stable.list
  apt-get update -y
  apt-get install -y caddy
fi

echo "==> 4/6 앱 설치 및 엔진 빌드 (이 서버 CPU에 맞춰 컴파일)"
id lunaris >/dev/null 2>&1 || useradd --system --home "$APP" --shell /usr/sbin/nologin lunaris
mkdir -p "$APP"
rsync -a --delete --exclude node_modules --exclude bin --exclude data --exclude .git "$SRC"/ "$APP"/
cd "$APP"
bash build.sh
npm ci --omit=dev
mkdir -p data
chown -R lunaris:lunaris "$APP"

echo "==> 5/6 서비스 등록 (재부팅해도 자동 시작)"
cat > /etc/systemd/system/lunaris-web.service <<UNIT
[Unit]
Description=Lunaris chess web + self-play
After=network.target

[Service]
User=lunaris
WorkingDirectory=$APP
Environment=NODE_ENV=production PORT=3000 DATA_DIR=$APP/data ALLOWED_ORIGINS=https://$DOMAIN
ExecStart=/usr/bin/node server/server.js
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable --now lunaris-web
systemctl restart lunaris-web

echo "==> 6/6 HTTPS 프록시"
cat > /etc/caddy/Caddyfile <<CADDY
$DOMAIN {
	encode gzip
	reverse_proxy 127.0.0.1:3000
}
CADDY
systemctl enable --now caddy
systemctl reload caddy || systemctl restart caddy
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then ufw allow 80/tcp; ufw allow 443/tcp; fi

echo
echo "완료! DNS(A 레코드)가 이 서버 IP를 가리키면 https://$DOMAIN 에서 열립니다."
echo "상태 확인:  systemctl status lunaris-web     로그:  journalctl -u lunaris-web -f"
