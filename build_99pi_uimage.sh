#!/bin/sh

echo Compressing...
lzma -c arch/loongarch/boot/vmlinux.bin > vmlinux.bin.lzma

addr=$(grep 'kernel_entry$' System.map | cut -b9-16)
echo Found kernel_entry at $addr
sed "s/@ENTRY@/$addr/" 99pi.its.in > 99pi.its

echo Building image...
mkimage -f 99pi.its uImage

