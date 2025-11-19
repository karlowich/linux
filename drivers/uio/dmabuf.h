// SPDX-License-Identifier: GPL-2.0

#ifndef UIO_DMABUF_H
#define UIO_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/rbtree.h>

struct uio_dma_buf_desc {
	int				dma_buf_fd;
	enum dma_data_direction		dir;
	struct dma_buf_attachment	*attach;
	struct dma_buf			*dma_buf;
	struct sg_table			*sgt;
	struct device			*dev;
	struct rb_node			node;
};

void uio_detach_dma_buf(struct uio_dma_buf_desc *desc);
int uio_attach_dma_buf(struct uio_dma_buf_desc **desc, int dma_buf_fd,
		     struct device *dev, enum dma_data_direction dir);
struct uio_dma_buf_desc *uio_get_dma_buf_desc(int dma_buf_fd);

#endif /* UIO_DMABUF_H */