/*
 * Copyright (c) 2016-2018 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/bug.h>
#include <lego/mm.h>
#include <lego/spinlock.h>
#include <lego/comp_memory.h>

#include <lego/comp_storage.h>

#include <memory/vm.h>
#include <memory/file_ops.h>
#include <memory/vm-pgtable.h>

static int do_swap_page(struct vm_area_struct *vma, unsigned long address,
			unsigned int flags, pte_t *ptep, pmd_t *pmd,
			pte_t entry)
{
	BUG();
	return 0;
}

static int do_wp_page(struct vm_area_struct *vma, unsigned long address,
		      unsigned int flags, pte_t *ptep, pmd_t *pmd, pte_t entry)
{
	BUG();
	return 0;
}

/* TODO:
 * This is a much simplified __do_fault
 * we need to consider alot other protection things.
 */
static int __do_fault(struct lego_mm_struct *mm, struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmd,
		pgoff_t pgoff, unsigned int flags, pte_t orig_pte,
		unsigned long *mapping_flags)
{
	struct vm_fault vmf;
	pte_t *page_table;
	pte_t entry;
	spinlock_t *ptl;
	int ret;

	vmf.virtual_address = address & PAGE_MASK;
	vmf.pgoff = pgoff;
	vmf.flags = flags;
	vmf.page = 0;

	ret = vma->vm_ops->fault(vma, &vmf);
	if (unlikely(ret & VM_FAULT_ERROR))
		return ret;

	page_table = lego_pte_offset_lock(mm, pmd, address, &ptl);

	/* Only go through if we didn't race with anybody else... */
	if (likely(pte_same(*page_table, orig_pte))) {
		entry = lego_vfn_pte(((signed long)vmf.page >> PAGE_SHIFT),
					vma->vm_page_prot);
		if (flags & FAULT_FLAG_WRITE)
			entry = pte_mkwrite(pte_mkdirty(entry));
		pte_set(page_table, entry);
	}

	lego_pte_unlock(page_table, ptl);
	if (mapping_flags)
		*mapping_flags = PCACHE_MAPPING_FILE;
	return 0;
}

static int do_linear_fault(struct vm_area_struct *vma, unsigned long address,
			   unsigned int flags, pte_t *page_table, pmd_t *pmd,
			   pte_t orig_pte, unsigned long *mapping_flags)
{
	pgoff_t pgoff = (((address & PAGE_MASK)
			- vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;

	return __do_fault(vma->vm_mm, vma, address, pmd, pgoff, flags, orig_pte, mapping_flags);
}

static int do_anonymous_page(struct vm_area_struct *vma, unsigned long address,
			     unsigned int flags, pte_t *page_table, pmd_t *pmd,
			     unsigned long *mapping_flags)
{
	pte_t entry;
	spinlock_t *ptl;
	unsigned long vaddr;
	struct lego_mm_struct *mm = vma->vm_mm;

	vaddr = __get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!vaddr)
		return VM_FAULT_OOM;

	/*
	 * Use (signed long) to do the logical shift
	 * so that sign bit (left-most bit) will be extended:
	 *
	 * Since this page table is never loaded into CR3,
	 * hence it is okay to write those high reserved bits
	 * in page table entries.
	 */
	entry = lego_vfn_pte(((signed long)vaddr >> PAGE_SHIFT),
				vma->vm_page_prot);
	if (vma->vm_flags & VM_WRITE)
		entry = pte_mkwrite(pte_mkdirty(entry));

	page_table = lego_pte_offset_lock(mm, pmd, address, &ptl);
	if (!pte_none(*page_table))
		goto unlock;

	pte_set(page_table, entry);
unlock:
	lego_pte_unlock(page_table, ptl);
	if (mapping_flags)
		*mapping_flags = PCACHE_MAPPING_ANON;
	return 0;
}

static int handle_pte_fault(struct vm_area_struct *vma, unsigned long address,
			    unsigned int flags, pte_t *pte, pmd_t *pmd,
			    unsigned long *mapping_flags)
{
	pte_t entry;
	spinlock_t *ptl;
	struct lego_mm_struct *mm = vma->vm_mm;

	entry = *pte;
	if (!pte_present(entry)) {
		if (pte_none(entry)) {
			if (vma->vm_ops && vma->vm_ops->fault)
				return do_linear_fault(vma, address, flags,
						       pte, pmd, entry, mapping_flags);
			else
				return do_anonymous_page(vma, address, flags,
							 pte, pmd, mapping_flags);
		}
		return do_swap_page(vma, address, flags, pte, pmd, entry);
	}

	ptl = lego_pte_lockptr(mm, pmd);
	spin_lock(ptl);
	if (unlikely(!pte_same(*pte, entry)))
		goto unlock;

	/*
	 * If someone use faultin_page against an already valid/mapped user
	 * virtual address, then we will walk here. People should use
	 * get_user_pages() instead of faultin_page() maybe?
	 *
	 * Or if the vma is already populated, then all uva are mapped.
	 * in which case, all pcache misses will walk here. Shall we do this?
	 */
	if (flags & FAULT_FLAG_WRITE) {
		if (!pte_write(entry))
			return do_wp_page(vma, address, flags, pte, pmd, entry);

		/* Update pte */
		entry = pte_mkdirty(entry);
		if (!pte_same(*pte, entry))
			*pte = entry;
	}

unlock:
	lego_pte_unlock(pte, ptl);
	return 0;
}

/*
 * Given a missing address, this function will establish the process's
 * virtual memory page table mapping.
 *
 * RETURN: VM_FAULT_XXX flags
 * It is the caller's responsibility to check return value;
 *
 *
 * Note that:
 * Traditional page table:
 *	[process virtual address --> machine physical address]
 * Lego page table:
 *	[process virtual address --> kernel virtual address]
 *
 * Why we are using kernel virtual address here?
 * 1) Lego page table is not used by hardware walker.
 * 2) Kernel virtual address is sufficient for us to find the exact page
 *    in memory component. If we are using machine physical address, we
 *    will need another conversion from pa->va, after this function returns.
 *    (Since memory manager is running in kernel mode only, we have to use
 *     kernel virtual address to reference memory.)
 *
 * TODO:
 *	We are reusing the pud_offset etc. macros, which are using physical
 * 	address to fill page table. This is useless extra cost for us.
 *	Hence, we need special lego_pud_offset etc. macros to use kernel
 *	virtual address only.
 */
int handle_lego_mm_fault(struct vm_area_struct *vma, unsigned long address,
			 unsigned int flags, unsigned long *ret_va, unsigned long *mapping_flags)
{
	struct lego_mm_struct *mm = vma->vm_mm;
	int ret;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = lego_pgd_offset(mm, address);
	pud = lego_pud_alloc(mm, pgd, address);
	if (!pud)
		return VM_FAULT_OOM;
	pmd = lego_pmd_alloc(mm, pud, address);
	if (!pmd)
		return VM_FAULT_OOM;
	pte = lego_pte_alloc(mm, pmd, address);
	if (!pte)
		return VM_FAULT_OOM;

	ret = handle_pte_fault(vma, address, flags, pte, pmd, mapping_flags);
	if (unlikely(ret))
		return ret;

	/*
	 * Return the kernel virtual address of the new
	 * allocated page:
	 */
	*ret_va = pte_val(*pte) & PTE_VFN_MASK;

	return 0;
}

/*
 * These functions are used for handle mmap faults with multiple page faults
 */
static int __do_prefetch_fault(struct lego_mm_struct *mm, struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmd,
		pgoff_t pgoff, unsigned int flags, pte_t orig_pte, unsigned long page)
{
	struct vm_fault vmf;
	pte_t *page_table;
	pte_t entry;
	spinlock_t *ptl;
	int ret;

	vmf.virtual_address = address & PAGE_MASK;
	vmf.pgoff = pgoff;
	vmf.flags = flags;
	vmf.page = page;

	ret = 0; 
	if (unlikely(ret & VM_FAULT_ERROR))
		return ret;

	page_table = lego_pte_offset_lock(mm, pmd, address, &ptl);

	/* Only go through if we didn't race with anybody else... */
	if (likely(pte_same(*page_table, orig_pte))) {
		entry = lego_vfn_pte(((signed long)vmf.page >> PAGE_SHIFT),
					vma->vm_page_prot);
		if (vma->vm_flags & VM_WRITE)
			entry = pte_mkwrite(pte_mkdirty(entry));
		pte_set(page_table, entry);
	}

	lego_pte_unlock(page_table, ptl);

	return 0;
}

static int do_linear_prefetch_fault(struct vm_area_struct *vma, unsigned long address,
			   unsigned int flags, pte_t *page_table, pmd_t *pmd,
			   pte_t orig_pte, unsigned long page)
{
	pgoff_t pgoff = (((address & PAGE_MASK)
			- vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;

	return __do_prefetch_fault(vma->vm_mm, vma, address, pmd, pgoff, flags, orig_pte, page);
}


/* should not be a none pte */
static int handle_prefetch_pte_fault(struct vm_area_struct *vma, unsigned long address,
			unsigned int flags, pte_t *pte, pmd_t *pmd, unsigned long page)
{
	pte_t entry;
	entry = *pte;

	return do_linear_prefetch_fault(vma, address, flags, pte, pmd, entry, page);	
}

int handle_lego_mmap_faults(struct vm_area_struct *vma, unsigned long address,
		unsigned int flags, u32 nr_pages)
{
	struct lego_mm_struct *mm = vma->vm_mm;
	struct lego_task_struct *tsk = vma->vm_mm->task;
	struct lego_file *file;
	size_t count;
	loff_t pos;
	pgoff_t pgoff;

	int i;
	pgd_t *pgd; 
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long pages = 0;
	unsigned long cur_addr = round_down(address, PAGE_SIZE);
	unsigned long cur_page_addr;

	pages = __get_free_pages(GFP_KERNEL, PREFETCH_ORDER);
	if (unlikely(!pages))
		return VM_FAULT_OOM;

	pgoff = (((address & PAGE_MASK)
			- vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;
	file = vma->vm_file;
	count = nr_pages * PAGE_SIZE;
	pos = pgoff << PAGE_SHIFT;

	storage_read(tsk, file, (char *) pages, count, &pos);

	cur_page_addr = pages;

	for (i = 0; i < nr_pages; i++) {
		pgd = lego_pgd_offset(mm, cur_addr);
		pud = lego_pud_alloc(mm, pgd, cur_addr);
		if (!pud)
			return VM_FAULT_OOM;
		pmd = lego_pmd_alloc(mm, pud, cur_addr);
		if (!pmd)
			return VM_FAULT_OOM;
		pte = lego_pte_alloc(mm, pmd, cur_addr);
		if (!pte)
			return VM_FAULT_OOM;

		if (!pte_none(*pte)){
			/* TODO: how to free one page in pages
			 */
			goto next_round;	
		}

		handle_prefetch_pte_fault(vma, cur_addr, flags, pte, pmd, cur_page_addr);
next_round:
		cur_addr += PAGE_SIZE;
		cur_page_addr += PAGE_SIZE;
	}

	//pr_info("%s: handles [%lu] faults\n", __func__, faults);
	return 0;
}

int count_empty_entries(struct vm_area_struct *vma, unsigned long address,
		u32 nr_pages)
{
	int i, ret = 0;
	struct lego_mm_struct *mm = vma->vm_mm;
	unsigned long cur_addr = address;

	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	for (i = 0; i < nr_pages; i++) {
		pgd = lego_pgd_offset(mm, cur_addr);
		pud = lego_pud_alloc(mm, pgd, cur_addr);
		if (!pud)
			return -ENOMEM;
		pmd = lego_pmd_alloc(mm, pud, cur_addr);
		if (!pmd)
			return -ENOMEM;
		pte = lego_pte_alloc(mm, pmd, cur_addr);
		if (!pte)
			return -ENOMEM;
		if (pte_none(*pte))
			ret++;

		cur_addr += PAGE_SIZE;
	}

	return ret;
}
