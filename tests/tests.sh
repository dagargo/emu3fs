#!/usr/bin/env bash

[ -z "$EMU3_TEST_DEBUG" ] && EMU3_TEST_DEBUG=0

EMU3_MOUNTPOINT=mountpoint

LANG=C

function cleanUp() {
        echo "Cleaning up..."
        sudo umount -f $EMU3_MOUNTPOINT
        rmdir $EMU3_MOUNTPOINT
        sudo losetup -d /dev/loop0
        rm -f image.iso  image_truncated.iso
}

function logAndRun() {
        echo "Running '$*'..."
        out=$(eval $*)
        err=$?
        [ -n "$out" ] && echo $out
        return $err
}

function printTest() {
        printf "\033[1;34m*** Test: $* ***\033[0m\n"
        echo "emu3fs: *** Test: $* ***" | sudo tee /dev/kmsg
        echo
}

function testCommon() {
  [ $1 -eq 1 ] && ok=$((ok+1))
  total=$((total+1))
  [ $EMU3_TEST_DEBUG -eq 1 ] && [ -n "$2" ] && echo "Listing '$EMU3_MOUNTPOINT/$2'..." && ls -lai $EMU3_MOUNTPOINT/$2
  if [ $1 -eq 1 ]; then
    printf "\033[0;32m"
  else
    printf "\033[0;31m"
  fi
  printf "Results: $ok/$total\033[0m\n\n"
  [ $1 -ne 1 ] && echo "emu3fs: Test error: $(date)" | sudo tee /dev/kmsg && sudo dmesg | tail -n 20 && cleanUp && exit 1
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
mkdir $EMU3_MOUNTPOINT

echo "Inserting module..."
logAndRun sudo modprobe -r emu3_fs
logAndRun sudo modprobe emu3_fs
echo

echo "Uncompressing image..."
logAndRun cp image.iso.xz.bak image.iso.xz
logAndRun sudo rm -f image.iso
logAndRun unxz image.iso.xz
logAndRun sudo losetup /dev/loop0 image.iso
echo

printTest "Very basic emu3 testing"

echo "Mounting image as emu3..."
logAndRun sudo mount -t emu3 /dev/loop0 $EMU3_MOUNTPOINT
test .
logAndRun mkdir $EMU3_MOUNTPOINT/foo
testError .
logAndRun 'echo "123" > $EMU3_MOUNTPOINT/t1'
logAndRun cat $EMU3_MOUNTPOINT/t1
test .
logAndRun '[ "123" == "$out" ]'
test
logAndRun 'rm $EMU3_MOUNTPOINT/t1'
test
logAndRun 'ls $EMU3_MOUNTPOINT/t1'
testError
logAndRun sudo umount $EMU3_MOUNTPOINT
test

printTest "mkdir and rmdir"

echo "Mounting image as emu4..."
sudo mount -t emu4 /dev/loop0 $EMU3_MOUNTPOINT
test .
logAndRun mkdir $EMU3_MOUNTPOINT/foo
test .

logAndRun mkdir $EMU3_MOUNTPOINT/foo
testError .

logAndRun rmdir $EMU3_MOUNTPOINT/foo
test .

logAndRun mkdir $EMU3_MOUNTPOINT/foo
test .

printTest "File creation and basic edition"

logAndRun touch $EMU3_MOUNTPOINT/foo/t0
logAndRun '[ 1024 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t0) ]'
test

logAndRun 'echo "123" > $EMU3_MOUNTPOINT/foo/t1'
test
logAndRun cat $EMU3_MOUNTPOINT/foo/t1
logAndRun '[ "123" == "$out" ]'
test
logAndRun 'echo "4567" >> $EMU3_MOUNTPOINT/foo/t1'
test foo/t1
logAndRun cat $EMU3_MOUNTPOINT/foo/t1
test
logAndRun '[ 123$'\''\n'\''4567 == "$out" ]'
test

printTest "cp"

logAndRun cp $EMU3_MOUNTPOINT/foo/t1 $EMU3_MOUNTPOINT/foo/t3
test
logAndRun ls -l $EMU3_MOUNTPOINT/foo/t1
test
logAndRun ls -l $EMU3_MOUNTPOINT/foo/t3
test

printTest "mv"

logAndRun mv $EMU3_MOUNTPOINT/foo/t3 $EMU3_MOUNTPOINT/foo/t2
test foo
logAndRun ls -l $EMU3_MOUNTPOINT/foo/t3
testError
logAndRun ls -l $EMU3_MOUNTPOINT/foo/t2
test

logAndRun '[ "$(< $EMU3_MOUNTPOINT/foo/t1)" == "$(< $EMU3_MOUNTPOINT/foo/t2)" ]'
test

printTest "cp with big files"

logAndRun 'head -c 32M </dev/urandom > t3'
logAndRun cp t3 $EMU3_MOUNTPOINT/foo
test
logAndRun ls -l $EMU3_MOUNTPOINT/foo/t3
test
logAndRun '[ $(stat --print "%s" t3) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t3) ]'
test
logAndRun '[ 65536 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t3) ]'
test

logAndRun cp t3 $EMU3_MOUNTPOINT/foo/t4
test
logAndRun ls -l $EMU3_MOUNTPOINT/foo/t4
test
logAndRun '[ $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t3) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t4) ]'
test
logAndRun '[ 65536 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t3) ]'
test

echo "Remounting..."
logAndRun sudo umount $EMU3_MOUNTPOINT
test
logAndRun sudo mount -t emu4 /dev/loop0 $EMU3_MOUNTPOINT
test

logAndRun '[ $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t3) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t4) ]'
test
logAndRun '[ $(stat --print "%s" t3) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t3) ]'
test
logAndRun '[ 65536 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t3) ]'
test

date | tee

logAndRun diff $EMU3_MOUNTPOINT/foo/t3 $EMU3_MOUNTPOINT/foo/t4
test

logAndRun cp $EMU3_MOUNTPOINT/foo/t3 t3.bak
test

logAndRun diff t3 t3.bak
test
rm -f t3 t3.bak

printTest "Truncate"

logAndRun '> $EMU3_MOUNTPOINT/foo/t1'
test foo/t1

logAndRun '[ 0 -eq $(wc -c $EMU3_MOUNTPOINT/foo/t1 | awk '\''{print $1}'\'') ]'
test

logAndRun '> $EMU3_MOUNTPOINT/foo/t3'
logAndRun '[ $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t3) -eq 0 ]'
test
logAndRun '[ 1024 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t3) ]'
test

logAndRun rm $EMU3_MOUNTPOINT/foo/t*
test foo

logAndRun rmdir $EMU3_MOUNTPOINT/foo
test .

printTest "cp with other size files (0 and non multiple of 512)..."

logAndRun mkdir $EMU3_MOUNTPOINT/foo
test .

logAndRun touch t5
logAndRun cp t5 $EMU3_MOUNTPOINT/foo
test foo/t5

logAndRun 'head -c 1234567 </dev/urandom > t6'
logAndRun cp t6 $EMU3_MOUNTPOINT/foo
test foo/t6

logAndRun '[ $(stat --print "%s" t5) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t5) ]'
test
logAndRun '[ $(stat --print "%s" t6) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t6) ]'
test

logAndRun diff t5 $EMU3_MOUNTPOINT/foo/t5
test
logAndRun diff t6 $EMU3_MOUNTPOINT/foo/t6
test

logAndRun '[ 1024 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t5) ]'
test
logAndRun '[ 3072 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t6) ]'
test

logAndRun diff t5 $EMU3_MOUNTPOINT/foo/t5
test
logAndRun diff t6 $EMU3_MOUNTPOINT/foo/t6
test

echo "Remounting..."
logAndRun sudo umount $EMU3_MOUNTPOINT
test
logAndRun sudo mount -t emu4 /dev/loop0 $EMU3_MOUNTPOINT
test

logAndRun '[ $(stat --print "%s" t5) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t5) ]'
test
logAndRun '[ $(stat --print "%s" t6) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/foo/t6) ]'
test
logAndRun '[ 1024 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t5) ]'
test
logAndRun '[ 3072 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/foo/t6) ]'
test

logAndRun diff t5 $EMU3_MOUNTPOINT/foo/t5
test
logAndRun diff t6 $EMU3_MOUNTPOINT/foo/t6
test

logAndRun rm t5 t6

printTest "Directory expansion"

logAndRun mkdir $EMU3_MOUNTPOINT/expansion
test expansion
logAndRun '[ 1 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/expansion) ]'
test

for i in $(seq 1 16); do
        name=f-${i}
        logAndRun touch $EMU3_MOUNTPOINT/expansion/$name
        test expansion/$name
done

logAndRun '[ 512 -eq $(stat --print "%s" $EMU3_MOUNTPOINT/expansion) ]'
test
logAndRun '[ 1 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/expansion) ]'
test

logAndRun touch $EMU3_MOUNTPOINT/expansion/f-17
test expansion/f-17

logAndRun '[ $((2*512)) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/expansion) ]'
test
logAndRun '[ 2 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/expansion) ]'
test

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
logAndRun 'echo "12345" > $EMU3_MOUNTPOINT/d1/t1'
logAndRun cat $EMU3_MOUNTPOINT/d1/t1
test d1

printTest "Directory full"

logAndRun mkdir $EMU3_MOUNTPOINT/full
test full
for i in $(seq 1 112); do
        name=f-${i}
        logAndRun touch $EMU3_MOUNTPOINT/full/$name
        test full/$name
done
logAndRun touch $EMU3_MOUNTPOINT/full/error
testError full

logAndRun '[ $((7*512)) -eq $(stat --print "%s" $EMU3_MOUNTPOINT/full) ]'
test
logAndRun '[ 7 -eq $(stat --print "%b" $EMU3_MOUNTPOINT/full) ]'
test

printTest "mv (rename)"

logAndRun mv $EMU3_MOUNTPOINT/d1/t1 $EMU3_MOUNTPOINT/d1/t2
test d1
logAndRun cat $EMU3_MOUNTPOINT/d1/t2
logAndRun ls -li $EMU3_MOUNTPOINT/d1/t2
test d1
logAndRun ls -li $EMU3_MOUNTPOINT/d1/t1
testError

logAndRun touch $EMU3_MOUNTPOINT/d2/t2
logAndRun ls -li $EMU3_MOUNTPOINT/d2
logAndRun mv $EMU3_MOUNTPOINT/d1/t2 $EMU3_MOUNTPOINT/d2
test d2
logAndRun cat $EMU3_MOUNTPOINT/d2/t2
logAndRun ls -li $EMU3_MOUNTPOINT/d2/t2
test d2
logAndRun ls -li $EMU3_MOUNTPOINT/d1/t1
testError d1

logAndRun rm $EMU3_MOUNTPOINT/d2/t2
logAndRun 'echo "1234567" > $EMU3_MOUNTPOINT/d1/t2'
logAndRun mv $EMU3_MOUNTPOINT/d1/t2 $EMU3_MOUNTPOINT/d2
test d2
logAndRun cat $EMU3_MOUNTPOINT/d2/t2
logAndRun ls -li $EMU3_MOUNTPOINT/d2/t2
test d2
logAndRun ls -li $EMU3_MOUNTPOINT/d1/t1
testError d1

logAndRun mv $EMU3_MOUNTPOINT/d1 $EMU3_MOUNTPOINT/d2
testError

printTest "Extended attributes (bank number)"

logAndRun 'getfattr -n "user.bank.number" $EMU3_MOUNTPOINT/d2/t2 2> /dev/null | awk -F\" '\''{print $2}'\'''
test
logAndRun '[ $out -eq 0 ]'
test
logAndRun setfattr -n "user.bank.number" -v 111 $EMU3_MOUNTPOINT/d2/t2
test
logAndRun 'getfattr -n "user.bank.number" $EMU3_MOUNTPOINT/d2/t2 2> /dev/null | awk -F\" '\''{print $2}'\'''
test
logAndRun '[ $out -eq 111 ]'
test
logAndRun setfattr -n "user.bank.number" -v 112 $EMU3_MOUNTPOINT/d2/t2
testError

logAndRun getfattr -d -m ".*" $EMU3_MOUNTPOINT/d2
test
logAndRun '[ -z "$out" ]'
test
logAndRun getfattr -n "user.bank.number" $EMU3_MOUNTPOINT/d2
testError
logAndRun setfattr -n "user.bank.number" -v 0 $EMU3_MOUNTPOINT/d2
testError

logAndRun getfattr -n "user.foo" $EMU3_MOUNTPOINT/d2/t2
testError
logAndRun setfattr -n "user.foo" -v 0 $EMU3_MOUNTPOINT/d2/t2
testError

logAndRun getfattr -n "user.foo" $EMU3_MOUNTPOINT/d2
testError
logAndRun setfattr -n "user.foo" -v 0 $EMU3_MOUNTPOINT/d2
testError

logAndRun setfattr -n "user.bank.number" -v foo $EMU3_MOUNTPOINT/d2/t2
testError

logAndRun sudo umount $EMU3_MOUNTPOINT
logAndRun sudo losetup -d /dev/loop0
echo

echo "Uncompressing truncated image..."
logAndRun cp image_truncated.iso.xz.bak image_truncated.iso.xz
logAndRun sudo rm -f image_truncated.iso
logAndRun unxz image_truncated.iso.xz
logAndRun sudo losetup /dev/loop0 image_truncated.iso
echo

printTest "Mounting truncated image"

logAndRun sudo mount -t emu3 /dev/loop0 $EMU3_MOUNTPOINT
testError

cleanUp

v=0
[ $ok -ne $total ] && v=1

exit $v
