#!/bin/bash
set -euo pipefail

VID="0411"
PID="03d1"
REPO_URL="https://github.com/lwfinger/rtw88.git"
SRC_DIR="/usr/local/src/rtw88"

echo "========================================"
echo " BUFFALO USB WiFi installer"
echo " USB ID: ${VID}:${PID}"
echo " Kernel: $(uname -r)"
echo "========================================"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: sudo で実行してください。"
    echo "  sudo $0"
    exit 1
fi

echo
echo "[1/8] USB WiFi を確認..."

if lsusb | grep -qi "${VID}:${PID}"; then
    echo "OK: BUFFALO ${VID}:${PID} を検出しました。"
else
    echo "WARNING: ${VID}:${PID} が現在見つかりません。"
    echo "ドングルを挿してから再実行することを推奨します。"
fi

echo
echo "[2/8] 古い 88x2bu ドライバを停止..."

modprobe -r 88x2bu 2>/dev/null || true

cat > /etc/modprobe.d/blacklist-88x2bu.conf <<EOF
blacklist 88x2bu
EOF

echo
echo "[3/8] 必要パッケージをインストール..."

apt update

apt install -y \
    git \
    dkms \
    build-essential \
    raspberrypi-kernel-headers \
    iw \
    usbutils

echo
echo "[4/8] カーネルヘッダを確認..."

if [ ! -e "/lib/modules/$(uname -r)/build" ]; then
    echo "ERROR: 現在のカーネル $(uname -r) 用ヘッダがありません。"
    echo
    echo "確認:"
    echo "  ls -l /lib/modules/$(uname -r)/build"
    echo "  dpkg -l | grep kernel-headers"
    exit 1
fi

echo "OK: kernel headers found."

echo
echo "[5/8] rtw88 ソースを取得..."

if [ -d "${SRC_DIR}/.git" ]; then
    echo "既存ソースを更新します。"
    git -C "${SRC_DIR}" fetch --all
    git -C "${SRC_DIR}" reset --hard origin/master
else
    rm -rf "${SRC_DIR}"
    git clone "${REPO_URL}" "${SRC_DIR}"
fi

cd "${SRC_DIR}"

echo
echo "BUFFALO ${VID}:${PID} の対応を確認..."

if grep -Rqi "0411.*03d1" .; then
    echo "OK: ${VID}:${PID} のエントリを確認しました。"
else
    echo "ERROR: 現在の rtw88 ソースに ${VID}:${PID} が見つかりません。"
    exit 1
fi

echo
echo "[6/8] DKMS ドライバをインストール..."

dkms install "${SRC_DIR}"

echo
echo "[7/8] firmware / modprobe 設定をインストール..."

make install_fw
cp -f rtw88.conf /etc/modprobe.d/rtw88.conf

depmod -a

echo
echo "[8/8] rtw_8822bu をロード..."

modprobe rtw_8822bu || true

sleep 2

echo
echo "========================================"
echo " インストール結果"
echo "========================================"

echo
echo "--- lsmod ---"
lsmod | grep -E 'rtw|88x2bu' || true

echo
echo "--- USB ---"
lsusb | grep -i "${VID}:${PID}" || true

echo
echo "--- WiFi interfaces ---"
iw dev || true

echo
echo "--- network interfaces ---"
ip -brief link || true

echo
echo "--- DKMS ---"
dkms status || true

echo
echo "========================================"

if iw dev 2>/dev/null | grep -q "Interface wlan"; then
    echo "SUCCESS: WiFi インターフェースを検出しました。"
    echo
    echo "WiFi設定:"
    echo "  sudo nmtui"
    echo
    echo "接続後の確認:"
    echo "  iw dev wlan0 link"
    echo "  ip addr show wlan0"
else
    echo "WiFi インターフェースがまだ見つかりません。"
    echo "一度再起動してください:"
    echo
    echo "  sudo reboot"
fi
