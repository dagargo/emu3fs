# emu3fs

emu3fs is a Linux kernel module that allows you to read from and write to block devices formatted as E-Mu EIII filesystem. The Emulator samplers using this filesystem are the EIII series, the ESI series (which belongs to the EIII series) and the EIV series.

Currently, it has been verified to work with CDs, Zip drives and SD cards (used with [SCSI2SD](http://www.codesrc.com/mediawiki/index.php/SCSI2SD) and limited to 14 GB because that is the biggest size supported by the ESI 3.02 OS).

## Installation

Simply run `make && sudo make install && sudo depmod`. Note that you will need the Linux kernel headers to compile the module.

## Usage

Once the module is inserted into the kernel (you might use `sudo modprobe emu3_fs` for this), you can mount the devices. You have 2 options here.

* Mount an EIII block device with `sudo mount -t emu3 device mountpoint`.

* Mount an EIV block device with `sudo mount -t emu4 device mountpoint`.

There are no differences between these two filesystems at the structure level. The only difference is that the root node is either the first directory on disk or the root directory respectively. The reason behind this is that EIV series allow to have directories, or folders as they call it, at the root level while older devices only give the user access to the first directory. Hence, it is possible but no recommended to mount an EIII disk as an `emu4` or vice versa.

If the filesystem type is not provided, `emu3` is used as default.

If you get the error below, use the `-t` option.

```
NTFS signature is missing.
Failed to mount '/dev/loop0': Invalid argument
The device '/dev/loop0' doesn't seem to have a valid NTFS.
Maybe the wrong device is used? Or the whole disk instead of a
partition (e.g. /dev/sda, not /dev/sda1)? Or the other way around?
```

### Mounting ISO images

ISO images can be accessed through loop devices. In this example, we are using the `loop0` device.

```
$ sudo losetup /dev/loop0 image
```

### Mounting CDs and other drives

CDs need to be mounted through a loop device when the CD reader is not SCSI. This is due to the fact that the EIII filesystem uses a 512 B block size, which is allowed in SCSI drives, and non SCSI drives have usually a 2 KiB block size. Notice that this applies as well to other drives that are not capable of providing 512 B block size.
If you are using a 512 B block size capable drive, like the `/dev/cdrom`, just do the following and forget about the loop devices.

```
$ sudo mount -t emu3 /dev/cdrom mountpoint
```

### Mounting SCSI2SD partitions

SCSI2SD does not store partition information on the card. Therefore, mounting the card only allows to see the first partition. Nevertheless, `losetup` allows to mount arbitraty portions of the card.

First, we need to know the starting points of each partition, either from a SCSI2SD configuration file, or from SCI2SD itself through `sci2sd-util`.

For example, a 16GB card could be splitted in 4 portions, which would give 4 offsets: 0, 7733504, 15467008 and 23200512 sectors. In order to calculate the offset for the `losetup` command, offsets in bytes have either to be multiplied the sector number by 512 or divide it by 2 in order to have the offset in KiB.

In this example, that will be 0, 3866752K, 7733504K and 11600256K.

Next, we need to know the first available /dev/loop.

```
$ losetup -f
/dev/loop20
```

Then, we can create the loop devices for our partitions.

```
$ sudo losetup /dev/loop20 /dev/sdb
$ sudo losetup -o 3866752K /dev/loop21 /dev/sdb
$ sudo losetup -o 7733504K /dev/loop22 /dev/sdb
$ sudo losetup -o 11600256K /dev/loop23 /dev/sdb
```

And, finally, can be mount the loop devices as usual.

## Bank numbers

The bank number is part of the structure stored on the device but it is **not** a part of the name. When a file is created, the lowest bank number available is used; when a file is deleted, the bank number it was using becomes available.

While any command line tool will work for listing the content, there is no way to show the bank number with any of them as it is not an stardard file attribute. However, it is an extended file attribute and, therefore, it is possible to read and write it with `getfattr` and `setfattr`.

This is how it works.

```
$ getfattr -d -m ".*" *
# file: 12 String Guitar
user.bank.number="31"

# file: 4 Piece Horns 4M
user.bank.number="8"

# file: E3 Main Code
user.bank.number="109"
[...]

$ getfattr -n "user.bank.number" Textural\ Strings
# file: Textural Strings
user.bank.number="6"

$ setfattr -n "user.bank.number" -v "99" Textural\ Strings

$ getfattr -n "user.bank.number" Textural\ Strings
# file: Textural Strings
user.bank.number="99"
```

Keep in mind that setting a bank number does **not** alter the remaining ones so attention must be paid for repeated numbers as devices will show **only** the first one they find for a given bank number.

Alternatively, an `lsemu3` command could be defined as follows.

```
#!/bin/bash

if [ $# -eq 0 ]; then
  lsemu3 * .*
  exit $?
fi

if [ $# -eq 1 ]  && [ "$1" == "-s" ]; then
  lsemu3 * | sort
  exit ${PIPESTATUS[0]}
fi

if [ $# -gt 1 ] && [ "$1" == "-s" ]; then
  shift
  lsemu3 "$@" | sort
  exit ${PIPESTATUS[0]}
fi

e=0
t=0
for f in "$@"; do
  t=$((t + 1))
  [ ! -e "$f" ] && echo "'$f' does not exist" >&2 && e=$((e + 1)) && continue
  if [ -f "$f" ]; then
    bn=$(getfattr -d -m user.bank.number "$f" 2> /dev/null | grep -v "^#" | awk -F\" '{print $2}');
    if [ -n "$bn" ]; then
      if [ $bn -lt 100 ]; then
        bn=$(printf " B%02d " $bn)
      else
        bn=$(printf ".%3d " $bn)
      fi
    else
      bn="F"
    fi
  elif [ -d "$f" ]; then
    bn="D"
  else
    bn="?"
  fi

  i=$(stat --printf="%i" "$f")
  s=$(stat --printf="%s" "$f")
  if [ $s -gt 1024 ]; then
    s=$((s / 1024))
    if [ $s -gt 1024 ]; then
      s=$((s / 1024))M
    else
      s=${s}K
    fi
  else
    s=${s}B
  fi
  printf "%4s %9s %6s '$f'\n" $bn $i "$s"
done

[ $e -gt 0 ] && [ $e -eq $t ] && exit 1
exit 0
```

This is how it works. The `-s` option sorts them by bank number and the second and third columns are the inode and the size respectively.

```
$ lsemu3 -s
 B00         5        1K 'E-mu Banks 1-44'
 B01         4        3M 'Full Arco String'
 B02         6        4M 'SecViolinTrils4M'
 B03         7        8M 'SecViolinTrils8M'
 B04         8        2M 'Solo Violin'
[...]
 B42        46        3M 'StereoGrandPiano'
 B43        47        3M 'Flautas Bonita'
 B44        48        3M 'Tenor Sax'
 B99        10        3M 'Textural Strings'
.109         3       64K 'E3 Main Code'
```

This helps to detect banks with the same number and it is useful when reordering banks. Files with bank number greater or equal than 100 are not considered banks but they are still there.

## About repeated filenames

Remember that although Unix does **not allow** files with the same name in the same directory, the samplers **do allow** this and thus some commands might seem to behave strangely so try to avoid this scenario. In Unix, paths are unique and point to a single inode.

```
Default Folder$ $ ls -li
total 32
4 -rw-r--r-- 1 user user 10738 ago 30 19:54 'Untitled Bank'
4 -rw-r--r-- 1 user user 10738 ago 30 19:54 'Untitled Bank'
4 -rw-r--r-- 1 user user 10738 ago 30 19:54 'Untitled Bank'
```

Listing the bank number does not work either.

```
Default Folder$ lsemu3
 B00     4     10738 'Untitled Bank'
 B00     4     10738 'Untitled Bank'
 B00     4     10738 'Untitled Bank'
```

However, it can be addressed easily although you still can not tell them apart.

```
Default Folder$ mv Untitled\ Bank Untitled\ Bank\ 2

Default Folder$ ls -li
total 32
5 -rw-r--r-- 1 user user 10738 ago 30 19:55 'Untitled Bank'
5 -rw-r--r-- 1 user user 10738 ago 30 19:55 'Untitled Bank'
4 -rw-r--r-- 1 user user 10738 ago 30 19:54 'Untitled Bank 2'

Default Folder$ mv Untitled\ Bank Untitled\ Bank\ 3

Default Folder$ ls -li
total 32
6 -rw-r--r-- 1 user user 10738 ago 30 20:56 'Untitled Bank'
5 -rw-r--r-- 1 user user 10738 ago 30 19:55 'Untitled Bank 2'
4 -rw-r--r-- 1 user user 10738 ago 30 19:54 'Untitled Bank 3'

Default Folder$ lsemu3 -s
 B00     4     10738 'Untitled Bank 2'
 B01     5     10738 'Untitled Bank 3'
 B02     6     10738 'Untitled Bank'
```

## Testing

You can run some simple tests from the `tests` directory. The script mounts a clean image and run some commands on it. **Be aware that you will be asked for the root password** because some commands like `mount` requiere this.


```
$ ./tests.sh
```

## Related project

[emu3bm](https://github.com/dagargo/emu3bm) is a EIII and EIV bank manager that allows a basic edition of presets and sample export and import.
