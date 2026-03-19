#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/set_memory.h>
#include <linux/slab.h>

struct tdx_shm_object {
	refcount_t refs;
	size_t size;
	unsigned long npages;
	struct page **pages;
};

static void tdx_shm_put(struct tdx_shm_object *obj)
{
	unsigned long i;

	if (!obj || !refcount_dec_and_test(&obj->refs))
		return;

	if (obj->pages) {
		for (i = 0; i < obj->npages; ++i) {
			unsigned long addr;

			if (!obj->pages[i])
				continue;
			addr = (unsigned long)page_to_virt(obj->pages[i]);
			set_memory_encrypted(addr, 1);
			__free_page(obj->pages[i]);
		}
		kfree(obj->pages);
	}
	kfree(obj);
}

static int tdx_shm_open(struct inode *inode, struct file *file)
{
	struct tdx_shm_object *obj;

	(void)inode;
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;
	refcount_set(&obj->refs, 1);
	file->private_data = obj;
	return 0;
}

static int tdx_shm_release(struct inode *inode, struct file *file)
{
	(void)inode;
	tdx_shm_put(file->private_data);
	return 0;
}

static void tdx_shm_vma_open(struct vm_area_struct *vma)
{
	struct tdx_shm_object *obj = vma->vm_private_data;

	refcount_inc(&obj->refs);
}

static void tdx_shm_vma_close(struct vm_area_struct *vma)
{
	struct tdx_shm_object *obj = vma->vm_private_data;

	tdx_shm_put(obj);
}

static const struct vm_operations_struct tdx_shm_vm_ops = {
	.open = tdx_shm_vma_open,
	.close = tdx_shm_vma_close,
};

static int tdx_shm_alloc_pages(struct tdx_shm_object *obj, unsigned long npages)
{
	unsigned long i;

	obj->pages = kcalloc(npages, sizeof(*obj->pages), GFP_KERNEL);
	if (!obj->pages)
		return -ENOMEM;

	obj->npages = npages;
	for (i = 0; i < npages; ++i) {
		unsigned long addr;
		int rc;

		obj->pages[i] = alloc_page(GFP_HIGHUSER | __GFP_ZERO);
		if (!obj->pages[i])
			return -ENOMEM;

		addr = (unsigned long)page_to_virt(obj->pages[i]);
		rc = set_memory_decrypted(addr, 1);
		if (rc != 0)
			return rc;
	}

	return 0;
}

static int tdx_shm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct tdx_shm_object *obj = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long npages = size >> PAGE_SHIFT;
	int rc;

	if (vma->vm_pgoff != 0)
		return -EINVAL;
	if (obj->pages != NULL)
		return -EBUSY;
	if (size == 0 || (size & (PAGE_SIZE - 1)) != 0)
		return -EINVAL;

	rc = tdx_shm_alloc_pages(obj, npages);
	if (rc != 0)
		return rc;

	obj->size = size;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);
	rc = vm_map_pages_zero(vma, obj->pages, obj->npages);
	if (rc != 0)
		return rc;

	vma->vm_ops = &tdx_shm_vm_ops;
	vma->vm_private_data = obj;
	tdx_shm_vma_open(vma);
	return 0;
}

static const struct file_operations tdx_shm_fops = {
	.owner = THIS_MODULE,
	.open = tdx_shm_open,
	.release = tdx_shm_release,
	.mmap = tdx_shm_mmap,
};

static struct miscdevice tdx_shm_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "tdx_shm",
	.fops = &tdx_shm_fops,
	.mode = 0600,
};

module_misc_device(tdx_shm_dev);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("TDX guest shared-memory helper for RDMA user mappings");
