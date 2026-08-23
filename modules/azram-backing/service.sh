#!/system/bin/sh
# ZRAM Backing module — creates the writeback device (backing) and nothing
# else. zram itself is Scene's job (scene_swap_controller): it rebuilds the
# zram device at boot and its "restore writeback" path re-attaches the
# backing we set (it reads backing_dev before its own reset).
#
# Division of labor:
#   - this module (runs first, "a" id): backing_dev = hybridswap
#   - Scene (runs after): zram size/algorithm/rebuild/swapon + backing restore
#   - ZramWritebackBoost: writeback scheduling (reads backing_dev)
#   - kernel config: default compressor lz4kd (CONFIG_ZRAM_DEF_COMP)
#
# Why swapoff/reset at all: zram_drv.c only accepts a backing device while
# zram is uninitialized ("Can't setup backing device for initialized device").
# init creates zram active, so we tear it down, attach the backing, and let
# Scene rebuild.

BACKING_DEV=/dev/block/by-name/hybridswap

log() { echo "zram-backing: $*"; }

# wait for zram0 to exist
i=0
while [ ! -e /sys/block/zram0/disksize ] && [ $i -lt 60 ]; do
    sleep 1; i=$((i + 1))
done
[ -e /sys/block/zram0/disksize ] || { log "zram0 never appeared, aborting"; exit 1; }

# ---- tear down init's active zram ----
# unconditional: /proc/swaps rows start at column 0, a " /dev/block/zram0 "
# grep never matches and the swap stays active -> reset fails silently
swapoff /dev/block/zram0 2>/dev/null
echo 1 > /sys/block/zram0/reset 2>/dev/null

# sanity: reset must have zeroed the disksize
if [ "$(cat /sys/block/zram0/disksize 2>/dev/null)" != "0" ]; then
    log "ERROR: reset failed (zram busy?), aborting"
    exit 1
fi

# ---- attach backing (must be before disksize; Scene sets disksize after) ----
if [ -n "$BACKING_DEV" ] && [ -b "$BACKING_DEV" ]; then
    echo "$BACKING_DEV" > /sys/block/zram0/backing_dev 2>/dev/null \
        && log "backing: $(cat /sys/block/zram0/backing_dev)" \
        || log "WARN: could not set backing $BACKING_DEV"
else
    log "skip backing: $BACKING_DEV not a block device"
fi

# That is all. Scene rebuilds zram (size/algorithm/swapon) afterwards and
# restores this backing through its "restore writeback" path.

exit 0
