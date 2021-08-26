#!/usr/bin/env bash

[ -z "$EMU3_TEST_DEBUG" ] && EMU3_TEST_DEBUG=0

if [ "$EMU3_MOUNTPOINT" == "" ]; then
  echo "The variable EMU3_MOUNTPOINT must be set"
  exit -1
fi

function logAndRun() {
        echo ">>> Running '$*'..."
        $*
}

function testCommon() {
  [ $1 -eq 1 ] && ok=$((ok+1))
  total=$((total+1))
  [ $EMU3_TEST_DEBUG -eq 1 ] && [ -n "$2" ] && echo "Listing '$EMU3_MOUNTPOINT/$2'..." && ls -lai $EMU3_MOUNTPOINT/$2
  echo "Results: $ok/$total"
  echo
  [ $1 -ne 1 ] && exit 1
}

function test() {
  [ $? -eq 0 ] && this=1 || this=0
  testCommon $this $1
}

function testError() {
  [ $? -ne 0 ] && this=1 || this=0
  testCommon $this $1
}

total=0
ok=0

echo "Inserting module..."
sudo modprobe -r emu3_fs
sudo modprobe emu3_fs

echo "Uncompressing image..."
cp image.iso.xz.bak image.iso.xz
sudo rm -f image.iso
unxz image.iso.xz
sudo losetup /dev/loop0 image.iso

echo "Mounting image as emu3..."
sudo mount -t emu3 /dev/loop0 $EMU3_MOUNTPOINT
sudo umount $EMU3_MOUNTPOINT

echo "Mounting image as emu4..."
sudo mount -t emu4 /dev/loop0 $EMU3_MOUNTPOINT

logAndRun mkdir $EMU3_MOUNTPOINT/foo
test .

logAndRun mkdir $EMU3_MOUNTPOINT/foo
testError

logAndRun rmdir $EMU3_MOUNTPOINT/foo
test .

logAndRun mkdir $EMU3_MOUNTPOINT/foo
test

echo "1234" > $EMU3_MOUNTPOINT/foo/t1
test

logAndRun cat $EMU3_MOUNTPOINT/foo/t1
echo "5678" >> $EMU3_MOUNTPOINT/foo/t1
test
logAndRun cat $EMU3_MOUNTPOINT/foo/t1
[ "12345678" != "$(< $EMU3_MOUNTPOINT/foo/t1)" ]
test foo/t1

logAndRun cp $EMU3_MOUNTPOINT/foo/t1 $EMU3_MOUNTPOINT/foo/t3
test foo/t3

logAndRun mv $EMU3_MOUNTPOINT/foo/t3 $EMU3_MOUNTPOINT/foo/t2
test foo/t2

[ "$(< $EMU3_MOUNTPOINT/foo/t1)" == "$(< $EMU3_MOUNTPOINT/foo/t2)" ]
test

> $EMU3_MOUNTPOINT/foo/t1
test foo/t2

[ 0 -eq $(wc -c $EMU3_MOUNTPOINT/foo/t1 | awk '{print $1}') ]
test

head -c 32M </dev/urandom > t3

logAndRun cp t3 $EMU3_MOUNTPOINT/foo
test foo/t2

logAndRun cp t3 $EMU3_MOUNTPOINT/foo/t4
test foo/t2

echo "Remounting..."
logAndRun sudo umount $EMU3_MOUNTPOINT
test
logAndRun sudo mount -t emu4 /dev/loop0 $EMU3_MOUNTPOINT
test

logAndRun diff $EMU3_MOUNTPOINT/foo/t3 $EMU3_MOUNTPOINT/foo/t4
test

logAndRun cp $EMU3_MOUNTPOINT/foo/t3 t3.bak
test

logAndRun diff t3 t3.bak
test
logAndRun rm t3 t3.bak
test

logAndRun rm $EMU3_MOUNTPOINT/foo/t*
test foo

logAndRun rmdir $EMU3_MOUNTPOINT/foo
test .

# Testing dir expansion

logAndRun mkdir $EMU3_MOUNTPOINT/expansion
test expansion

for i in $(seq 1 16); do
        name=f-${i}
        logAndRun touch $EMU3_MOUNTPOINT/expansion/$name
        test expansion/$name
done

logAndRun touch $EMU3_MOUNTPOINT/expansion/f-17
test expansion

logAndRun rm -rf $EMU3_MOUNTPOINT/expansion
test .

logAndRun mkdir $EMU3_MOUNTPOINT/src
test .
logAndRun mv $EMU3_MOUNTPOINT/src $EMU3_MOUNTPOINT/dst
logAndRun ls -l $EMU3_MOUNTPOINT/dst
test .
logAndRun ls -l $EMU3_MOUNTPOINT/src
testError

logAndRun mkdir $EMU3_MOUNTPOINT/d1
test
logAndRun mkdir $EMU3_MOUNTPOINT/d2
test
echo "1234" > $EMU3_MOUNTPOINT/d1/t1
test d1

logAndRun mv $EMU3_MOUNTPOINT/d1/t1 $EMU3_MOUNTPOINT/d1/t2
test d1
logAndRun ls -li $EMU3_MOUNTPOINT/d1/t2
test
logAndRun ls -li $EMU3_MOUNTPOINT/d1/t1
testError

# This operation is not alloweb by the emu3 filesystem.
logAndRun mv $EMU3_MOUNTPOINT/d1/t2 $EMU3_MOUNTPOINT/d2
testError d2
logAndRun ls -li $EMU3_MOUNTPOINT/d1/t2
test d1

echo "12345678" > $EMU3_MOUNTPOINT/d1/t2
test d1
logAndRun ls -li $EMU3_MOUNTPOINT/d2/t2
logAndRun cp $EMU3_MOUNTPOINT/d1/t2 $EMU3_MOUNTPOINT/d2/t2
test d2
logAndRun ls -li $EMU3_MOUNTPOINT/d2/t2
test
[ "12345678" == "$(< $EMU3_MOUNTPOINT/d2/t2)" ]

logAndRun mv $EMU3_MOUNTPOINT/d1 $EMU3_MOUNTPOINT/d2
testError

echo "Cleaning up..."
sudo umount $EMU3_MOUNTPOINT
sudo losetup -d /dev/loop0
rm image.iso

v=0
[ $ok -ne $total ] && v=1

exit $v
