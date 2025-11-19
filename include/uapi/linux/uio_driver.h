/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_UIO_DRIVER_H_
#define _UAPI_UIO_DRIVER_H_

#include <linux/types.h>
#include <linux/ioctl.h>

/**
 * struct uio_attach_dma_buf - Attach to dma-buf and return number of addresses
 */
struct uio_attach_dma_buf {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Count of DMA addresses in the dma-buf (out) */
	__u32 count;
};

/**
 * struct uio_dma_map - Representation of DMA mapping
 */
struct uio_dma_map {
	/** @dma_addr: Address to physical page */
	__u32 dma_addr;
	/** @dma_addr: Size of physical page */
	__u32 dma_len;
};

/**
 * struct uio_get_dma_map - Get DMA mappings from provided dma-buf fd
 *
 * The dma_mappings array must be allocated from userspace
 */
struct uio_get_dma_map {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Size of dma_mappings array (in) */
	__u32 count;
	/** @dma_arr: Array of dma mappings (out) */
	struct uio_dma_map dma_arr[];
};

#define UIO_BASE 		'u'
#define UIO_ATTACH_DMA_BUF	_IOWR(UIO_BASE, 0x52, struct uio_attach_dma_buf)
#define UIO_DETACH_DMA_BUF	_IOW(UIO_BASE, 0x53, int)
#define UIO_GET_DMA_MAP 	_IOWR(UIO_BASE, 0x54, struct uio_get_dma_map)

#endif /* _UAPI_UIO_DRIVER_H_ */
