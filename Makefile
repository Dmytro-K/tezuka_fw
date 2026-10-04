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
#   COPY    rsync (default) or scp. rsync needs rsync on the board too,
#           which images built before it was added to the defconfig do not
#           have: flash such a board once with COPY=scp.
#   SSH_USER  ssh user (default root)
#   REBOOT  0 = copy and sync only, no reboot (default 1)

BOARD ?= plutoskyr2
FILES ?= uImage uramdisk.image.xz devicetree.dtb
COPY ?= rsync
SSH_USER ?= root
REBOOT ?= 1

SDIMG := output/$(BOARD)/images/sdimg
SSH_TARGET = $(SSH_USER)@$(IP)

# /boot is FAT: no owners, permissions or symlinks, and timestamps have a
# 2 s resolution, so only times are kept and compared with that slack.
# rsync writes to a temporary file and renames it, so an aborted copy leaves
# the old file intact.
RSYNC_FLAGS := -rt --no-perms --no-owner --no-group --modify-window=2 --progress
# scp -O: the board runs dropbear without sftp-server, and OpenSSH >= 9.0
# scp uses the SFTP protocol unless told otherwise.
SCP_FLAGS := -O

ifeq ($(COPY),rsync)
COPY_CMD = rsync $(RSYNC_FLAGS)
else ifeq ($(COPY),scp)
COPY_CMD = scp $(SCP_FLAGS)
else
$(error COPY must be rsync or scp, not '$(COPY)')
endif

.PHONY: flash

flash:
ifndef IP
	$(error IP is not set: make flash IP=<board address>)
endif
	@for f in $(FILES); do \
		test -f "$(SDIMG)/$$f" || { echo "missing $(SDIMG)/$$f (run ./build.sh $(BOARD))"; exit 1; }; \
	done
	$(COPY_CMD) $(addprefix $(SDIMG)/,$(FILES)) $(SSH_TARGET):/boot/
ifeq ($(REBOOT),1)
	ssh $(SSH_TARGET) 'sync && reboot'
else
	ssh $(SSH_TARGET) 'sync'
endif
