#!/usr/bin/env bash
set -euo pipefail

# IgH EtherCAT 1.6.13 + native ec_igb installer for Raspberry Pi OS
# Target: Linux 6.12.x, Intel I210/I211-class NIC using Linux igb initially.
#
# Usage:
#   chmod +x install_igh_ec_igb.sh
#   ./install_igh_ec_igb.sh eth1
#
# The first argument is the Linux interface currently bound to the normal igb driver.

IFACE="${1:-eth1}"
IGH_TAG="1.6.13"
SRC_DIR="${HOME}/src/ethercat-${IGH_TAG}"

log() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

KREL="$(uname -r)"
KMAJMIN="$(printf '%s' "$KREL" | sed -E 's/^([0-9]+\.[0-9]+).*/\1/')"

if [[ "$KMAJMIN" != "6.12" ]]; then
    die "This script requires Linux 6.12.x. Current kernel: ${KREL}"
fi

if ! ip link show "$IFACE" >/dev/null 2>&1; then
    die "Interface ${IFACE} does not exist."
fi

if ! command -v sudo >/dev/null 2>&1; then
    die "sudo is required."
fi

log "Installing build dependencies"
sudo apt update
sudo apt install -y \
    git build-essential autoconf automake libtool pkg-config \
    ethtool systemd

if [[ ! -e "/lib/modules/${KREL}/build" ]]; then
    log "Kernel headers are missing; trying to install them"
    if ! sudo apt install -y "linux-headers-${KREL}"; then
        sudo apt install -y raspberrypi-kernel-headers
    fi
fi

[[ -e "/lib/modules/${KREL}/build" ]] || \
    die "Kernel headers are still unavailable for ${KREL}"

DRIVER="$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/ {print $2}')"
PCI_BDF="$(ethtool -i "$IFACE" 2>/dev/null | awk '/^bus-info:/ {print $2}')"
MAC="$(cat "/sys/class/net/${IFACE}/address")"

[[ "$DRIVER" == "igb" ]] || \
    die "${IFACE} is currently using '${DRIVER:-unknown}', not the normal Linux igb driver."
[[ "$PCI_BDF" == *:*:*.* ]] || \
    die "Could not determine PCI BDF for ${IFACE}. Got: '${PCI_BDF}'"

log "Target NIC"
echo "Interface : ${IFACE}"
echo "MAC       : ${MAC}"
echo "PCI BDF   : ${PCI_BDF}"
echo "Driver    : ${DRIVER}"

log "Fetching IgH EtherCAT ${IGH_TAG}"
mkdir -p "$(dirname "$SRC_DIR")"
if [[ -d "${SRC_DIR}/.git" ]]; then
    git -C "$SRC_DIR" fetch --tags --force
    git -C "$SRC_DIR" checkout -f "$IGH_TAG"
else
    rm -rf "$SRC_DIR"
    git clone https://gitlab.com/etherlab.org/ethercat.git "$SRC_DIR"
    git -C "$SRC_DIR" checkout "$IGH_TAG"
fi

cd "$SRC_DIR"

log "Generating configure"
./bootstrap

log "Configuring native ec_igb for Linux 6.12"
./configure \
    --enable-igb \
    --disable-generic \
    --disable-8139too \
    --sysconfdir=/etc

grep -q 'S\["ENABLE_IGB"\]="1"' config.status || \
    die "ENABLE_IGB is not 1 after configure."
grep -q 'S\["KERNEL_IGB"\]="6.12"' config.status || \
    die "IgH did not select the 6.12 igb source."

log "Building IgH EtherCAT"
make -j"$(nproc)" all modules

EC_IGB_KO="$(find "$SRC_DIR" -type f -name 'ec_igb.ko*' -print -quit)"
[[ -n "$EC_IGB_KO" ]] || die "ec_igb.ko was not built."

log "Installing IgH EtherCAT"
sudo make install
sudo make modules_install
sudo depmod -a

modinfo ec_master >/dev/null 2>&1 || die "ec_master is not installed."
modinfo ec_igb >/dev/null 2>&1 || die "ec_igb is not installed."

log "Writing /etc/ethercat.conf"
if [[ -f /etc/ethercat.conf ]]; then
    sudo cp -a /etc/ethercat.conf "/etc/ethercat.conf.bak.$(date +%Y%m%d-%H%M%S)"
fi

sudo tee /etc/ethercat.conf >/dev/null <<EOF
MASTER0_DEVICE="${MAC}"
DEVICE_MODULES="igb"
EOF

log "Installing safe per-device igb unbind helper"
# Do not blacklist igb globally. Only release the selected PCI device before
# ethercat.service loads ec_igb. This avoids disturbing other igb NICs.
sudo tee /usr/local/sbin/ethercat-prepare-igb >/dev/null <<EOF
#!/bin/sh
set -eu

PCI_BDF="${PCI_BDF}"
IFACE="${IFACE}"

DEV="/sys/bus/pci/devices/\${PCI_BDF}"

if [ ! -e "\${DEV}" ]; then
    echo "EtherCAT PCI device \${PCI_BDF} not found" >&2
    exit 1
fi

if [ -L "\${DEV}/driver" ]; then
    DRIVER_NAME="\$(basename "\$(readlink -f "\${DEV}/driver")")"
    if [ "\${DRIVER_NAME}" = "igb" ]; then
        ip link set "\${IFACE}" down 2>/dev/null || true
        echo "\${PCI_BDF}" > /sys/bus/pci/drivers/igb/unbind
    elif [ "\${DRIVER_NAME}" = "ec_igb" ]; then
        exit 0
    fi
fi

exit 0
EOF
sudo chmod 0755 /usr/local/sbin/ethercat-prepare-igb

log "Adding systemd pre-start override"
sudo mkdir -p /etc/systemd/system/ethercat.service.d
sudo tee /etc/systemd/system/ethercat.service.d/10-i210-native-igb.conf >/dev/null <<'EOF'
[Service]
ExecStartPre=/usr/local/sbin/ethercat-prepare-igb
EOF

sudo systemctl daemon-reload

if ! systemctl cat ethercat.service >/dev/null 2>&1; then
    # Normally make install installs this automatically. Keep a fallback.
    if [[ -f "${SRC_DIR}/script/ethercat.service" ]]; then
        sudo cp "${SRC_DIR}/script/ethercat.service" /etc/systemd/system/ethercat.service
        sudo systemctl daemon-reload
    else
        die "ethercat.service was not installed."
    fi
fi

log "Enabling and starting EtherCAT at boot"
sudo systemctl enable ethercat.service
sudo systemctl restart ethercat.service

log "Verification"
echo
echo "--- Kernel ---"
uname -r

echo
echo "--- IgH version ---"
if command -v ethercat >/dev/null 2>&1; then
    ethercat version || true
elif [[ -x /usr/local/bin/ethercat ]]; then
    /usr/local/bin/ethercat version || true
fi

echo
echo "--- Kernel modules ---"
lsmod | grep -E '^(ec_master|ec_igb)\b' || true

echo
echo "--- Service ---"
systemctl --no-pager --full status ethercat.service || true

echo
echo "--- EtherCAT master ---"
if command -v ethercat >/dev/null 2>&1; then
    sudo ethercat master || true
elif [[ -x /usr/local/bin/ethercat ]]; then
    sudo /usr/local/bin/ethercat master || true
fi

echo
echo "Installation complete."
echo "After connecting a slave, run:"
echo "  sudo ethercat slaves"
echo
echo "On the next reboot, verify:"
echo "  systemctl status ethercat"
echo "  lsmod | grep -E 'ec_master|ec_igb'"
echo "  sudo ethercat master"
echo "  sudo ethercat slaves"

if ldconfig -p | grep -q "libethercat.so.1"; then
    echo "[OK] libethercat.so.1 is registered."
else
    echo "[ERROR] libethercat.so.1 was not found in ldconfig cache."
    exit 1
fi