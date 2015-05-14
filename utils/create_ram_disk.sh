#!/bin/sh
# utils/create_ram_disk.sh MOUNT_POINT
#
#

RAM_DISK=`hdiutil attach -nomount ram://2414293`
MOUNT_POINT=$1

echo the disk is $RAM_DISK, the  mount point is $MOUNT_POINT

newfs_hfs $RAM_DISK
mount -t hfs $RAM_DISK $MOUNT_POINT

echo
echo umount $MOUNT_POINT
echo hdiutil eject $RAM_DISK

#umount $MOUNT_POINT
#hdiutil eject $RAM_DISK
