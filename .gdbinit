set confirm off
set architecture riscv:rv64
symbol-file /home/chu/cheri/output/rootfs-riscv64-hybrid/boot/kernel/kernel.full
set disassemble-next-line auto
set riscv use-compressed-breakpoints yes
target remote 127.0.0.1:26002

