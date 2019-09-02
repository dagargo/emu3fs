#!/usr/bin/env bash

if [ "$EMU3_MOUNTPOINT" == "" ]; then
  echo "The variable EMU3_MOUNTPOINT must be set"
  exit -1
fi

function test() {
  [ $? -eq 0 ] && ok=$((ok+1))
  total=$((total+1))
  echo "Results: $ok/$total"
}

total=0
ok=0

echo "Inserting module..."
sudo modprobe -r emu3_fs
sudo modprobe emu3_fs

echo "Uncompressing image..."
cp image.iso.xz.bak image.iso.xz
unxz image.iso.xz

echo "Mounting image..."
sudo losetup /dev/loop0 image.iso
sudo mount -t emu3 /dev/loop0 $EMU3_MOUNTPOINT

echo "1234" > $EMU3_MOUNTPOINT/t1
echo "5678" >> $EMU3_MOUNTPOINT/t1
[ "12345678" != "$(< $EMU3_MOUNTPOINT/t1)" ]
test

cp $EMU3_MOUNTPOINT/t1 $EMU3_MOUNTPOINT/t3
test

mv $EMU3_MOUNTPOINT/t3 $EMU3_MOUNTPOINT/t2
test

[ "$(< $EMU3_MOUNTPOINT/t1)" == "$(< $EMU3_MOUNTPOINT/t2)" ]
test

> $EMU3_MOUNTPOINT/t1
test

[ 0 -eq $(wc -c $EMU3_MOUNTPOINT/t1 | awk '{print $1}') ]
test

head -c 32M </dev/urandom > t3

cp t3 $EMU3_MOUNTPOINT
test

cp t3 $EMU3_MOUNTPOINT/t4
test

echo "Remounting..."
sudo umount $EMU3_MOUNTPOINT
test
sudo mount -t emu3 /dev/loop0 $EMU3_MOUNTPOINT
test

diff $EMU3_MOUNTPOINT/t3 $EMU3_MOUNTPOINT/t4
test

cp $EMU3_MOUNTPOINT/t3 t3.bak
test

diff t3 t3.bak
test
rm t3 t3.bak

rm $EMU3_MOUNTPOINT/t*
test

echo "Cleaning up..."
sudo umount $EMU3_MOUNTPOINT
sudo losetup -d /dev/loop0
rm image.iso

v=0
[ $ok -ne $total ] && v=1

exit $v
