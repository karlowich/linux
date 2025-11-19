// SPDX-License-Identifier: GPL-2.0

#include "dmabuf.h"

static struct rb_root uio_dma_tree = RB_ROOT;
static DEFINE_RWLOCK(uio_dma_treelock);

static struct uio_dma_buf_desc *uio_dma_tree_find(struct rb_root *root, int dma_buf_fd)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct uio_dma_buf_desc *this = container_of(node, struct uio_dma_buf_desc, node);

		if (dma_buf_fd < this->dma_buf_fd)
			node = node->rb_left;
		else if (dma_buf_fd > this->dma_buf_fd)
			node = node->rb_right;
		else
			return this;
	}
	return NULL;
}

static int uio_dma_tree_insert(struct rb_root *root, struct uio_dma_buf_desc *desc)
{
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;

	while(*link) {
		struct uio_dma_buf_desc *this = container_of(*link, struct uio_dma_buf_desc, node);
		parent = *link;

		if (desc->dma_buf_fd < this->dma_buf_fd)
			link = &parent->rb_left;
		else if (desc->dma_buf_fd > this->dma_buf_fd)
			link = &parent->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&desc->node, parent, link);
	rb_insert_color(&desc->node, root);

	return 0;
}

void uio_detach_dma_buf(struct uio_dma_buf_desc *desc)
{
	if (!desc)
		return;

	if (desc->sgt)
		dma_buf_unmap_attachment_unlocked(desc->attach, desc->sgt,
						  desc->dir);
	if (desc->attach)
		dma_buf_detach(desc->dma_buf, desc->attach);
	if (desc->dma_buf)
		dma_buf_put(desc->dma_buf);
	if (desc->dev)
		put_device(desc->dev);

	rb_erase(&desc->node, &uio_dma_tree);
	kfree(desc);
}

int uio_attach_dma_buf(struct uio_dma_buf_desc **desc, int dma_buf_fd,
		     struct device *dev, enum dma_data_direction dir)
{
	struct uio_dma_buf_desc *tmp;
	int ret;

	if (WARN_ON_ONCE(!dev))
		return -EFAULT;

	read_lock(&uio_dma_treelock);
	tmp = uio_dma_tree_find(&uio_dma_tree, dma_buf_fd);
	read_unlock(&uio_dma_treelock);
	if (tmp) {
		// Don't fail if dma-buf is already attached
		*desc = tmp;
		return 0;
	}

	tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	tmp->dir = dir;
	tmp->dma_buf = dma_buf_get(dma_buf_fd);
	if (IS_ERR(tmp->dma_buf)) {
		ret = PTR_ERR(tmp->dma_buf);
		tmp->dma_buf = NULL;
		goto err;
	}

	tmp->attach = dma_buf_attach(tmp->dma_buf, dev);
	if (IS_ERR(tmp->attach)) {
		ret = PTR_ERR(tmp->attach);
		tmp->attach = NULL;
		goto err;
	}

	tmp->sgt = dma_buf_map_attachment_unlocked(tmp->attach, dir);
	if (IS_ERR(tmp->sgt)) {
		ret = PTR_ERR(tmp->sgt);
		tmp->sgt = NULL;
		goto err;
	}

	tmp->dir = dir;
	tmp->dev = get_device(dev);
	tmp->dma_buf_fd = dma_buf_fd;

	write_lock(&uio_dma_treelock);
	ret = uio_dma_tree_insert(&uio_dma_tree, tmp);
	write_unlock(&uio_dma_treelock);
	if (ret)
		goto err;

	*desc = tmp;
	return 0;
err:
	uio_detach_dma_buf(tmp);
	return ret;
}

struct uio_dma_buf_desc *uio_get_dma_buf_desc(int dma_buf_fd) {
	struct uio_dma_buf_desc *desc;

	read_lock(&uio_dma_treelock);
	desc = uio_dma_tree_find(&uio_dma_tree, dma_buf_fd);
	read_unlock(&uio_dma_treelock);
	if (!desc)
		return ERR_PTR(-EINVAL);

	return desc;
}
