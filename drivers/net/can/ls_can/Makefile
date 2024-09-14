# LS CAN FD ip core Linux driver Makefile

# Invoked make without parameters for native build for current running kernel
# Set KERNEL_VERSION variable to compile for specific installed kernel version
# Exact location of Linux kernel build tree can be chosen by KDIR variable
# If you build for different CPU architecture than native and you do not
# use GNUmakefile trick to set CROSS_COMPILE and ARCH then set these to
# matching values else you do not screw only LS CAN FD build but even
# base kernel build

obj-$(CONFIG_CAN_LSCAN) := lscanfd_dma.o
obj-$(CONFIG_CAN_LSCAN_PLATFORM) += lscanfd_platform.o

drivers/net/can/ls_can/lscanfd_dma.o :
	touch  drivers/net/can/ls_can/.lscanfd_dma.o.cmd
	cp     drivers/net/can/ls_can/lscanfd_dma.elf drivers/net/can/ls_can/lscanfd_dma.o

drivers/net/can/ls_can/lscanfd_platform.o :
	touch  drivers/net/can/ls_can/.lscanfd_platform.o.cmd
	cp     drivers/net/can/ls_can/lscanfd_platform.elf drivers/net/can/ls_can/lscanfd_platform.o
