emu3fs
======

emu3fs is a linux kernel module that allows to read from and write to disks formatted in an E-MU emu3 sampler family filesystem. The samplers using this filesystem are the emulator 3, emulator 3x and the ESI family.

Currently, it has been tested to work with CDs and Zip drives.

It's distributed under the terms of the GPL v3.

Installation
------------

Simply run "make && sudo make install".

Note that you will need the linux kernel headers to compile the module. 
