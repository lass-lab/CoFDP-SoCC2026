/**
 * f2fs_format_utils.c
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * Dual licensed under the GPL or LGPL version 2 licenses.
 */
#define _LARGEFILE64_SOURCE

#include "f2fs_fs.h"

extern struct f2fs_configuration c;

int f2fs_trim_device(int, uint64_t);
int f2fs_trim_devices(void);
int f2fs_format_device(void);

/*
 * CoFDP PiDir-FS: "mkfs.f2fs -O lost_found" sets up the per-PID logs and the
 * Initial P-Directories. The CoFDP kernel enables PiDir-FS from the same
 * superblock feature bit, so this bit is part of the on-disk contract.
 */
#define PIDIR_ENABLED	(c.feature & cpu_to_le32(F2FS_FEATURE_LOST_FOUND))

/* f2fs_format.c: checks and PID discovery, before anything is written */
int f2fs_pidir_discover(void);

/* f2fs_format_pidir.c: FDP device access (xNVMe) */
int f2fs_pidir_query_pids(uint16_t *pids, unsigned int max);
int f2fs_pidir_write_block(void *buf, uint64_t blkaddr, uint16_t pid);
