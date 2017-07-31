/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/fit_ibapi.h>
#include <lego/comp_memory.h>

#include <memory/include/vm.h>
#include <memory/include/pid.h>

/*
 * Processor manager rely on the length of replied
 * message to know if us succeed or failed.
 */
static void llc_miss_error(u32 retval, u64 desc,
			   struct lego_task_struct *p, u64 vaddr)
{
	WARN(1, "src_nid:%u,pid:%u,vaddr:%#Lx\n", p->node, p->pid, vaddr);
	ibapi_reply_message(&retval, 4, desc);
}

static void bad_area(struct lego_task_struct *p, u64 vaddr, u64 offset, u64 desc)
{
	int retval = RET_ESIGSEGV;
	WARN(1, "src_nid:%u,pid:%u,vaddr:%#Lx\n", p->node, p->pid, vaddr);
	ibapi_reply_message(&retval, 4, desc);
}

static void do_handle_p2m_llc_miss(struct lego_task_struct *p,
				   u64 vaddr, u64 offset, u32 flags, u64 desc)
{
	struct vm_area_struct *vma;
	struct lego_mm_struct *mm = p->mm;
	unsigned long new_page;
	int ret;

	down_read(&mm->mmap_sem);

	vma = find_vma(mm, vaddr);
	if (unlikely(!vma)) {
		bad_area(p, vaddr, offset, desc);
		return;
	}

	/* VMAs except stack */
	if (likely(vma->vm_start <= vaddr))
		goto good_area;

	/* stack? */
	if (unlikely(!(vma->vm_flags & VM_GROWSDOWN))) {
		bad_area(p, vaddr, offset, desc);
		return;
	}

	if (unlikely(expand_stack(vma, vaddr))) {
		bad_area(p, vaddr, offset, desc);
		return;
	}

	/*
	 * Ok, we have a good vm_area for this memory access,
	 * go for it...
	 */
good_area:
	ret = handle_lego_mm_fault(vma, vaddr, flags, &new_page);
	if (unlikely(ret & VM_FAULT_ERROR)) {
		if (ret & VM_FAULT_OOM)
			ret = RET_ENOMEM;
		else if (ret & (VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV))
			ret = RET_ESIGSEGV;
		llc_miss_error(ret, desc, p, vaddr);
		return;
	}

	up_read(&mm->mmap_sem);

	/* Send the cacheline back to processor! */
	ibapi_reply_message((void *)(new_page + offset),
		PAGE_SIZE / CONFIG_PCACHE_FILL_SPLIT_NR, desc);
}

static int fault_in_kernel_space(unsigned long address)
{
	return address >= TASK_SIZE_MAX;
}

int handle_p2m_llc_miss(struct p2m_llc_miss_struct *payload, u64 desc,
			struct common_header *hdr)
{
	u32 pid, nid, flags;
	u64 vaddr, offset;
	struct lego_task_struct *p;

	nid    = hdr->src_nid;
	pid    = payload->pid;
	flags  = payload->flags;
	vaddr  = payload->missing_vaddr;
	offset = payload->offset; 

	pr_info("%s: nid: %u, pid: %u, missing_vaddr: %#Lx, offset: %#Lx, nr_split: %d\n",
		__func__, nid, pid, vaddr, offset, CONFIG_PCACHE_FILL_SPLIT_NR);

	p = find_lego_task_by_pid(hdr->src_nid, payload->pid);
	if (unlikely(!p)) {
		llc_miss_error(RET_ESRCH, desc, p, vaddr);
		return 0;
	}

	if (unlikely(fault_in_kernel_space(vaddr))) {
		llc_miss_error(RET_EFAULT, desc, p, vaddr);
		return 0;
	}

	do_handle_p2m_llc_miss(p, vaddr, offset, flags, desc);
	return 0;
}