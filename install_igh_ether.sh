#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# IgH EtherCAT 1.6.13 clean installer
#
# Raspberry Pi OS / Linux 6.12.x
#
# Usage:
#
#   Raspberry Pi onboard Ethernet:
#       ./install_igh_ether.sh eth0 generic
#
#   Intel I210:
#       ./install_igh_ether.sh eth1 igb
#
# The script removes the existing IgH installation first,
# then builds and installs:
#
#   ec_master
#   ec_generic
#   ec_igb
#   ec_8139too
#
# Only the selected driver is enabled in /etc/ethercat.conf.
# ============================================================


# ============================================================
# Arguments
# ============================================================

IFACE="${1:-}"
MODE="${2:-}"

IGH_TAG="1.6.13"
SRC_DIR="${HOME}/src/ethercat-${IGH_TAG}"


log()
{
    printf '\n==> %s\n' "$*"
}


die()
{
    printf '\nERROR: %s\n' "$*" >&2
    exit 1
}


usage()
{
    cat <<EOF

Usage:

  Raspberry Pi onboard Ethernet:
    $0 eth0 generic

  Intel I210:
    $0 eth1 igb

EOF
}


if [[ -z "$IFACE" || -z "$MODE" ]]; then
    usage
    exit 1
fi


case "$MODE" in

    generic)
        ;;

    igb)
        ;;

    *)
        die "Driver must be 'generic' or 'igb'."
        ;;

esac


# ============================================================
# Basic checks
# ============================================================

if ! command -v sudo >/dev/null 2>&1; then
    die "sudo is required."
fi


KREL="$(uname -r)"

KMAJMIN="$(
    printf '%s' "$KREL" |
    sed -E 's/^([0-9]+\.[0-9]+).*/\1/'
)"


if [[ "$KMAJMIN" != "6.12" ]]; then
    die "This script requires Linux 6.12.x. Current kernel: ${KREL}"
fi


echo
echo "============================================================"
echo " IgH EtherCAT Installer"
echo "============================================================"
echo
echo "Interface : ${IFACE}"
echo "Mode      : ${MODE}"
echo "Kernel    : ${KREL}"
echo


# ============================================================
# Dependencies
# ============================================================

log "Installing build dependencies"

sudo apt update

sudo apt install -y \
    git \
    build-essential \
    autoconf \
    automake \
    libtool \
    pkg-config \
    ethtool \
    systemd


# ============================================================
# Kernel headers
# ============================================================

if [[ ! -e "/lib/modules/${KREL}/build" ]]; then

    log "Kernel headers are missing; trying to install them"

    if ! sudo apt install -y "linux-headers-${KREL}"; then
        sudo apt install -y raspberrypi-kernel-headers
    fi

fi


[[ -e "/lib/modules/${KREL}/build" ]] || \
    die "Kernel headers are still unavailable for ${KREL}"


# ============================================================
# Stop previous EtherCAT installation
# ============================================================

log "Stopping existing EtherCAT installation"

sudo systemctl stop ethercat.service 2>/dev/null || true


# ============================================================
# Remove old systemd overrides
# ============================================================

log "Removing old EtherCAT systemd overrides"

sudo rm -f \
    /etc/systemd/system/ethercat.service.d/10-i210-native-igb.conf

sudo rm -f \
    /etc/systemd/system/ethercat.service.d/10-generic.conf

sudo rm -f \
    /etc/systemd/system/ethercat.service.d/20-generic-network.conf


# ============================================================
# Unload existing EtherCAT modules
# ============================================================

log "Unloading existing EtherCAT kernel modules"

sudo modprobe -r ec_8139too 2>/dev/null || true
sudo modprobe -r ec_igb     2>/dev/null || true
sudo modprobe -r ec_generic 2>/dev/null || true
sudo modprobe -r ec_master  2>/dev/null || true


# Check again
if lsmod | grep -q '^ec_'; then

    echo
    echo "WARNING: Some EtherCAT modules are still loaded:"
    lsmod | grep '^ec_' || true
    echo

fi


# ============================================================
# Restore normal Linux NIC driver
# ============================================================

if [[ "$MODE" == "igb" ]]; then

    log "Loading normal Linux igb driver"

    sudo modprobe igb

fi


# Give udev / kernel a moment
sleep 1


# ============================================================
# Check selected interface
# ============================================================

if ! ip link show "$IFACE" >/dev/null 2>&1; then

    echo
    echo "Available interfaces:"
    ip -br link
    echo

    die "Interface ${IFACE} does not exist."

fi


# ============================================================
# Get NIC information
# ============================================================

MAC="$(
    cat "/sys/class/net/${IFACE}/address"
)"


DRIVER="$(
    ethtool -i "$IFACE" 2>/dev/null |
    awk '/^driver:/ {print $2}'
)"


PCI_BDF="$(
    ethtool -i "$IFACE" 2>/dev/null |
    awk '/^bus-info:/ {print $2}'
)"


log "Selected Ethernet device"

echo "Interface : ${IFACE}"
echo "MAC       : ${MAC}"
echo "Driver    : ${DRIVER:-unknown}"
echo "PCI BDF   : ${PCI_BDF:-N/A}"


# ============================================================
# Validate selected mode
# ============================================================

if [[ "$MODE" == "igb" ]]; then

    [[ "$DRIVER" == "igb" ]] || \
        die "${IFACE} uses '${DRIVER:-unknown}', not Linux igb."

    [[ "$PCI_BDF" == *:*:*.* ]] || \
        die "Could not determine PCI BDF for ${IFACE}."

fi


# ============================================================
# Bring generic NIC up before EtherCAT
# ============================================================

if [[ "$MODE" == "generic" ]]; then

    log "Preparing generic Ethernet interface"

    sudo ip link set "$IFACE" up

fi


# ============================================================
# Remove old installed IgH files
# ============================================================

log "Removing previous IgH installation"


# CLI / controller
sudo rm -f \
    /usr/local/bin/ethercat

sudo rm -f \
    /usr/local/sbin/ethercatctl


# Helper scripts
sudo rm -f \
    /usr/local/sbin/ethercat-prepare-igb

sudo rm -f \
    /usr/local/sbin/ethercat-prepare-generic


# Shared library
sudo rm -f \
    /usr/local/lib/libethercat.so \
    /usr/local/lib/libethercat.so.*


# Headers
sudo rm -f \
    /usr/local/include/ecrt.h \
    /usr/local/include/ectty.h


# Systemd unit installed by previous IgH
sudo rm -f \
    /lib/systemd/system/ethercat.service

sudo rm -f \
    /usr/local/lib/systemd/system/ethercat.service


# Previous config
sudo rm -f \
    /etc/ethercat.conf


# ============================================================
# Remove old EtherCAT kernel modules
# ============================================================

log "Removing previously installed EtherCAT kernel modules"

sudo find "/lib/modules/${KREL}" \
    -type f \
    \( \
        -name 'ec_master.ko' \
        -o -name 'ec_master.ko.*' \
        -o -name 'ec_generic.ko' \
        -o -name 'ec_generic.ko.*' \
        -o -name 'ec_igb.ko' \
        -o -name 'ec_igb.ko.*' \
        -o -name 'ec_8139too.ko' \
        -o -name 'ec_8139too.ko.*' \
    \) \
    -delete


sudo depmod -a


# ============================================================
# Remove old source tree
# ============================================================

log "Removing previous source tree"

rm -rf "$SRC_DIR"


# ============================================================
# Clone IgH
# ============================================================

log "Fetching IgH EtherCAT ${IGH_TAG}"

mkdir -p "$(dirname "$SRC_DIR")"


git clone \
    https://gitlab.com/etherlab.org/ethercat.git \
    "$SRC_DIR"


git -C "$SRC_DIR" \
    checkout "$IGH_TAG"


cd "$SRC_DIR"


# ============================================================
# bootstrap
# ============================================================

log "Generating configure"

./bootstrap


# ============================================================
# configure
# ============================================================

log "Configuring IgH EtherCAT"

./configure \
    --enable-igb \
    --enable-8139too \
    --enable-generic \
    --sysconfdir=/etc


# ============================================================
# Check IGB configuration
# ============================================================

grep -q 'S\["ENABLE_IGB"\]="1"' config.status || \
    die "ENABLE_IGB is not enabled."


grep -q 'S\["KERNEL_IGB"\]="6.12"' config.status || \
    die "IgH did not select Linux 6.12 igb source."


# ============================================================
# Build
# ============================================================

log "Building IgH EtherCAT"

make -j"$(nproc)" all modules


# ============================================================
# Verify build results
# ============================================================

EC_MASTER_KO="$(
    find "$SRC_DIR" \
    -type f \
    -name 'ec_master.ko*' \
    -print \
    -quit
)"


EC_IGB_KO="$(
    find "$SRC_DIR" \
    -type f \
    -name 'ec_igb.ko*' \
    -print \
    -quit
)"


EC_GENERIC_KO="$(
    find "$SRC_DIR" \
    -type f \
    -name 'ec_generic.ko*' \
    -print \
    -quit
)"


[[ -n "$EC_MASTER_KO" ]] || \
    die "ec_master.ko was not built."


[[ -n "$EC_IGB_KO" ]] || \
    die "ec_igb.ko was not built."


[[ -n "$EC_GENERIC_KO" ]] || \
    die "ec_generic.ko was not built."


# ============================================================
# Install
# ============================================================

log "Installing IgH EtherCAT"

sudo make install

sudo make modules_install

sudo depmod -a


# ============================================================
# Verify installed modules
# ============================================================

modinfo ec_master >/dev/null 2>&1 || \
    die "ec_master is not installed."


modinfo ec_igb >/dev/null 2>&1 || \
    die "ec_igb is not installed."


modinfo ec_generic >/dev/null 2>&1 || \
    die "ec_generic is not installed."


# ============================================================
# Shared library path
# ============================================================

log "Registering /usr/local/lib"

echo "/usr/local/lib" |
sudo tee \
    /etc/ld.so.conf.d/ethercat.conf \
    >/dev/null


sudo ldconfig


if ! ldconfig -p | grep -q "libethercat.so.1"; then

    die "libethercat.so.1 was not registered."

fi


# ============================================================
# Write /etc/ethercat.conf
# ============================================================

log "Writing /etc/ethercat.conf"


sudo tee \
    /etc/ethercat.conf \
    >/dev/null <<EOF
MASTER0_DEVICE="${MAC}"
DEVICE_MODULES="${MODE}"
EOF


# ============================================================
# I210 native ec_igb preparation
# ============================================================

if [[ "$MODE" == "igb" ]]; then

    log "Installing I210 ec_igb preparation helper"


    sudo tee \
        /usr/local/sbin/ethercat-prepare-igb \
        >/dev/null <<EOF
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

    DRIVER_NAME="\$(
        basename "\$(
            readlink -f "\${DEV}/driver"
        )"
    )"


    if [ "\${DRIVER_NAME}" = "igb" ]; then

        ip link set "\${IFACE}" down \
            2>/dev/null || true

        echo "\${PCI_BDF}" \
            > /sys/bus/pci/drivers/igb/unbind


    elif [ "\${DRIVER_NAME}" = "ec_igb" ]; then

        exit 0

    fi

fi


exit 0
EOF


    sudo chmod \
        0755 \
        /usr/local/sbin/ethercat-prepare-igb


    log "Installing I210 systemd override"


    sudo mkdir -p \
        /etc/systemd/system/ethercat.service.d


    sudo tee \
        /etc/systemd/system/ethercat.service.d/10-i210-native-igb.conf \
        >/dev/null <<'EOF'
[Service]
ExecStartPre=/usr/local/sbin/ethercat-prepare-igb
EOF

fi


# ============================================================
# Generic driver preparation
# ============================================================

if [[ "$MODE" == "generic" ]]; then

    log "Installing generic Ethernet preparation helper"


    sudo tee \
        /usr/local/sbin/ethercat-prepare-generic \
        >/dev/null <<EOF
#!/bin/sh
set -eu

IFACE="${IFACE}"

ip link set "\${IFACE}" up

exit 0
EOF


    sudo chmod \
        0755 \
        /usr/local/sbin/ethercat-prepare-generic


    sudo mkdir -p \
        /etc/systemd/system/ethercat.service.d


    sudo tee \
        /etc/systemd/system/ethercat.service.d/10-generic.conf \
        >/dev/null <<'EOF'
[Unit]
Requires=network.target
After=network.target

[Service]
ExecStartPre=/usr/local/sbin/ethercat-prepare-generic
EOF

fi


# ============================================================
# Reload systemd
# ============================================================

log "Reloading systemd"

sudo systemctl daemon-reload


# ============================================================
# Check service
# ============================================================

if ! systemctl cat ethercat.service >/dev/null 2>&1; then

    die "ethercat.service was not installed."

fi


# ============================================================
# Enable / start
# ============================================================

log "Starting EtherCAT"

sudo systemctl enable ethercat.service

sudo systemctl restart ethercat.service


# ============================================================
# Small startup delay
# ============================================================

sleep 1


# ============================================================
# Verification
# ============================================================

log "Verification"

echo
echo "------------------------------------------------------------"
echo "Configuration"
echo "------------------------------------------------------------"

cat /etc/ethercat.conf


echo
echo "------------------------------------------------------------"
echo "Kernel modules"
echo "------------------------------------------------------------"

lsmod |
grep -E '^(ec_master|ec_igb|ec_generic|ec_8139too)\b' ||
true


echo
echo "------------------------------------------------------------"
echo "EtherCAT master"
echo "------------------------------------------------------------"

if command -v ethercat >/dev/null 2>&1; then

    sudo ethercat master || true

elif [[ -x /usr/local/bin/ethercat ]]; then

    sudo /usr/local/bin/ethercat master || true

fi


echo
echo "------------------------------------------------------------"
echo "EtherCAT slaves"
echo "------------------------------------------------------------"

if command -v ethercat >/dev/null 2>&1; then

    sudo ethercat slaves || true

elif [[ -x /usr/local/bin/ethercat ]]; then

    sudo /usr/local/bin/ethercat slaves || true

fi


echo
echo "============================================================"
echo " Installation complete"
echo "============================================================"
echo
echo "Interface : ${IFACE}"
echo "MAC       : ${MAC}"
echo "Mode      : ${MODE}"
echo


if [[ "$MODE" == "generic" ]]; then

    echo "Raspberry Pi / generic configuration active."

else

    echo "Intel I210 / native ec_igb configuration active."

fi


echo
echo "Verify later with:"
echo
echo "  sudo ethercat master"
echo "  sudo ethercat slaves"
echo
echo "  cat /etc/ethercat.conf"
echo