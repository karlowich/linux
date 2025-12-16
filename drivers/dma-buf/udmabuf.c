// SPDX-License-Identifier: GPL-2.0
#include <linux/cred.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memfd.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/udmabuf.h>
#include <linux/vmalloc.h>
#include <linux/iosys-map.h>
#include <linux/rbtree.h>

static int list_limit = 1024;
module_param(list_limit, int, 0644);
MODULE_PARM_DESC(list_limit, "udmabuf_create_list->count limit. Default is 1024.");

static int size_limit_mb = 64;
module_param(size_limit_mb, int, 0644);
MODULE_PARM_DESC(size_limit_mb, "Max size of a dmabuf, in megabytes. Default is 64.");

struct udmabuf {
	pgoff_t pagecount;
	struct page **pages;
	struct sg_table *sg;
	struct miscdevice *device;
};

static vm_fault_t udmabuf_vm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct udmabuf *ubuf = vma->vm_private_data;
	pgoff_t pgoff = vmf->pgoff;

	if (pgoff >= ubuf->pagecount)
		return VM_FAULT_SIGBUS;
	vmf->page = ubuf->pages[pgoff];
	get_page(vmf->page);
	return 0;
}

static const struct vm_operations_struct udmabuf_vm_ops = {
	.fault = udmabuf_vm_fault,
};

static int mmap_udmabuf(struct dma_buf *buf, struct vm_area_struct *vma)
{
	struct udmabuf *ubuf = buf->priv;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
		return -EINVAL;

	vma->vm_ops = &udmabuf_vm_ops;
	vma->vm_private_data = ubuf;
	return 0;
}

static int vmap_udmabuf(struct dma_buf *buf, struct iosys_map *map)
{
	struct udmabuf *ubuf = buf->priv;
	void *vaddr;

	dma_resv_assert_held(buf->resv);

	vaddr = vm_map_ram(ubuf->pages, ubuf->pagecount, -1);
	if (!vaddr)
		return -EINVAL;

	iosys_map_set_vaddr(map, vaddr);
	return 0;
}

static void vunmap_udmabuf(struct dma_buf *buf, struct iosys_map *map)
{
	struct udmabuf *ubuf = buf->priv;

	dma_resv_assert_held(buf->resv);

	vm_unmap_ram(map->vaddr, ubuf->pagecount);
}

static struct sg_table *get_sg_table(struct device *dev, struct dma_buf *buf,
				     enum dma_data_direction direction)
{
	struct udmabuf *ubuf = buf->priv;
	struct sg_table *sg;
	int ret;

	sg = kzalloc(sizeof(*sg), GFP_KERNEL);
	if (!sg)
		return ERR_PTR(-ENOMEM);
	ret = sg_alloc_table_from_pages(sg, ubuf->pages, ubuf->pagecount,
					0, ubuf->pagecount << PAGE_SHIFT,
					GFP_KERNEL);
	if (ret < 0)
		goto err;
	ret = dma_map_sgtable(dev, sg, direction, 0);
	if (ret < 0)
		goto err;
	return sg;

err:
	sg_free_table(sg);
	kfree(sg);
	return ERR_PTR(ret);
}

static void put_sg_table(struct device *dev, struct sg_table *sg,
			 enum dma_data_direction direction)
{
	dma_unmap_sgtable(dev, sg, direction, 0);
	sg_free_table(sg);
	kfree(sg);
}

static struct sg_table *map_udmabuf(struct dma_buf_attachment *at,
				    enum dma_data_direction direction)
{
	return get_sg_table(at->dev, at->dmabuf, direction);
}

static void unmap_udmabuf(struct dma_buf_attachment *at,
			  struct sg_table *sg,
			  enum dma_data_direction direction)
{
	return put_sg_table(at->dev, sg, direction);
}

static void release_udmabuf(struct dma_buf *buf)
{
	struct udmabuf *ubuf = buf->priv;
	struct device *dev = ubuf->device->this_device;
	pgoff_t pg;

	if (ubuf->sg)
		put_sg_table(dev, ubuf->sg, DMA_BIDIRECTIONAL);

	for (pg = 0; pg < ubuf->pagecount; pg++)
		put_page(ubuf->pages[pg]);
	kfree(ubuf->pages);
	kfree(ubuf);
}

static int begin_cpu_udmabuf(struct dma_buf *buf,
			     enum dma_data_direction direction)
{
	struct udmabuf *ubuf = buf->priv;
	struct device *dev = ubuf->device->this_device;
	int ret = 0;

	if (!ubuf->sg) {
		ubuf->sg = get_sg_table(dev, buf, direction);
		if (IS_ERR(ubuf->sg)) {
			ret = PTR_ERR(ubuf->sg);
			ubuf->sg = NULL;
		}
	} else {
		dma_sync_sg_for_cpu(dev, ubuf->sg->sgl, ubuf->sg->nents,
				    direction);
	}

	return ret;
}

static int end_cpu_udmabuf(struct dma_buf *buf,
			   enum dma_data_direction direction)
{
	struct udmabuf *ubuf = buf->priv;
	struct device *dev = ubuf->device->this_device;

	if (!ubuf->sg)
		return -EINVAL;

	dma_sync_sg_for_device(dev, ubuf->sg->sgl, ubuf->sg->nents, direction);
	return 0;
}

static const struct dma_buf_ops udmabuf_ops = {
	.cache_sgt_mapping = true,
	.map_dma_buf	   = map_udmabuf,
	.unmap_dma_buf	   = unmap_udmabuf,
	.release	   = release_udmabuf,
	.mmap		   = mmap_udmabuf,
	.vmap		   = vmap_udmabuf,
	.vunmap		   = vunmap_udmabuf,
	.begin_cpu_access  = begin_cpu_udmabuf,
	.end_cpu_access    = end_cpu_udmabuf,
};

#define SEALS_WANTED (F_SEAL_SHRINK)
#define SEALS_DENIED (F_SEAL_WRITE)

static long udmabuf_create(struct miscdevice *device,
			   struct udmabuf_create_list *head,
			   struct udmabuf_create_item *list)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct file *memfd = NULL;
	struct address_space *mapping = NULL;
	struct udmabuf *ubuf;
	struct dma_buf *buf;
	pgoff_t pgoff, pgcnt, pgidx, pgbuf = 0, pglimit;
	struct page *page;
	int seals, ret = -EINVAL;
	u32 i, flags;

	ubuf = kzalloc(sizeof(*ubuf), GFP_KERNEL);
	if (!ubuf)
		return -ENOMEM;

	pglimit = (size_limit_mb * 1024 * 1024) >> PAGE_SHIFT;
	for (i = 0; i < head->count; i++) {
		if (!IS_ALIGNED(list[i].offset, PAGE_SIZE))
			goto err;
		if (!IS_ALIGNED(list[i].size, PAGE_SIZE))
			goto err;
		ubuf->pagecount += list[i].size >> PAGE_SHIFT;
		if (ubuf->pagecount > pglimit)
			goto err;
	}

	if (!ubuf->pagecount)
		goto err;

	ubuf->pages = kmalloc_array(ubuf->pagecount, sizeof(*ubuf->pages),
				    GFP_KERNEL);
	if (!ubuf->pages) {
		ret = -ENOMEM;
		goto err;
	}

	pgbuf = 0;
	for (i = 0; i < head->count; i++) {
		ret = -EBADFD;
		memfd = fget(list[i].memfd);
		if (!memfd)
			goto err;
		mapping = memfd->f_mapping;
		if (!shmem_mapping(mapping))
			goto err;
		seals = memfd_fcntl(memfd, F_GET_SEALS, 0);
		if (seals == -EINVAL)
			goto err;
		ret = -EINVAL;
		if ((seals & SEALS_WANTED) != SEALS_WANTED ||
		    (seals & SEALS_DENIED) != 0)
			goto err;
		pgoff = list[i].offset >> PAGE_SHIFT;
		pgcnt = list[i].size   >> PAGE_SHIFT;
		for (pgidx = 0; pgidx < pgcnt; pgidx++) {
			page = shmem_read_mapping_page(mapping, pgoff + pgidx);
			if (IS_ERR(page)) {
				ret = PTR_ERR(page);
				goto err;
			}
			ubuf->pages[pgbuf++] = page;
		}
		fput(memfd);
		memfd = NULL;
	}

	exp_info.ops  = &udmabuf_ops;
	exp_info.size = ubuf->pagecount << PAGE_SHIFT;
	exp_info.priv = ubuf;
	exp_info.flags = O_RDWR;

	ubuf->device = device;
	buf = dma_buf_export(&exp_info);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto err;
	}

	flags = 0;
	if (head->flags & UDMABUF_FLAGS_CLOEXEC)
		flags |= O_CLOEXEC;
	return dma_buf_fd(buf, flags);

err:
	while (pgbuf > 0)
		put_page(ubuf->pages[--pgbuf]);
	if (memfd)
		fput(memfd);
	kfree(ubuf->pages);
	kfree(ubuf);
	return ret;
}

static long udmabuf_ioctl_create(struct file *filp, unsigned long arg)
{
	struct udmabuf_create create;
	struct udmabuf_create_list head;
	struct udmabuf_create_item list;

	if (copy_from_user(&create, (void __user *)arg,
			   sizeof(create)))
		return -EFAULT;

	head.flags  = create.flags;
	head.count  = 1;
	list.memfd  = create.memfd;
	list.offset = create.offset;
	list.size   = create.size;

	return udmabuf_create(filp->private_data, &head, &list);
}

static long udmabuf_ioctl_create_list(struct file *filp, unsigned long arg)
{
	struct udmabuf_create_list head;
	struct udmabuf_create_item *list;
	int ret = -EINVAL;
	u32 lsize;

	if (copy_from_user(&head, (void __user *)arg, sizeof(head)))
		return -EFAULT;
	if (head.count > list_limit)
		return -EINVAL;
	lsize = sizeof(struct udmabuf_create_item) * head.count;
	list = memdup_user((void __user *)(arg + sizeof(head)), lsize);
	if (IS_ERR(list))
		return PTR_ERR(list);

	ret = udmabuf_create(filp->private_data, &head, list);
	kfree(list);
	return ret;
}

static struct rb_root udmabuf_dma_tree = RB_ROOT;
static DEFINE_RWLOCK(udmabuf_dma_treelock);

struct udmabuf_dma_buf_desc {
	int				dma_buf_fd;
	enum dma_data_direction		dir;
	struct dma_buf_attachment	*attach;
	struct dma_buf			*dma_buf;
	struct sg_table			*sgt;
	struct rb_node			node;
};

static struct udmabuf_dma_buf_desc *udmabuf_dma_tree_find(struct rb_root *root, int dma_buf_fd)
{
	struct udmabuf_dma_buf_desc *this;
	struct rb_node *node = root->rb_node;

	while (node) {
		this = container_of(node, struct udmabuf_dma_buf_desc, node);

		if (dma_buf_fd < this->dma_buf_fd)
			node = node->rb_left;
		else if (dma_buf_fd > this->dma_buf_fd)
			node = node->rb_right;
		else
			return this;
	}
	return NULL;
}

static int udmabuf_dma_tree_insert(struct rb_root *root, struct udmabuf_dma_buf_desc *desc)
{
	struct udmabuf_dma_buf_desc *this;
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;

	while(*link) {
		this = container_of(*link, struct udmabuf_dma_buf_desc, node);
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

static struct udmabuf_dma_buf_desc *udmabuf_get_dma_buf_desc(int dma_buf_fd)
{
	struct udmabuf_dma_buf_desc *desc;

	read_lock(&udmabuf_dma_treelock);
	desc = udmabuf_dma_tree_find(&udmabuf_dma_tree, dma_buf_fd);
	read_unlock(&udmabuf_dma_treelock);
	if (!desc)
		return ERR_PTR(-EINVAL);

	return desc;
}

static void udmabuf_detach(struct udmabuf_dma_buf_desc *desc)
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

	rb_erase(&desc->node, &udmabuf_dma_tree);
	kfree(desc);
}

static int udmabuf_attach(struct udmabuf_dma_buf_desc **desc, int dma_buf_fd,
                   struct device *dev, enum dma_data_direction dir)
{
	struct udmabuf_dma_buf_desc *tmp;
	int ret;

	if (WARN_ON_ONCE(!dev))
		return -EFAULT;

	read_lock(&udmabuf_dma_treelock);
	tmp = udmabuf_dma_tree_find(&udmabuf_dma_tree, dma_buf_fd);
	read_unlock(&udmabuf_dma_treelock);
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
	tmp->dma_buf_fd = dma_buf_fd;

	write_lock(&udmabuf_dma_treelock);
	ret = udmabuf_dma_tree_insert(&udmabuf_dma_tree, tmp);
	write_unlock(&udmabuf_dma_treelock);
	if (ret)
		goto err;

	*desc = tmp;
	return 0;
err:
	udmabuf_detach(tmp);
	return ret;
}

static long udmabuf_ioctl_attach(struct file *filp, unsigned long arg)
{
	struct miscdevice *misc = filp->private_data;
	struct device *dev = misc->this_device;
	struct udmabuf_attach __user *uattach;
	struct udmabuf_attach attach;
	struct udmabuf_dma_buf_desc *desc;
	int ret;

	uattach = (void __user *)arg;

	if (copy_from_user(&attach, uattach, sizeof(attach)))
		return -EFAULT;

	ret = udmabuf_attach(&desc, attach.fd, dev, DMA_BIDIRECTIONAL);
	if (ret) 
		return ret;

	attach.count = desc->sgt->nents;

	if (copy_to_user(uattach, &attach, sizeof(attach)))
		return -EFAULT;

	return 0;
}

static long udmabuf_ioctl_detach(struct file *filp, unsigned long arg)
{
	struct udmabuf_dma_buf_desc *desc;
	int fd;

	if (copy_from_user(&fd, (void __user *)arg, sizeof(fd)))
		return -EFAULT;

	desc = udmabuf_get_dma_buf_desc(fd);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	udmabuf_detach(desc);
	return 0;
}

static long udmabuf_ioctl_get_map(struct file *filp, unsigned long arg)
{
	struct udmabuf_dma_buf_desc *desc;
	struct udmabuf_get_map __user *uget_map;
	struct udmabuf_get_map get_map;
	struct udmabuf_dma_map map;
	struct udmabuf_dma_map __user *umap;
	struct scatterlist *sg;
	int i;

	uget_map = (void __user *)arg;

	if (copy_from_user(&get_map, uget_map, sizeof(get_map)))
		return -EFAULT;

	desc = udmabuf_get_dma_buf_desc(get_map.fd);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	umap = uget_map->dma_arr;
	for_each_sgtable_dma_sg(desc->sgt, sg, i) {
		if (i > get_map.count)
			break;

		map.dma_addr = sg_dma_address(sg);
		map.dma_len = sg_dma_len(sg);
		if (copy_to_user(umap + i, &map, sizeof(map)))
			return -EFAULT;
	}

	return 0;
}

static long udmabuf_ioctl(struct file *filp, unsigned int ioctl,
			  unsigned long arg)
{
	long ret;

	switch (ioctl) {
	case UDMABUF_CREATE:
		ret = udmabuf_ioctl_create(filp, arg);
		break;
	case UDMABUF_CREATE_LIST:
		ret = udmabuf_ioctl_create_list(filp, arg);
		break;
	case UDMABUF_ATTACH:
		ret = udmabuf_ioctl_attach(filp, arg);
		break;
	case UDMABUF_DETACH:
		ret = udmabuf_ioctl_detach(filp, arg);
		break;
	case UDMABUF_GET_MAP:
		ret = udmabuf_ioctl_get_map(filp, arg);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static const struct file_operations udmabuf_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = udmabuf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = udmabuf_ioctl,
#endif
};

static struct miscdevice udmabuf_misc = {
	.minor          = MISC_DYNAMIC_MINOR,
	.name           = "udmabuf",
	.fops           = &udmabuf_fops,
};

static int __init udmabuf_dev_init(void)
{
	int ret;

	ret = misc_register(&udmabuf_misc);
	if (ret < 0) {
		pr_err("Could not initialize udmabuf device\n");
		return ret;
	}

	ret = dma_coerce_mask_and_coherent(udmabuf_misc.this_device,
					   DMA_BIT_MASK(64));
	if (ret < 0) {
		pr_err("Could not setup DMA mask for udmabuf device\n");
		misc_deregister(&udmabuf_misc);
		return ret;
	}

	return 0;
}

static void __exit udmabuf_dev_exit(void)
{
	misc_deregister(&udmabuf_misc);
}

module_init(udmabuf_dev_init)
module_exit(udmabuf_dev_exit)

MODULE_AUTHOR("Gerd Hoffmann <kraxel@redhat.com>");
