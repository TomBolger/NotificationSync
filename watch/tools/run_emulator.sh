#!/bin/bash
# run_emulator.sh <platform>: boots a watch emulator with QMP at $EMU_DIR/qmp.sock and the
# phone link on tcp:12344 (for fakephone.py).
P=$1
D=${EMU_DIR:-.}
Q=${QEMU:-qemu-system-arm}
pkill -x qemu-system-arm; sleep 0.5
cp $D/$P/qemu/qemu_spi_flash.bin $D/spi_$P.bin
SPI=$D/spi_$P.bin
case $P in
  basalt) M="-machine pebble-snowy-bb -cpu cortex-m4 -drive if=none,id=spi-flash,file=$SPI,format=raw";;
  diorite) M="-machine pebble-silk-bb -cpu cortex-m4 -drive if=mtd,format=raw,file=$SPI";;
  emery) M="-machine pebble-emery -cpu cortex-m33 -drive if=mtd,format=raw,file=$SPI";;
  gabbro) M="-machine pebble-gabbro -cpu cortex-m33 -drive if=mtd,format=raw,file=$SPI";;
  flint) M="-machine pebble-flint -cpu cortex-m4 -drive if=mtd,format=raw,file=$SPI";;
esac
rm -f $D/qmp.sock
nohup $Q -rtc base=localtime -qmp unix:$D/qmp.sock,server=on,wait=off -serial null -serial tcp::12344,server=on,wait=off -serial tcp::12345,server=on,wait=off -kernel $D/$P/qemu/qemu_micro_flash.bin $M -display none > $D/qemu_$P.log 2>&1 &
echo started $P
