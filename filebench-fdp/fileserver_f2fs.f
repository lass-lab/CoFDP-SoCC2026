#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Portions Copyright 2025-2026 The CoFDP Authors
#

set $dir=/home/femu/f2fs_fdp_mount/fp3
set $nfiles=500000
set $meandirwidth=20
set $filesize=cvar(type=cvar-gamma,parameters=mean:67108864;gamma:1.5)
set $nthreads=16
set $iosize=32m
set $meanappendsize=16m
set $runtime=1200

define fileset name=bigfileset,path=$dir,size=$filesize,entries=$nfiles,dirwidth=$meandirwidth,prealloc=0

define process name=filereader,instances=1
{
    thread name=filereaderthread,memsize=192m,instances=$nthreads
    {
        flowop createfile name=createfile1,filesetname=bigfileset,fd=1
        flowop writewholefile name=wrtfile1,srcfd=1,fd=1,iosize=$iosize
        flowop appendfilerand name=appendfilerand1,iosize=$meanappendsize,fd=1
        flowop readwholefile name=readfile1,fd=1,iosize=$iosize
        flowop statfile name=statfile1,fd=1
        flowop closefile name=closefile1,fd=1
        flowop deletefile name=deletefile1,filesetname=bigfileset,fd=1
    }
}

echo  "File-server Version 3.0 personality successfully loaded"

psrun $runtime