################################################################################
#
# iqnet-kmod
#
################################################################################

IQNET_KMOD_VERSION = 1.0
IQNET_KMOD_SITE_METHOD = local
IQNET_KMOD_SITE = $(BR2_EXTERNAL_PLUTOSDR_PATH)/package/iqnet-kmod/src
IQNET_KMOD_LICENSE = GPL-2.0 (module), MIT (iqnet_proto.h)
IQNET_KMOD_DEPENDENCIES = linux
IQNET_KMOD_MODULE_MAKE_OPTS = KVERSION=$(LINUX_VERSION_PROBED)

# iqnet_uapi.h (ioctl ABI) and iqnet_proto.h (wire format) are consumed by
# iqnetd; install them into staging so it can #include them.
IQNET_KMOD_INSTALL_STAGING = YES

define IQNET_KMOD_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/iqnet_uapi.h \
		$(STAGING_DIR)/usr/include/iqnet_uapi.h
	$(INSTALL) -D -m 0644 $(@D)/iqnet_proto.h \
		$(STAGING_DIR)/usr/include/iqnet_proto.h
endef

$(eval $(kernel-module))
$(eval $(generic-package))
