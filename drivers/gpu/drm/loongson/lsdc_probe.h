/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __LSDC_PROBE_H__
#define __LSDC_PROBE_H__

/* Helpers for chip detection */
unsigned int loongson_cpu_get_prid(u8 *impl, u8 *rev);

enum loongson_chip_id loongson_chip_id_fixup(enum loongson_chip_id chip_id);

#endif
