################################################################################
#
# IQNETD
#
################################################################################

IQNETD_VERSION = 1.0
IQNETD_SITE_METHOD = local
IQNETD_SITE = $(BR2_EXTERNAL_PLUTOSDR_PATH)/package/iqnetd/src
IQNETD_LICENSE = MIT
# iqnet-kmod provides /dev/iqnet at runtime and the shared headers
# (iqnet_uapi.h, iqnet_proto.h). The headers are taken straight from the
# kmod package's source dir so the build does not depend on whether the
# kmod also installs them to staging.
IQNETD_DEPENDENCIES = iqnet-kmod
IQNETD_IQNET_INC = $(BR2_EXTERNAL_PLUTOSDR_PATH)/package/iqnet-kmod/src

define IQNETD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) \
		IQNET_INC=$(IQNETD_IQNET_INC)
endef

define IQNETD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/iqnetd $(TARGET_DIR)/usr/sbin/iqnetd
endef

define IQNETD_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(IQNETD_PKGDIR)/S97iqnetd \
		$(TARGET_DIR)/etc/init.d/S97iqnetd
endef

$(eval $(generic-package))
