# emu3fs

emu3fs is a Linux kernel module that allows to read from and write to disks formatted as E-Mu EIII filesystem. The samplers using this filesystem are the emulator 3, emulator 3x, the ESI series (which belong to the EIII series) and the EIV series.
Currently, it has been tested to work with CDs, Zip drives and SD cards up to 2 GB.
It's distributed under the terms of the GPL v3.

## Installation

Simply run `make && sudo make install`.
Note that you will need the Linux kernel headers to compile the module.

## Usage

Of course, in any case you will need to insert the kernel module with `modprobe emu3_fs`.

### Mounting ISO images

ISO images can be accessed through loop devices. In this example, we are using the loop0 device.

```
$ losetup /dev/loop0 /path/to/iso/image
```

Then, you can mount the image. You have 2 options here.

#### Mount it as emu3

```
$ mount -t emu3 /dev/loop0 /mount/point
```

#### Mount it as emu4

```
$ mount -t emu4 /dev/loop0 /mount/point
```

There are no differences between these two filesystems at the structure level. The only difference is that the root node is either the first directory on disk or the root directory respectively. The reason behind this is that EIV series allow to have directories, or folders as they call it, at the root level while other devices only give the user access to the first directory.

If the filesystem type is not provided, `emu3` is used as default but, if you get the error below, use the `-t` option.

```
NTFS signature is missing.
Failed to mount '/dev/loop0': Invalid argument
The device '/dev/loop0' doesn't seem to have a valid NTFS.
Maybe the wrong device is used? Or the whole disk instead of a
partition (e.g. /dev/sda, not /dev/sda1)? Or the other way around?
```

### Mounting CDs and other drives

CDs need to be mounted through a loop device when the CD reader is not SCSI. This is due to the fact that the EIII filesystem uses a 512 B block size, which is allowed in SCSI drives, and non SCSI drives have usually a 2KB block size. Notice that this applies as well to other drives that are not capable of providing 512 B block size.
If you are using a 512 B block size capable drive, like the `/dev/cdrom` just do the following and forget about the loop devices.

```
$ mount -t emu3 /dev/cdrom /mount/point
```

## Testing

You can run some simple tests from the `tests` directory. The script mounts a clean image and run some commands on it. **Beware it uses `sudo` to run some commands**, so you might be asked for your password.

Also, you will need to pass the `EMU3_MOUNTPOINT` variable.

```
$ EMU3_MOUNTPOINT=/media/emu3 ./tests.sh
```

## Related project

[emu3bm](https://github.com/dagargo/emu3bm) is a EIII bank manager that allows a basic edition of presets and sample export and import for all the devices in the EIII series.
