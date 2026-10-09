#!/bin/bash
# Run FEMU as a multi-streamed SSD (femu_mode=7).
# Derived from run-blackbox.sh (Huaicheng Li <huaicheng@cs.uchicago.edu>).
#
# Each namespace is one stream (stream = NSID - 1), so the device exposes
# stream_number namespaces. Like run-fdp.sh, the script is copied into
# build-femu/ by femu-copy-scripts.sh and started there (./run-ms.sh). The
# guest image is $IMGDIR/fdp.qcow2 (IMGDIR defaults to $HOME/images) and the
# FEMU binary is build-femu/qemu-system-x86_64 of this tree (override with
# QEMU=...). QEMU's output is saved to ./log in the directory the script is
# started from.

FEMU_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# image directory
IMGDIR=${IMGDIR:-$HOME/images}
# Virtual machine disk image
OSIMGF=${OSIMGF:-$IMGDIR/fdp.qcow2}
# FEMU binary, built by femu-compile.sh in build-femu/
QEMU=${QEMU:-$FEMU_ROOT/build-femu/qemu-system-x86_64}

# Configurable SSD Controller layout parameters
secsz=4096 # sector size in bytes, must be the 4 KiB LBA size
pls_per_lun=1 # keep it at one, no multiplanes support
luns_per_ch=8 # number of chips per channel
nchs=8 # number of channels
NAND_PAGE_SIZE_KB=4 # must be the 4 KiB LBA size
NAND_BLOCK_SIZE_MB=64
# user capacity in megabytes; the NAND capacity is ssd_size * 100 / gc_thres_pcent
ssd_size=65536

# Multi-stream: one namespace per stream
stream_number=2

# Latency in nanoseconds
pg_rd_lat=40000 # page read latency
pg_wr_lat=200000 # page write latency
blk_er_lat=2000000 # block erase latency
ch_xfer_lat=25000 # channel transfer time per page

# GC Threshold (1-100)
gc_thres_pcent=75
gc_thres_pcent_high=95

#-----------------------------------------------------------------------

#Compose the entire FEMU MS-SSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=${stream_number}"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=7"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nand_page_size_kb=${NAND_PAGE_SIZE_KB}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nand_block_size_mb=${NAND_BLOCK_SIZE_MB}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",stream_number=${stream_number}"
FEMU_OPTIONS=${FEMU_OPTIONS}",multipoller_enabled=1"

echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit 1
fi

if [[ ! -x "$QEMU" ]]; then
	echo ""
	echo "FEMU binary couldn't be found at $QEMU"
	echo "Please build FEMU first (see femu-scripts/femu-compile.sh)"
	echo ""
	exit 1
fi

sudo "$QEMU" \
    -name "FEMU-MSSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 64 \
    -m 32G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=io_uring,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::8095-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
