/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_UDMABUF_H
#define _UAPI_LINUX_UDMABUF_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define UDMABUF_FLAGS_CLOEXEC	0x01

struct udmabuf_create {
	__u32 memfd;
	__u32 flags;
	__u64 offset;
	__u64 size;
};

struct udmabuf_create_item {
	__u32 memfd;
	__u32 __pad;
	__u64 offset;
	__u64 size;
};

struct udmabuf_create_list {
	__u32 flags;
	__u32 count;
	struct udmabuf_create_item list[];
};

#define UDMABUF_CREATE       _IOW('u', 0x42, struct udmabuf_create)
#define UDMABUF_CREATE_LIST  _IOW('u', 0x43, struct udmabuf_create_list)

/**
 * struct udmabuf_attach - import dma-buf and return number of addresses
 */
struct udmabuf_attach {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Count of DMA addresses in the dma-buf (out) */
	__u32 count;
};

/**
 * struct udmabuf_dma_map - Representation of DMA mapping
 */
struct udmabuf_dma_map {
	/** @dma_addr: Address to physical page */
	__u64 dma_addr;
	/** @dma_addr: Size of physical page */
	__u64 dma_len;
};

/**
 * struct udmabuf_get_map - Get DMA mappings from provided dma-buf fd
 *
 * The dma-buf fd must match one of the dma-bufs from udmabuf_attach
 * The dma_mappings array must be allocated from userspace
 */
struct udmabuf_get_map {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Size of dma_mappings array (in) */
	__u32 count;
	/** @dma_arr: Array of dma mappings (out) */
	struct udmabuf_dma_map dma_arr[];
};

#define UDMABUF_ATTACH       _IOWR('u', 0x47, struct udmabuf_attach)
#define UDMABUF_DETACH       _IOW('u', 0x48, int)
#define UDMABUF_GET_MAP      _IOWR('u', 0x49, struct udmabuf_get_map)

#endif /* _UAPI_LINUX_UDMABUF_H */
