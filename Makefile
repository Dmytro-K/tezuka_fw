# Developer helpers. The firmware itself is built with ./build.sh.
#
#   make flash IP=10.10.11.20
#       copy the SD boot files to the running board (its SD card is
#       mounted at /boot by S20fat-mount), sync and reboot
#
# Optional variables:
#   BOARD   board output to take images from (default plutoskyr2)
#   FILES   files from output/$(BOARD)/images/sdimg to copy
#           (default: kernel, rootfs and devicetree; BOOT.bin and the
#           bitstream are left alone so an overclocked BOOT.bin survives)
#   SSH_USER  ssh user (default root)
#   REBOOT  0 = copy and sync only, no reboot (default 1)

BOARD ?= plutoskyr2
FILES ?= uImage uramdisk.image.xz devicetree.dtb
SSH_USER ?= root
REBOOT ?= 1

SDIMG := output/$(BOARD)/images/sdimg
# scp -O: the board runs dropbear without sftp-server, and OpenSSH >= 9.0
# scp uses the SFTP protocol unless told otherwise.
SSH_TARGET = $(SSH_USER)@$(IP)

.PHONY: flash

flash:
ifndef IP
	$(error IP is not set: make flash IP=<board address>)
endif
	@for f in $(FILES); do \
		test -f "$(SDIMG)/$$f" || { echo "missing $(SDIMG)/$$f (run ./build.sh $(BOARD))"; exit 1; }; \
	done
	scp -O $(addprefix $(SDIMG)/,$(FILES)) $(SSH_TARGET):/boot/
ifeq ($(REBOOT),1)
	ssh $(SSH_TARGET) 'sync && reboot'
else
	ssh $(SSH_TARGET) 'sync'
endif
