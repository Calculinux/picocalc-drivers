/* SPDX-License-Identifier: GPL-2.0 */
/* Minimal resource table for Linux remoteproc ELF loader */

#include "stdint.h"

#define RSC_TRACE	0
#define RSC_DEVMEM	1
#define RSC_DEVMEM_NOMAP	2
#define RSC_VDEV	3

struct resource_table {
	uint32_t ver;
	uint32_t num;
	uint32_t reserved[2];
	uint32_t offset[];
} __attribute__((packed));

/* Place in .resource_table section so kernel finds it */
__attribute__((section(".resource_table"), used))
struct resource_table resource_table = {
	.ver = 1,
	.num = 0,
	.reserved = { 0, 0 },
};
