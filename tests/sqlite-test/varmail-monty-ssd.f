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
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#set $dir=/tmp
set $dir=/mnt/lbz0/
#set $nfiles=50000
#set $meandirwidth=1000000
#set $nfiles=50000
#set $meandirwidth=10000
set $nfiles=900000
set $meandirwidth=20
#set $filesize=cvar(type=cvar-gamma,parameters=mean:16384;gamma:1.5)
#set $filesize=10m
#set $filesize=500k
set $filesize=4k
set $nthreads=16
set $iosize=4k
#set $meanappendsize=500k
#set $meanappendsize=3m
set $meanappendsize=4k
set $directio=1
set $directio_read=1

#define fileset name=bigfileset,path=$dir,size=$filesize,entries=$nfiles,dirwidth=$meandirwidth,prealloc=80
define fileset name=bigfileset,path=$dir,size=$filesize,entries=$nfiles,dirwidth=$meandirwidth,prealloc=100

define process name=filereader,instances=1
{
  thread name=filereaderthread,memsize=10m,instances=$nthreads
  {
    flowop deletefile name=deletefile1,filesetname=bigfileset
    #flowop createfile name=createfile2,filesetname=bigfileset,fd=1
    #flowop appendfilerand name=appendfilerand2,iosize=$meanappendsize,fd=1
    flowop createfile name=createfile2,filesetname=bigfileset,dsync,fd=1
    flowop appendfilerand name=appendfilerand2,iosize=$meanappendsize,dsync,fd=1
    #flowop createfile name=createfile2,filesetname=bigfileset,dsync,fd=1
    #flowop appendfilerand name=appendfilerand2,iosize=$meanappendsize,directio=1,dsync,fd=1
    flowop fsync name=fsyncfile2,fd=1
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile3,filesetname=bigfileset,fd=1,dsync
    flowop readwholefile name=readfile3,fd=1,iosize=$iosize
    flowop appendfilerand name=appendfilerand3,iosize=$meanappendsize,dsync,fd=1
    flowop fsync name=fsyncfile3,fd=1
    flowop closefile name=closefile3,fd=1
    flowop openfile name=openfile4,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile4,fd=1,iosize=$iosize
    flowop closefile name=closefile4,fd=1
  }
}

echo  "Varmail Version 3.0 personality successfully loaded"

#run 30
#run 300
#run 60
#run 600
#run 1200
#run 1800
#run 3600
run 10800
