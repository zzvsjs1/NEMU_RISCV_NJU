.section .data
.global ramdisk_start, ramdisk_end
ramdisk_start:
ramdisk_end:

.section .rodata
.globl logo
logo:
.incbin "../nanos-lite/resources/logo.txt"
.byte 0
