#!/usr/bin/env bash
#
# Write the generated card assets onto a microSD card for the device.
#
#   tools/provision_sd.sh /dev/sdX            # copy onto the existing FAT32 fs
#   tools/provision_sd.sh /dev/sdX --format   # repartition + mkfs.vfat first
#
# The card must be FAT32: exFAT is compiled out of the firmware
# (FF_FS_EXFAT=0), so an exFAT card mounts as nothing at all.
#
# DESTRUCTIVE: --format wipes the whole device. The script refuses to touch
# anything that looks like a system disk and always shows you what it is about
# to erase, requiring a typed confirmation.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS="${REPO_ROOT}/assets/phonics"

die() { echo "error: $*" >&2; exit 1; }

DEVICE="${1:-}"
FORMAT=0
[[ "${2:-}" == "--format" ]] && FORMAT=1

[[ -n "$DEVICE" ]] || die "usage: $0 /dev/sdX [--format]"
[[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"
[[ -d "$ASSETS" ]] || die "no assets at $ASSETS -- run tools/gen_assets.py first"
[[ -f "$ASSETS/manifest.json" ]] || die "no manifest.json in $ASSETS"

# --- Refuse to eat the machine -------------------------------------------
BASE="$(basename "$DEVICE")"
if [[ "$BASE" == mmcblk* || "$BASE" == nvme* ]]; then
    die "$DEVICE looks like internal storage; refusing"
fi
ROOT_SRC="$(findmnt -no SOURCE / || true)"
if [[ -n "$ROOT_SRC" && "$ROOT_SRC" == "$DEVICE"* ]]; then
    die "$DEVICE hosts the root filesystem; refusing"
fi
if [[ "$(lsblk -ndo RM "$DEVICE" 2>/dev/null || echo 0)" != "1" ]]; then
    echo "WARNING: $DEVICE is not flagged removable."
fi

echo "=== target ==="
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,MODEL "$DEVICE"
# lsblk rather than blockdev: blockdev --getsize64 needs root, and this
# script should be able to show you what it is about to touch before it asks
# for any privilege.
SIZE_BYTES="$(lsblk -bndo SIZE "$DEVICE" 2>/dev/null || echo 0)"
SIZE_GB="$(( SIZE_BYTES / 1000000000 ))"
echo "capacity: ~${SIZE_GB} GB"
echo
ASSET_MB="$(du -sm "$ASSETS" | cut -f1)"
CARD_COUNT="$(python3 -c "import json;print(len(json.load(open('$ASSETS/manifest.json'))['cards']))")"
echo "payload : ${ASSET_MB} MB, ${CARD_COUNT} cards"
echo

if [[ $FORMAT -eq 1 ]]; then
    echo "*** THIS WILL ERASE EVERY PARTITION ON $DEVICE ***"
    read -r -p "Type ERASE to continue: " reply
    [[ "$reply" == "ERASE" ]] || die "aborted"
else
    read -r -p "Copy assets onto $DEVICE? [y/N] " reply
    [[ "$reply" == "y" || "$reply" == "Y" ]] || die "aborted"
fi

# Unmount anything currently mounted from this device.
for part in $(lsblk -lno NAME "$DEVICE" | tail -n +2); do
    mp="$(findmnt -no TARGET "/dev/$part" || true)"
    if [[ -n "$mp" ]]; then
        echo "unmounting /dev/$part from $mp"
        udisksctl unmount -b "/dev/$part" >/dev/null 2>&1 || sudo umount "/dev/$part"
    fi
done

PART=""
if [[ $FORMAT -eq 1 ]]; then
    echo "wiping partition table..."
    sudo wipefs -a "$DEVICE" >/dev/null
    # One primary FAT32 partition spanning the card.
    sudo sfdisk "$DEVICE" >/dev/null <<SFDISK
label: dos
,,c
SFDISK
    sudo partprobe "$DEVICE" 2>/dev/null || true
    sleep 2
    PART="$(lsblk -lno NAME "$DEVICE" | tail -n +2 | head -1)"
    [[ -n "$PART" ]] || die "no partition appeared after partitioning"
    PART="/dev/$PART"
    echo "formatting $PART as FAT32 (label PHONICS)..."
    # 32KB clusters: 130 cards is only a few hundred files, and larger
    # clusters keep the FAT small, which the SPI-mode SD driver appreciates.
    sudo mkfs.vfat -F 32 -n PHONICS -s 64 "$PART" >/dev/null
    sudo partprobe "$DEVICE" 2>/dev/null || true
    sleep 2
else
    PART="$(lsblk -lno NAME,FSTYPE "$DEVICE" | awk '$2=="vfat"{print $1; exit}')"
    [[ -n "$PART" ]] || die "no FAT partition on $DEVICE (re-run with --format)"
    PART="/dev/$PART"
fi

MNT="$(mktemp -d)"
cleanup() {
    sync
    sudo umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

echo "mounting $PART..."
sudo mount -o uid="$(id -u)",gid="$(id -g)" "$PART" "$MNT"

echo "copying assets..."
rm -rf "${MNT:?}/phonics"
mkdir -p "$MNT/phonics"
cp -r "$ASSETS/." "$MNT/phonics/"
sync

echo
echo "=== on card ==="
echo "manifest : $(stat -c%s "$MNT/phonics/manifest.json") bytes"
echo "images   : $(find "$MNT/phonics/cards" -name '*.png' | wc -l)"
echo "audio    : $(find "$MNT/phonics/cards" -name '*.wav' | wc -l)"
df -h "$MNT" | tail -1
echo
echo "done. Put the card in the device and power-cycle it."
echo "The firmware expects /sd/phonics/manifest.json"
