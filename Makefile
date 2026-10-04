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
#
#   make ssh-key IP=10.10.11.20 KEY=~/.ssh/id_ed25519.pub
#       append the public key(s) in KEY to ~/.ssh/authorized_keys on the
#       board (skipping keys already there) and, for root, save them to
#       /mnt/jffs2 with device_persistent_keys so S21misc restores them
#       after every reboot and reflash. Always logs in with the password:
#       the key is not on the board yet, and offering many agent keys hits
#       dropbear's MaxAuthTries.

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

# Command-line -o wins over ~/.ssh/config, so a Host entry with
# "PasswordAuthentication no" (common once a key is set up) does not block it.
SSH_PASSWORD_ONLY := -o PubkeyAuthentication=no \
	-o PasswordAuthentication=yes -o KbdInteractiveAuthentication=yes \
	-o PreferredAuthentications=password,keyboard-interactive

# make does not expand ~, and zsh does not expand it after KEY= either
KEY_FILE = $(patsubst ~/%,$(HOME)/%,$(KEY))

# The key travels base64-encoded in the command line, not on stdin: with
# stdin redirected and DISPLAY set, ssh asks SSH_ASKPASS for the password
# instead of the terminal, and the login silently fails.
KEY_B64 = $(shell { base64 -w0 < "$(KEY_FILE)"; } 2>/dev/null)

# Runs on the board (busybox sh). No single quotes inside: the whole script
# is passed to ssh in single quotes.
define SSH_KEY_REMOTE
umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys || exit 1; \
printf "%s" "$(KEY_B64)" | base64 -d | while IFS= read -r k || [ -n "$$k" ]; do \
	case "$$k" in "" | "#"*) continue ;; esac; \
	if grep -qxF "$$k" ~/.ssh/authorized_keys; then \
		echo "already present: $${k##* }"; \
	else \
		printf "%s\n" "$$k" >> ~/.ssh/authorized_keys && echo "added: $${k##* }"; \
	fi; \
done; \
chmod 600 ~/.ssh/authorized_keys; \
if [ "$$(id -u)" != 0 ]; then \
	echo "not root: key kept in ~/.ssh only (lost on reboot)"; \
elif grep -q mtd2 /proc/mounts; then \
	device_persistent_keys && echo "saved to /mnt/jffs2 (restored on every boot)"; \
else \
	echo "warning: /mnt/jffs2 is not mounted, the key is lost on reboot (see device_format_jffs2)"; \
fi
endef

.PHONY: flash ssh-key

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

ssh-key:
ifndef IP
	$(error IP is not set: make ssh-key IP=<board address> KEY=<public key file>)
endif
ifndef KEY
	$(error KEY is not set: make ssh-key IP=<board address> KEY=<public key file>)
endif
	@test -f "$(KEY_FILE)" || { echo "$(KEY_FILE): no such file"; exit 1; }
	@! grep -q "PRIVATE KEY" "$(KEY_FILE)" || { echo "$(KEY_FILE) is a private key, pass the .pub file"; exit 1; }
	@grep -qE '^(ssh-|ecdsa-|sk-)' "$(KEY_FILE)" || { echo "$(KEY_FILE) does not look like an OpenSSH public key"; exit 1; }
	ssh $(SSH_PASSWORD_ONLY) $(SSH_TARGET) '$(SSH_KEY_REMOTE)'
