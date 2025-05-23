# SPDX-License-Identifier: GPL-2.0-only
################################################################################
#
# r8126 is the Linux device driver released for Realtek 5 Gigabit Ethernet
# controllers with PCI-Express interface.
#
# Copyright(c) 2024 Realtek Semiconductor Corp. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, see <http://www.gnu.org/licenses/>.
#
# Author:
# Realtek NIC software team <nicfae@realtek.com>
# No. 2, Innovation Road II, Hsinchu Science Park, Hsinchu 300, Taiwan
#
################################################################################

################################################################################
#  This product is covered by one or more of the following patents:
#  US6,570,884, US6,115,776, and US6,327,625.
################################################################################

LINUX_VERSION := $(shell expr $(VERSION) \* 256 + $(PATCHLEVEL))
LINUX_VERSION_6_9 := $(shell expr 6 \* 256 + 9)

# Use dummy R8126 driver for Kernel versions greater that K69
ifeq ($(shell test $(LINUX_VERSION) -gt $(LINUX_VERSION_6_9); echo $$?),0)
obj-m := r8126.o
r8126-objs := r8126_dummy.o
else

CONFIG_SOC_LAN = n
ENABLE_REALWOW_SUPPORT = n
ENABLE_DASH_SUPPORT = n
ENABLE_DASH_PRINTER_SUPPORT = n
CONFIG_DOWN_SPEED_100 = n
CONFIG_ASPM = y
ENABLE_S5WOL = y
ENABLE_S5_KEEP_CURR_MAC = n
ENABLE_EEE = y
ENABLE_S0_MAGIC_PACKET = n
ENABLE_TX_NO_CLOSE = y
ENABLE_MULTIPLE_TX_QUEUE = y
ENABLE_PTP_SUPPORT = y
ENABLE_PTP_MASTER_MODE = n
ENABLE_RSS_SUPPORT = y
ENABLE_LIB_SUPPORT = n
ENABLE_USE_FIRMWARE_FILE = n
DISABLE_WOL_SUPPORT = n
DISABLE_MULTI_MSIX_VECTOR = n
ENABLE_DOUBLE_VLAN = n
ENABLE_PAGE_REUSE = n
ENABLE_RX_PACKET_FRAGMENT = n

obj-m := r8126.o
r8126-objs := r8126_n.o rtl_eeprom.o rtltool.o
ifeq ($(CONFIG_SOC_LAN), y)
	EXTRA_CFLAGS += -DCONFIG_SOC_LAN
endif
ifeq ($(ENABLE_REALWOW_SUPPORT), y)
	r8126-objs += r8126_realwow.o
	EXTRA_CFLAGS += -DENABLE_REALWOW_SUPPORT
endif
ifeq ($(ENABLE_DASH_SUPPORT), y)
	r8126-objs += r8126_dash.o
	EXTRA_CFLAGS += -DENABLE_DASH_SUPPORT
endif
ifeq ($(ENABLE_DASH_PRINTER_SUPPORT), y)
	r8126-objs += r8126_dash.o
	EXTRA_CFLAGS += -DENABLE_DASH_SUPPORT -DENABLE_DASH_PRINTER_SUPPORT
endif
EXTRA_CFLAGS += -DCONFIG_R8126_NAPI
EXTRA_CFLAGS += -DCONFIG_R8126_VLAN
ifeq ($(CONFIG_DOWN_SPEED_100), y)
	EXTRA_CFLAGS += -DCONFIG_DOWN_SPEED_100
endif
ifeq ($(CONFIG_ASPM), y)
	EXTRA_CFLAGS += -DCONFIG_ASPM
endif
ifeq ($(ENABLE_S5WOL), y)
	EXTRA_CFLAGS += -DENABLE_S5WOL
endif
ifeq ($(ENABLE_S5_KEEP_CURR_MAC), y)
	EXTRA_CFLAGS += -DENABLE_S5_KEEP_CURR_MAC
endif
ifeq ($(ENABLE_EEE), y)
	EXTRA_CFLAGS += -DENABLE_EEE
endif
ifeq ($(ENABLE_S0_MAGIC_PACKET), y)
	EXTRA_CFLAGS += -DENABLE_S0_MAGIC_PACKET
endif
ifeq ($(ENABLE_TX_NO_CLOSE), y)
	EXTRA_CFLAGS += -DENABLE_TX_NO_CLOSE
endif
ifeq ($(ENABLE_MULTIPLE_TX_QUEUE), y)
	EXTRA_CFLAGS += -DENABLE_MULTIPLE_TX_QUEUE
endif
ifeq ($(ENABLE_PTP_SUPPORT), y)
	r8126-objs += r8126_ptp.o
	EXTRA_CFLAGS += -DENABLE_PTP_SUPPORT
endif
ifeq ($(ENABLE_PTP_MASTER_MODE), y)
	EXTRA_CFLAGS += -DENABLE_PTP_MASTER_MODE
endif
ifeq ($(ENABLE_RSS_SUPPORT), y)
	r8126-objs += r8126_rss.o
	EXTRA_CFLAGS += -DENABLE_RSS_SUPPORT
endif
ifeq ($(ENABLE_LIB_SUPPORT), y)
	r8126-objs += r8126_lib.o
	EXTRA_CFLAGS += -DENABLE_LIB_SUPPORT
endif
ifeq ($(ENABLE_USE_FIRMWARE_FILE), y)
	r8126-objs += r8126_firmware.o
	EXTRA_CFLAGS += -DENABLE_USE_FIRMWARE_FILE
endif
ifeq ($(DISABLE_WOL_SUPPORT), y)
	EXTRA_CFLAGS += -DDISABLE_WOL_SUPPORT
endif
ifeq ($(DISABLE_MULTI_MSIX_VECTOR), y)
	EXTRA_CFLAGS += -DDISABLE_MULTI_MSIX_VECTOR
endif
ifeq ($(ENABLE_DOUBLE_VLAN), y)
	EXTRA_CFLAGS += -DENABLE_DOUBLE_VLAN
endif
ifeq ($(ENABLE_PAGE_REUSE), y)
	EXTRA_CFLAGS += -DENABLE_PAGE_REUSE
endif
ifeq ($(ENABLE_RX_PACKET_FRAGMENT), y)
	EXTRA_CFLAGS += -DENABLE_RX_PACKET_FRAGMENT
endif

endif
