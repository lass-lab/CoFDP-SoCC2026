#!/bin/bash
# Run FEMU as the configurable FDP SSD of CoFDP (femu_mode=6).
# Derived from run-blackbox.sh (Huaicheng Li <huaicheng@cs.uchicago.edu>).
#
# Defaults: 64 GiB user capacity,
# 8 channels x 8 dies, 4 KiB pages, 16 MiB blocks, 4 reclaim groups (RGs)
# x 2 placement handles (8 PIDs), 40/200 us page read/program, 2 ms block
# erase, 25 us channel transfer, and an 8-vCPU / 16 GiB guest.
#
# PIDs are flat: PID p belongs to RG p / handle_number, so the guest mounts
# the CoFDP F2FS with fdp_log_n = rg_number * handle_number (8 here).
#
# Like the other FEMU run scripts, it is copied into build-femu/ by
# femu-copy-scripts.sh and started there:
#   cd build-femu && ./run-fdp.sh
# Every setting below can be overridden from the environment, e.g.
#   rg_number=1 handle_number=8 ./run-fdp.sh
# The guest image is $IMGDIR/fdp.qcow2 (IMGDIR defaults to $HOME/images) and
# the FEMU binary is build-femu/qemu-system-x86_64 of this tree (override
# with QEMU=...). QEMU's output, including the per-GC log lines and the dump
# printed by the FEMU reset command (I/O Management Send MO 2, issued by
# tools/fdp-reset; format in ../README.md, section 7), is saved to ./log, i.e.
# in the directory the script is started from (build-femu/log above).

FEMU_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# image directory
IMGDIR=${IMGDIR:-$HOME/images}
# Virtual machine disk image
OSIMGF=${OSIMGF:-$IMGDIR/fdp.qcow2}
# FEMU binary, built by femu-compile.sh in build-femu/
QEMU=${QEMU:-$FEMU_ROOT/build-femu/qemu-system-x86_64}

# Configurable SSD Controller layout parameters
secsz=${secsz:-4096} # sector size in bytes, must be the 4 KiB LBA size
pls_per_lun=${pls_per_lun:-1} # keep it at one, no multiplanes support
luns_per_ch=${luns_per_ch:-8} # number of chips per channel
nchs=${nchs:-8} # number of channels
NAND_PAGE_SIZE_KB=${NAND_PAGE_SIZE_KB:-4} # must be the 4 KiB LBA size
NAND_BLOCK_SIZE_MB=${NAND_BLOCK_SIZE_MB:-16}
# user capacity in megabytes; the NAND capacity is ssd_size * 100 / gc_thres_pcent
ssd_size=${ssd_size:-65536}

# FDP: rg_number must be a power-of-two divisor of luns_per_ch * nchs; a
# reclaim unit is one block on every die of an RG
rg_number=${rg_number:-4} # number of reclaim groups
handle_number=${handle_number:-2} # placement handles per RG (PIDs = rg_number * handle_number)

# Latency in nanoseconds
pg_rd_lat=${pg_rd_lat:-40000} # page read latency
pg_wr_lat=${pg_wr_lat:-200000} # page write latency
blk_er_lat=${blk_er_lat:-2000000} # block erase latency
ch_xfer_lat=${ch_xfer_lat:-25000} # channel transfer time per page

# GC Threshold (1-100)
gc_thres_pcent=${gc_thres_pcent:-75}
gc_thres_pcent_high=${gc_thres_pcent_high:-95}

# Guest
vm_smp=${vm_smp:-8}
vm_mem=${vm_mem:-16G}

#-----------------------------------------------------------------------

#Compose the entire FEMU FDP-SSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=6"
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
FEMU_OPTIONS=${FEMU_OPTIONS}",handle_number=${handle_number}"
FEMU_OPTIONS=${FEMU_OPTIONS}",rg_number=${rg_number}"
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
    -name "FEMU-FDPSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp ${vm_smp} \
    -m ${vm_mem} \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=io_uring,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::8085-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
