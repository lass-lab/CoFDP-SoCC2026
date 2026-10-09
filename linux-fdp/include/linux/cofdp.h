/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CoFDP: user-kernel cooperative storage for NVMe Flexible Data Placement SSDs.
 *
 * File systems that implement PiDir-FS keep a 32-bit "PID bits" value per inode.
 * It is held in memory by the PiDirManager (Inode-to-PID table) and persisted in
 * the inode itself (PinodeBlock):
 *
 *   bit 31 clear  Initial P-Directory: bits 0..30 hold one placement ID (PID).
 *   bit 31 set    Flexible P-Directory: bits 0..30 are a bitmap of mapped PIDs,
 *                 one of which is chosen pseudo-randomly for each allocation.
 */
#ifndef _LINUX_COFDP_H
#define _LINUX_COFDP_H

#include <linux/types.h>

#define COFDP_PID_FLEXIBLE	(1U << 31)

static inline bool cofdp_pid_is_flexible(u32 pid_bits)
{
	return pid_bits & COFDP_PID_FLEXIBLE;
}

static inline u32 cofdp_pid_bitmap(u32 pid_bits)
{
	return pid_bits & ~COFDP_PID_FLEXIBLE;
}

#endif /* _LINUX_COFDP_H */
