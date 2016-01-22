# emu3fs

emu3fs is a Linux kernel module that allows to read from and write to disks formatted in an E-MU emu3 sampler family filesystem. The samplers using this filesystem are the emulator 3, emulator 3x and the ESI family.
Currently, it has been tested to work with CDs and Zip drives.
It's distributed under the terms of the GPL v3.

## Installation

Simply run `make && sudo make install`.
Note that you will need the Linux kernel headers to compile the module.

## Usage
Of course, in any case you will need to install the kernel module with `modprobe emu3_fs`.

### Mounting ISO images

ISO images can be used through loop devices. In this example we are using the loop0 device.
```
losetup /dev/loop0 /path/to/iso/image
mount -t emu3 /dev/loop0 /mount/point
```
If the module ntfs is inserted in the kernel the -t option might be needed, otherwise you can ignore it. If you get the error below use the -t option.
```
NTFS signature is missing.
Failed to mount '/dev/loop0': Invalid argument
The device '/dev/loop0' doesn't seem to have a valid NTFS.
Maybe the wrong device is used? Or the whole disk instead of a
partition (e.g. /dev/sda, not /dev/sda1)? Or the other way around?
```

### Mounting CDs and other drives

CDs need to be mounted through a loop device when the CD reader is not SCSI. This is due to the fact that the emu3 filesystem uses a 512B block size, which is allowed in SCSI drives, and non SCSI drives have usually a 2KB block size. Notice that this applies as well to other drives that are not capable of providing 512B block size.
If you are using a 512B block size capable drive, like the `/dev/cdrom` just do the following and forget about the loop devices.
```
mount -t emu3 /dev/cdrom /mount/point
```
