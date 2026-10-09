#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Copy necessary scripts for running FEMU

FSD="../femu-scripts"

# CoFDP: run-fdp.sh (FDP SSD, femu_mode=6) and run-ms.sh (multi-stream SSD,
# femu_mode=7) are started from build-femu/ like the other run scripts and
# write QEMU's output to ./log there
CPL=(pkgdep.sh femu-compile.sh run-whitebox.sh run-blackbox.sh run-nossd.sh run-zns.sh run-fdp.sh run-ms.sh pin.sh ftk)

echo ""
echo "==> Copying following FEMU script to current directory:"
for f in "${CPL[@]}"
do
	if [[ ! -e $FSD/$f ]]; then
		echo "Make sure you are under build-femu/ directory!"
		exit
	fi
	cp -r $FSD/$f . && echo "    --> $f"
done
echo "Done!"
echo ""

