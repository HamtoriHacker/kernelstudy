/*
 *  linux/arch/arm/mm/flush.c
 *
 *  Copyright (C) 1995-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>

#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/highmem.h>
#include <asm/smp_plat.h>
#include <asm/tlbflush.h>
#include <linux/hugetlb.h>

#include "mm.h"

#ifdef CONFIG_ARM_HEAVY_MB
void (*soc_mb)(void);

void arm_heavy_mb(void)
{
#ifdef CONFIG_OUTER_CACHE_SYNC
	if (outer_cache.sync)
		outer_cache.sync();
#endif
	if (soc_mb)
		soc_mb();
}
EXPORT_SYMBOL(arm_heavy_mb);
#endif

#ifdef CONFIG_CPU_CACHE_VIPT

static void flush_pfn_alias(unsigned long pfn, unsigned long vaddr)
{
	unsigned long to = FLUSH_ALIAS_START + (CACHE_COLOUR(vaddr) << PAGE_SHIFT);
	const int zero = 0;

	set_top_pte(to, pfn_pte(pfn, PAGE_KERNEL));

	asm(	"mcrr	p15, 0, %1, %0, c14\n"
	"	mcr	p15, 0, %2, c7, c10, 4"
	    :
	    : "r" (to), "r" (to + PAGE_SIZE - 1), "r" (zero)
	    : "cc");
	//%0~%1까지 주소에 Clean and flush the line 명령 수행
}

static void flush_icache_alias(unsigned long pfn, unsigned long vaddr, unsigned long len)
{
	unsigned long va = FLUSH_ALIAS_START + (CACHE_COLOUR(vaddr) << PAGE_SHIFT);
	unsigned long offset = vaddr & (PAGE_SIZE - 1);
	unsigned long to;

	set_top_pte(va, pfn_pte(pfn, PAGE_KERNEL));
	to = va + offset;
	flush_icache_range(to, to + len);
}

void flush_cache_mm(struct mm_struct *mm)
{
	if (cache_is_vivt()) {
		vivt_flush_cache_mm(mm);
		return;
	}

	if (cache_is_vipt_aliasing()) {
		asm(	"mcr	p15, 0, %0, c7, c14, 0\n"
		"	mcr	p15, 0, %0, c7, c10, 4"
		    :
		    : "r" (0)
		    : "cc");
	}
}

void flush_cache_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	if (cache_is_vivt()) {
		vivt_flush_cache_range(vma, start, end);
		return;
	}

	if (cache_is_vipt_aliasing()) {
		asm(	"mcr	p15, 0, %0, c7, c14, 0\n"
		"	mcr	p15, 0, %0, c7, c10, 4"
		    :
		    : "r" (0)
		    : "cc");
	}

	if (vma->vm_flags & VM_EXEC)
		__flush_icache_all();
}

void flush_cache_page(struct vm_area_struct *vma, unsigned long user_addr, unsigned long pfn)
{
	if (cache_is_vivt()) {
		vivt_flush_cache_page(vma, user_addr, pfn);
		return;
	}

	if (cache_is_vipt_aliasing()) {
		flush_pfn_alias(pfn, user_addr);
		__flush_icache_all();
	}

	if (vma->vm_flags & VM_EXEC && icache_is_vivt_asid_tagged())
		__flush_icache_all();
}

#else
#define flush_pfn_alias(pfn,vaddr)		do { } while (0)
#define flush_icache_alias(pfn,vaddr,len)	do { } while (0)
#endif

#define FLAG_PA_IS_EXEC 1
#define FLAG_PA_CORE_IN_MM 2

static void flush_ptrace_access_other(void *args)
{
	__flush_icache_all();
}

static inline
void __flush_ptrace_access(struct page *page, unsigned long uaddr, void *kaddr,
			   unsigned long len, unsigned int flags)
{
	if (cache_is_vivt()) {
		if (flags & FLAG_PA_CORE_IN_MM) {
			unsigned long addr = (unsigned long)kaddr;
			__cpuc_coherent_kern_range(addr, addr + len);
		}
		return;
	}

	if (cache_is_vipt_aliasing()) {
		flush_pfn_alias(page_to_pfn(page), uaddr);
		__flush_icache_all();
		return;
	}

	/* VIPT non-aliasing D-cache */
	if (flags & FLAG_PA_IS_EXEC) {
		unsigned long addr = (unsigned long)kaddr;
		if (icache_is_vipt_aliasing())
			flush_icache_alias(page_to_pfn(page), uaddr, len);
		else
			__cpuc_coherent_kern_range(addr, addr + len);
		if (cache_ops_need_broadcast())
			smp_call_function(flush_ptrace_access_other,
					  NULL, 1);
	}
}

static
void flush_ptrace_access(struct vm_area_struct *vma, struct page *page,
			 unsigned long uaddr, void *kaddr, unsigned long len)
{
	unsigned int flags = 0;
	if (cpumask_test_cpu(smp_processor_id(), mm_cpumask(vma->vm_mm)))
		flags |= FLAG_PA_CORE_IN_MM;
	if (vma->vm_flags & VM_EXEC)
		flags |= FLAG_PA_IS_EXEC;
	__flush_ptrace_access(page, uaddr, kaddr, len, flags);
}

void flush_uprobe_xol_access(struct page *page, unsigned long uaddr,
			     void *kaddr, unsigned long len)
{
	unsigned int flags = FLAG_PA_CORE_IN_MM|FLAG_PA_IS_EXEC;

	__flush_ptrace_access(page, uaddr, kaddr, len, flags);
}

/*
 * Copy user data from/to a page which is mapped into a different
 * processes address space.  Really, we want to allow our "user
 * space" model to handle this.
 *
 * Note that this code needs to run on the current CPU.
 */
void copy_to_user_page(struct vm_area_struct *vma, struct page *page,
		       unsigned long uaddr, void *dst, const void *src,
		       unsigned long len)
{
#ifdef CONFIG_SMP
	preempt_disable();
#endif
	memcpy(dst, src, len);
	flush_ptrace_access(vma, page, uaddr, dst, len);
#ifdef CONFIG_SMP
	preempt_enable();
#endif
}

void __flush_dcache_page(struct address_space *mapping, struct page *page)
{
	//page 구조체가 가리키는 메모리 영역에 대한 d-cache flush
	//d-cache : data cache
	/*
	 * Writeback any data associated with the kernel mapping of this
	 * page.  This ensures that data in the physical page is mutually
	 * coherent with the kernels mapping.
	 ->페이지의 커널매핑과 연관된 어떤 데이터를 writeback. physical 페이지의 데이터가 커널매핑과 상호적으로 연결되어있음을 보장한다.
	 */
	if (!PageHighMem(page)) {//highmem이 아닐때, physical 매핑이 불가
		size_t page_size = PAGE_SIZE << compound_order(page);
		//페이지 사이즈(PAGE_SIZE)에서 2의 compound_order(페이지 정보에서 가져옴)
		//승을 곱한다
		__cpuc_flush_dcache_area(page_address(page), page_size);
		//하드웨어 종속적인 콜백 함수를 수행.
		//Ensure that the data held in page is written back.
		//->페이지가 가지고 있는 데이터를 writeback하는 함수.
		//page_address : 페이지의 가상주소를 얻어옴.
		
	} else {//highmem일 경우
		unsigned long i;
		if (cache_is_vipt_nonaliasing()) {
	//캐시 타입이 vipt nonaliasing인 경우
	//pipt : aliasing에 대한 이슈가 없는 방식이지만 TLB hit 발생시 속도 저하.
	//pivt : 이론적으로 존재할 뿐 실제로 사용하지 않는 방식.
	//vivt : 서로 다른 가상 주소가 물리 주소를 가리키는 aliasing 문제가 발생. 
	/*vipt nonaliasing : virtually index, physically tagged cache, latency가 줄어듬.
			     가상주소가 겹쳐도 물리메모리가 태그됨.
			     aliasing은 두개의 가상주소가 하나의 물리메모리를 가리킴.
	 */		     
			for (i = 0; i < (1 << compound_order(page)); i++) {
				void *addr = kmap_atomic(page + i);
				//fixmap 영역에 highmem 메모리 한 페이지를 매핑.
				//현재 cpu에 해당하는 fixmap 영역을 사용하여 매핑.
				//이미 kmap 영역에 매핑된 경우 해당 가상주소를 리턴.
				//fixmap : 극히 짧은 시간 매핑이 필요할떄 사용.
				__cpuc_flush_dcache_area(addr, PAGE_SIZE);
				kunmap_atomic(addr);
				//kmap_atomic을 사용하여 fixmap 영역에 매핑된 highmem 해제
				//사실상 모든 주소구간을 모두 해제.
			}
		} else {//vipt nonaliasing이 아닌경우,이미 kmap 매핑된 가상 주소를 찾아 flush 후 해제
			for (i = 0; i < (1 << compound_order(page)); i++) {
				void *addr = kmap_high_get(page + i);
				//이미 kmap 매핑된 가상주소를 찾는다.
				//매핑이 되지 않았는데도 사용중인 구간에 대해(aliasing이				  된 구간에 대한 flush를 방지.
				if (addr) {
					__cpuc_flush_dcache_area(addr, PAGE_SIZE);
					kunmap_high(page + i);
					//kmap 영역에 매핑된 highmem 페이지 주소 매핑.
				}
			}
		}
	}

	/*
	 * If this is a page cache page, and we have an aliasing VIPT cache,
	 * we only need to do one flush - which would be at the relevant
	 * userspace colour, which is congruent with page->index.
	 페이지 캐시이고 vipt aliasing이면, 유저스페이스 colour와 연관될 하나의 flush를 실행,  
	 */
	if (mapping && cache_is_vipt_aliasing())
		//인수로 요청한 mapping 있는경우(주석 입력 시점에선 mapping NULL)이면서 vipt aliasing인 경우(TLB flush와 연관된 부분)
		flush_pfn_alias(page_to_pfn(page),
				page->index << PAGE_SHIFT);
}

static void __flush_dcache_aliases(struct address_space *mapping, struct page *page)
{
	struct mm_struct *mm = current->active_mm;
	struct vm_area_struct *mpnt;
	pgoff_t pgoff;

	/*
	 * There are possible user space mappings of this page:
	 * - VIVT cache: we need to also write back and invalidate all user
	 *   data in the current VM view associated with this page.
	 * - aliasing VIPT: we only need to find one mapping of this page.
	 */
	pgoff = page->index;

	flush_dcache_mmap_lock(mapping);
	vma_interval_tree_foreach(mpnt, &mapping->i_mmap, pgoff, pgoff) {
		unsigned long offset;

		/*
		 * If this VMA is not in our MM, we can ignore it.
		 */
		if (mpnt->vm_mm != mm)
			continue;
		if (!(mpnt->vm_flags & VM_MAYSHARE))
			continue;
		offset = (pgoff - mpnt->vm_pgoff) << PAGE_SHIFT;
		flush_cache_page(mpnt, mpnt->vm_start + offset, page_to_pfn(page));
	}
	flush_dcache_mmap_unlock(mapping);
}

#if __LINUX_ARM_ARCH__ >= 6
void __sync_icache_dcache(pte_t pteval)
{
	unsigned long pfn;
	struct page *page;
	struct address_space *mapping;

	if (cache_is_vipt_nonaliasing() && !pte_exec(pteval))
		/* only flush non-aliasing VIPT caches for exec mappings */
		return;
	pfn = pte_pfn(pteval);
	if (!pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);
	if (cache_is_vipt_aliasing())
		mapping = page_mapping(page);
	else
		mapping = NULL;

	if (!test_and_set_bit(PG_dcache_clean, &page->flags))
		__flush_dcache_page(mapping, page);

	if (pte_exec(pteval))
		__flush_icache_all();
}
#endif

/*
 * Ensure cache coherency between kernel mapping and userspace mapping
 * of this page.
 *
 * We have three cases to consider:
 *  - VIPT non-aliasing cache: fully coherent so nothing required.
 *  - VIVT: fully aliasing, so we need to handle every alias in our
 *          current VM view.
 *  - VIPT aliasing: need to handle one alias in our current VM view.
 *
 * If we need to handle aliasing:
 *  If the page only exists in the page cache and there are no user
 *  space mappings, we can be lazy and remember that we may have dirty
 *  kernel cache lines for later.  Otherwise, we assume we have
 *  aliasing mappings.
 *
 * Note that we disable the lazy flush for SMP configurations where
 * the cache maintenance operations are not automatically broadcasted.
 */
void flush_dcache_page(struct page *page)
{
	struct address_space *mapping;

	/*
	 * The zero page is never written to, so never has any dirty
	 * cache lines, and therefore never needs to be flushed.
	 */
	if (page == ZERO_PAGE(0))
		return;

	mapping = page_mapping(page);

	if (!cache_ops_need_broadcast() &&
	    mapping && !page_mapcount(page))
		clear_bit(PG_dcache_clean, &page->flags);
	else {
		__flush_dcache_page(mapping, page);
		if (mapping && cache_is_vivt())
			__flush_dcache_aliases(mapping, page);
		else if (mapping)
			__flush_icache_all();
		set_bit(PG_dcache_clean, &page->flags);
	}
}
EXPORT_SYMBOL(flush_dcache_page);

/*
 * Ensure cache coherency for the kernel mapping of this page. We can
 * assume that the page is pinned via kmap.
 *
 * If the page only exists in the page cache and there are no user
 * space mappings, this is a no-op since the page was already marked
 * dirty at creation.  Otherwise, we need to flush the dirty kernel
 * cache lines directly.
 */
void flush_kernel_dcache_page(struct page *page)
{
	if (cache_is_vivt() || cache_is_vipt_aliasing()) {
		struct address_space *mapping;

		mapping = page_mapping(page);

		if (!mapping || mapping_mapped(mapping)) {
			void *addr;

			addr = page_address(page);
			/*
			 * kmap_atomic() doesn't set the page virtual
			 * address for highmem pages, and
			 * kunmap_atomic() takes care of cache
			 * flushing already.
			 */
			if (!IS_ENABLED(CONFIG_HIGHMEM) || addr)
				__cpuc_flush_dcache_area(addr, PAGE_SIZE);
		}
	}
}
EXPORT_SYMBOL(flush_kernel_dcache_page);

/*
 * Flush an anonymous page so that users of get_user_pages()
 * can safely access the data.  The expected sequence is:
 *
 *  get_user_pages()
 *    -> flush_anon_page
 *  memcpy() to/from page
 *  if written to page, flush_dcache_page()
 */
void __flush_anon_page(struct vm_area_struct *vma, struct page *page, unsigned long vmaddr)
{
	unsigned long pfn;

	/* VIPT non-aliasing caches need do nothing */
	if (cache_is_vipt_nonaliasing())
		return;

	/*
	 * Write back and invalidate userspace mapping.
	 */
	pfn = page_to_pfn(page);
	if (cache_is_vivt()) {
		flush_cache_page(vma, vmaddr, pfn);
	} else {
		/*
		 * For aliasing VIPT, we can flush an alias of the
		 * userspace address only.
		 */
		flush_pfn_alias(pfn, vmaddr);
		__flush_icache_all();
	}

	/*
	 * Invalidate kernel mapping.  No data should be contained
	 * in this mapping of the page.  FIXME: this is overkill
	 * since we actually ask for a write-back and invalidate.
	 */
	__cpuc_flush_dcache_area(page_address(page), PAGE_SIZE);
}
