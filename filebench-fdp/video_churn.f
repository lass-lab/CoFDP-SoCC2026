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
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Portions Copyright 2025-2026 The CoFDP Authors
#
# Realistic Video Server Workload for Filebench
#
# Based on the original Sun Microsystems example.
# Extended to simulate dynamic active and passive video set changes.
#

set mode quit alldone

set $piddir4=/home/femu/f2fs_fdp_mount/p4
set $piddir5=/home/femu/f2fs_fdp_mount/p5


set $eventrate=96
set $filesize=512m
set $nthreads=4
set $numactivevids=240
set $numpassivevids=240
set $reuseit=true
set $readiosize=1m
set $writeiosize=64m

set $passvidsname=passivevids
set $actvidsname=activevids

set $repintval=5
set $actrepintval=20
set $promotionintval=30

eventgen rate=$eventrate

define fileset name=$actvidsname,path=$piddir4,size=$filesize,entries=$numactivevids,dirwidth=4,prealloc=2,paralloc,reuse=$reuseit
define fileset name=$passvidsname,path=$piddir5,size=$filesize,entries=$numpassivevids,dirwidth=20,prealloc=25,paralloc,reuse=$reuseit

#
# Writer thread to update passive video set frequently
#
define process name=vidwriter,instances=1
{
  thread name=vidwriter,memsize=128m,instances=1
  {
    flowop deletefile name=vidremover,filesetname=$passvidsname
    flowop createfile name=wrtopen,filesetname=$passvidsname,fd=1
    flowop writewholefile name=newvid,iosize=$writeiosize,fd=1,srcfd=1
    flowop closefile name=wrtclose, fd=1
    flowop delay name=replaceinterval, value=$repintval
    flowop finishoncount name=foc1,value=2000
  }
}

#
# Readers simulate clients watching videos from the active set
#
define process name=vidreaders,instances=1
{
  thread name=vidreaders,memsize=1m,instances=$nthreads
  {
    flowop openfile name=openvid,filesetname=$actvidsname,fd=1
    flowop read name=vidreader,filesetname=$actvidsname,iosize=$readiosize,workingset=10g,random,fd=1
    flowop bwlimit name=serverlimit, target=vidreader,value=1048576
    flowop closefile name=closevid,fd=1
    flowop finishoncount name=foc2,value=20154800
  }
}



#
# Active video set updater (simulate content churn)
#
define process name=activevids_updater,instances=1
{
  thread name=actupdater,memsize=128m,instances=1
  {
    flowop deletefile name=actremover,filesetname=$actvidsname
    flowop createfile name=actcreate,filesetname=$actvidsname,fd=1
    flowop writewholefile name=actwrite,iosize=$writeiosize,fd=1,srcfd=1
    flowop closefile name=actclose,fd=1
    flowop delay name=actupdateinterval,value=$actrepintval
    flowop finishoncount name=foc4,value=500
  }
}

#
# Simulate promotion from passive to active (simplified)
#
define process name=promotion,instances=1
{
  thread name=promoter,memsize=128m,instances=1
  {
    flowop deletefile name=promoremove,filesetname=$actvidsname
    flowop createfile name=promocreate,filesetname=$actvidsname,fd=1
    flowop writewholefile name=promowrite,iosize=$writeiosize,fd=1,srcfd=1
    flowop closefile name=promoclose,fd=1
    flowop delay name=promodelay,value=$promotionintval
    flowop finishoncount name=foc3,value=360
  }
}



echo "Realistic Video Server Workload loaded."
psrun
