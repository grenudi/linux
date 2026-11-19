/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UFS host controller revision, handed to a UFS PHY that needs it.
 */

#ifndef __PHY_UFS_H_
#define __PHY_UFS_H_

#include <linux/types.h>

/**
 * struct phy_configure_opts_ufs - UFS PHY configuration set
 * @hw_ver_major:	Major number of the host's UFS_HW_VERSION (bits 31:28).
 * @hw_ver_minor:	Minor number (bits 27:16).
 * @hw_ver_step:	Step number (bits 15:0).
 *
 * Some QMP UFS PHYs are programmed with a table that depends on the
 * revision of the UFS host they sit next to. The host driver reads the
 * revision and passes it here before the PHY is powered on.
 */
struct phy_configure_opts_ufs {
	u8	hw_ver_major;
	u16	hw_ver_minor;
	u16	hw_ver_step;
};

#endif /* __PHY_UFS_H_ */
