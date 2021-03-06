/* 
 ****************************************************************************
 * (C) 2003 - Rolf Neugebauer - Intel Research Cambridge
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: mm.c
 *      Author: Rolf Neugebauer (neugebar@dcs.gla.ac.uk)
 *     Changes: Grzegorz Milos
 *              
 *        Date: Aug 2003, chages Aug 2005
 * 
 * Environment: Xen Minimal OS
 * Description: memory management related functions
 *              contains buddy page allocator from Xen.
 *
 ****************************************************************************
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

#include <os.h>
#include <hypervisor.h>
#include <mm.h>
#include <types.h>
#include <lib.h>
#include <xmalloc.h>
#include <xen/memory.h>

#ifdef MM_DEBUG
#define DEBUG(_f, _a...) \
    printk("MINI_OS(file=mm.c, line=%d) " _f "\n", __LINE__, ## _a)
#else
#define DEBUG(_f, _a...)    ((void)0)
#endif

unsigned long *phys_to_machine_mapping;
unsigned long mfn_zero;
extern char stack[];
extern void page_walk(unsigned long virt_addr);

void new_pt_frame(unsigned long *pt_pfn, unsigned long prev_l_mfn, 
                                unsigned long offset, unsigned long level)
{   
    pgentry_t *tab = (pgentry_t *)start_info.pt_base;
    unsigned long pt_page = (unsigned long)pfn_to_virt(*pt_pfn); 
    pgentry_t prot_e, prot_t;
    mmu_update_t mmu_updates[1];
    
    prot_e = prot_t = 0;
    DEBUG("Allocating new L%d pt frame for pt_pfn=%lx, "
           "prev_l_mfn=%lx, offset=%lx", 
           level, *pt_pfn, prev_l_mfn, offset);

    /* We need to clear the page, otherwise we might fail to map it
       as a page table page */
    memset((void*) pt_page, 0, PAGE_SIZE);  
 
    switch ( level )
    {
    case L1_FRAME:
         prot_e = L1_PROT;
         prot_t = L2_PROT;
         break;
    case L2_FRAME:
         prot_e = L2_PROT;
         prot_t = L3_PROT;
         break;
#if defined(__x86_64__)
    case L3_FRAME:
         prot_e = L3_PROT;
         prot_t = L4_PROT;
         break;
#endif
    default:
         printk("new_pt_frame() called with invalid level number %d\n", level);
         do_exit();
         break;
    }

    /* Update the entry */
#if defined(__x86_64__)
    tab = pte_to_virt(tab[l4_table_offset(pt_page)]);
#endif
    tab = pte_to_virt(tab[l3_table_offset(pt_page)]);

    mmu_updates[0].ptr = (tab[l2_table_offset(pt_page)] & PAGE_MASK) + 
                         sizeof(pgentry_t) * l1_table_offset(pt_page);
    mmu_updates[0].val = (pgentry_t)pfn_to_mfn(*pt_pfn) << PAGE_SHIFT | 
                         (prot_e & ~_PAGE_RW);
    if(HYPERVISOR_mmu_update(mmu_updates, 1, NULL, DOMID_SELF) < 0)
    {
         printk("PTE for new page table page could not be updated\n");
         do_exit();
    }
                        
    /* Now fill the new page table page with entries.
       Update the page directory as well. */
    mmu_updates[0].ptr = ((pgentry_t)prev_l_mfn << PAGE_SHIFT) + sizeof(pgentry_t) * offset;
    mmu_updates[0].val = (pgentry_t)pfn_to_mfn(*pt_pfn) << PAGE_SHIFT | prot_t;
    if(HYPERVISOR_mmu_update(mmu_updates, 1, NULL, DOMID_SELF) < 0) 
    {
       printk("ERROR: mmu_update failed\n");
       do_exit();
    }

    *pt_pfn += 1;
}

/* Checks if a pagetable frame is needed (if weren't allocated by Xen) */
static int need_pt_frame(unsigned long virt_address, int level)
{
    unsigned long hyp_virt_start = HYPERVISOR_VIRT_START;
#if defined(__x86_64__)
    unsigned long hyp_virt_end = HYPERVISOR_VIRT_END;
#else
    unsigned long hyp_virt_end = 0xffffffff;
#endif

    /* In general frames will _not_ be needed if they were already
       allocated to map the hypervisor into our VA space */
#if defined(__x86_64__)
    if(level == L3_FRAME)
    {
        if(l4_table_offset(virt_address) >= 
           l4_table_offset(hyp_virt_start) &&
           l4_table_offset(virt_address) <= 
           l4_table_offset(hyp_virt_end))
            return 0;
        return 1;
    } else
#endif

    if(level == L2_FRAME)
    {
#if defined(__x86_64__)
        if(l4_table_offset(virt_address) >= 
           l4_table_offset(hyp_virt_start) &&
           l4_table_offset(virt_address) <= 
           l4_table_offset(hyp_virt_end))
#endif
            if(l3_table_offset(virt_address) >= 
               l3_table_offset(hyp_virt_start) &&
               l3_table_offset(virt_address) <= 
               l3_table_offset(hyp_virt_end))
                return 0;

        return 1;
    } else 

    /* Always need l1 frames */
    if(level == L1_FRAME)
        return 1;

    printk("ERROR: Unknown frame level %d, hypervisor %llx,%llx\n", 
        level, hyp_virt_start, hyp_virt_end);
    return -1;
}

void build_pagetable(unsigned long *start_pfn, unsigned long *max_pfn)
{
    unsigned long start_address, end_address;
    unsigned long pfn_to_map, pt_pfn = *start_pfn;
    static mmu_update_t mmu_updates[L1_PAGETABLE_ENTRIES + 1];
    pgentry_t *tab = (pgentry_t *)start_info.pt_base, page;
    unsigned long mfn = pfn_to_mfn(virt_to_pfn(start_info.pt_base));
    unsigned long offset;
    int count = 0;

    pfn_to_map = (start_info.nr_pt_frames - NOT_L1_FRAMES) * L1_PAGETABLE_ENTRIES;

    if (*max_pfn >= virt_to_pfn(HYPERVISOR_VIRT_START))
    {
        printk("WARNING: Mini-OS trying to use Xen virtual space. "
               "Truncating memory from %dMB to ",
               ((unsigned long)pfn_to_virt(*max_pfn) - (unsigned long)&_text)>>20);
        *max_pfn = virt_to_pfn(HYPERVISOR_VIRT_START - PAGE_SIZE);
        printk("%dMB\n",
               ((unsigned long)pfn_to_virt(*max_pfn) - (unsigned long)&_text)>>20);
    }

    start_address = (unsigned long)pfn_to_virt(pfn_to_map);
    end_address = (unsigned long)pfn_to_virt(*max_pfn);

    /* We worked out the virtual memory range to map, now mapping loop */
    printk("Mapping memory range 0x%lx - 0x%lx\n", start_address, end_address);

    while(start_address < end_address)
    {
        tab = (pgentry_t *)start_info.pt_base;
        mfn = pfn_to_mfn(virt_to_pfn(start_info.pt_base));

#if defined(__x86_64__)
        offset = l4_table_offset(start_address);
        /* Need new L3 pt frame */
        if(!(start_address & L3_MASK)) 
            if(need_pt_frame(start_address, L3_FRAME)) 
                new_pt_frame(&pt_pfn, mfn, offset, L3_FRAME);

        page = tab[offset];
        mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(mfn) << PAGE_SHIFT);
#endif
        offset = l3_table_offset(start_address);
        /* Need new L2 pt frame */
        if(!(start_address & L2_MASK))
            if(need_pt_frame(start_address, L2_FRAME))
                new_pt_frame(&pt_pfn, mfn, offset, L2_FRAME);

        page = tab[offset];
        mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(mfn) << PAGE_SHIFT);
        offset = l2_table_offset(start_address);        
        /* Need new L1 pt frame */
        if(!(start_address & L1_MASK))
            if(need_pt_frame(start_address, L1_FRAME)) 
                new_pt_frame(&pt_pfn, mfn, offset, L1_FRAME);

        page = tab[offset];
        mfn = pte_to_mfn(page);
        offset = l1_table_offset(start_address);

        mmu_updates[count].ptr = ((pgentry_t)mfn << PAGE_SHIFT) + sizeof(pgentry_t) * offset;
        mmu_updates[count].val = (pgentry_t)pfn_to_mfn(pfn_to_map++) << PAGE_SHIFT | L1_PROT;
        count++;
        if (count == L1_PAGETABLE_ENTRIES || pfn_to_map == *max_pfn)
        {
            if(HYPERVISOR_mmu_update(mmu_updates, count, NULL, DOMID_SELF) < 0)
            {
                printk("PTE could not be updated\n");
                do_exit();
            }
            count = 0;
        }
        start_address += PAGE_SIZE;
    }

    *start_pfn = pt_pfn;
}

extern void shared_info;
static void set_readonly(void *text, void *etext)
{
    unsigned long start_address = ((unsigned long) text + PAGE_SIZE - 1) & PAGE_MASK;
    unsigned long end_address = (unsigned long) etext;
    static mmu_update_t mmu_updates[L1_PAGETABLE_ENTRIES + 1];
    pgentry_t *tab = (pgentry_t *)start_info.pt_base, page;
    unsigned long mfn = pfn_to_mfn(virt_to_pfn(start_info.pt_base));
    unsigned long offset;
    int count = 0;

    printk("setting %p-%p readonly\n", text, etext);

    while (start_address + PAGE_SIZE <= end_address) {
        tab = (pgentry_t *)start_info.pt_base;
        mfn = pfn_to_mfn(virt_to_pfn(start_info.pt_base));

#if defined(__x86_64__)
        offset = l4_table_offset(start_address);
        page = tab[offset];
        mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(mfn) << PAGE_SHIFT);
#endif
        offset = l3_table_offset(start_address);
        page = tab[offset];
        mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(mfn) << PAGE_SHIFT);
        offset = l2_table_offset(start_address);        
        page = tab[offset];
        mfn = pte_to_mfn(page);
        tab = to_virt(mfn_to_pfn(mfn) << PAGE_SHIFT);

        offset = l1_table_offset(start_address);

	if (start_address != (unsigned long)&shared_info) {
	    mmu_updates[count].ptr = ((pgentry_t)mfn << PAGE_SHIFT) + sizeof(pgentry_t) * offset;
	    mmu_updates[count].val = tab[offset] & ~_PAGE_RW;
	    count++;
	} else
	    printk("skipped %p\n", start_address);

        start_address += PAGE_SIZE;

        if (count == L1_PAGETABLE_ENTRIES || start_address + PAGE_SIZE > end_address)
        {
            if(HYPERVISOR_mmu_update(mmu_updates, count, NULL, DOMID_SELF) < 0)
            {
                printk("PTE could not be updated\n");
                do_exit();
            }
            count = 0;
        }
    }

    {
	mmuext_op_t op = {
	    .cmd = MMUEXT_TLB_FLUSH_ALL,
	};
	int count;
	HYPERVISOR_mmuext_op(&op, 1, &count, DOMID_SELF);
    }
}

void mem_test(unsigned long *start_add, unsigned long *end_add)
{
    unsigned long mask = 0x10000;
    unsigned long *pointer;

    for(pointer = start_add; pointer < end_add; pointer++)
    {
        if(!(((unsigned long)pointer) & 0xfffff))
        {
            printk("Writing to %lx\n", pointer);
            page_walk((unsigned long)pointer);
        }
        *pointer = (unsigned long)pointer & ~mask;
    }

    for(pointer = start_add; pointer < end_add; pointer++)
    {
        if(((unsigned long)pointer & ~mask) != *pointer)
            printk("Read error at 0x%lx. Read: 0x%lx, should read 0x%lx\n",
                (unsigned long)pointer, 
                *pointer, 
                ((unsigned long)pointer & ~mask));
    }

}

static pgentry_t *get_pgt(unsigned long addr)
{
    unsigned long mfn;
    pgentry_t *tab;
    unsigned offset;

    tab = (pgentry_t *)start_info.pt_base;
    mfn = virt_to_mfn(start_info.pt_base);

#if defined(__x86_64__)
    offset = l4_table_offset(addr);
    if (!(tab[offset] & _PAGE_PRESENT))
        return NULL;
    mfn = pte_to_mfn(tab[offset]);
    tab = mfn_to_virt(mfn);
#endif
    offset = l3_table_offset(addr);
    if (!(tab[offset] & _PAGE_PRESENT))
        return NULL;
    mfn = pte_to_mfn(tab[offset]);
    tab = mfn_to_virt(mfn);
    offset = l2_table_offset(addr);
    if (!(tab[offset] & _PAGE_PRESENT))
        return NULL;
    mfn = pte_to_mfn(tab[offset]);
    tab = mfn_to_virt(mfn);
    offset = l1_table_offset(addr);
    return &tab[offset];
}

pgentry_t *need_pgt(unsigned long addr)
{
    unsigned long mfn;
    pgentry_t *tab;
    unsigned long pt_pfn;
    unsigned offset;

    tab = (pgentry_t *)start_info.pt_base;
    mfn = virt_to_mfn(start_info.pt_base);

#if defined(__x86_64__)
    offset = l4_table_offset(addr);
    if (!(tab[offset] & _PAGE_PRESENT)) {
        pt_pfn = virt_to_pfn(alloc_page());
        new_pt_frame(&pt_pfn, mfn, offset, L3_FRAME);
    }
    ASSERT(tab[offset] & _PAGE_PRESENT);
    mfn = pte_to_mfn(tab[offset]);
    tab = mfn_to_virt(mfn);
#endif
    offset = l3_table_offset(addr);
    if (!(tab[offset] & _PAGE_PRESENT)) {
        pt_pfn = virt_to_pfn(alloc_page());
        new_pt_frame(&pt_pfn, mfn, offset, L2_FRAME);
    }
    ASSERT(tab[offset] & _PAGE_PRESENT);
    mfn = pte_to_mfn(tab[offset]);
    tab = mfn_to_virt(mfn);
    offset = l2_table_offset(addr);
    if (!(tab[offset] & _PAGE_PRESENT)) {
        pt_pfn = virt_to_pfn(alloc_page());
	new_pt_frame(&pt_pfn, mfn, offset, L1_FRAME);
    }
    ASSERT(tab[offset] & _PAGE_PRESENT);
    mfn = pte_to_mfn(tab[offset]);
    tab = mfn_to_virt(mfn);

    offset = l1_table_offset(addr);
    return &tab[offset];
}

static unsigned long demand_map_area_start;
#ifdef __x86_64__
#define DEMAND_MAP_PAGES ((128ULL << 30) / PAGE_SIZE)
#else
#define DEMAND_MAP_PAGES ((2ULL << 30) / PAGE_SIZE)
#endif

#ifndef HAVE_LIBC
#define HEAP_PAGES 0
#else
unsigned long heap, brk, heap_mapped, heap_end;
#ifdef __x86_64__
#define HEAP_PAGES ((128ULL << 30) / PAGE_SIZE)
#else
#define HEAP_PAGES ((1ULL << 30) / PAGE_SIZE)
#endif
#endif

void arch_init_demand_mapping_area(unsigned long cur_pfn)
{
    cur_pfn++;

    demand_map_area_start = (unsigned long) pfn_to_virt(cur_pfn);
    cur_pfn += DEMAND_MAP_PAGES;
    printk("Demand map pfns at %lx-%lx.\n", demand_map_area_start, pfn_to_virt(cur_pfn));

#ifdef HAVE_LIBC
    cur_pfn++;
    heap_mapped = brk = heap = (unsigned long) pfn_to_virt(cur_pfn);
    cur_pfn += HEAP_PAGES;
    heap_end = (unsigned long) pfn_to_virt(cur_pfn);
    printk("Heap resides at %lx-%lx.\n", brk, heap_end);
#endif
}

#define MAP_BATCH ((STACK_SIZE / 2) / sizeof(mmu_update_t))
void do_map_frames(unsigned long addr,
        unsigned long *f, unsigned long n, unsigned long stride,
	unsigned long increment, domid_t id, int may_fail, unsigned long prot)
{
    pgentry_t *pgt = NULL;
    unsigned long done = 0;
    unsigned long i;
    int rc;

    while (done < n) {
	unsigned long todo;

	if (may_fail)
	    todo = 1;
	else
	    todo = n - done;

	if (todo > MAP_BATCH)
		todo = MAP_BATCH;

	{
	    mmu_update_t mmu_updates[todo];

	    for (i = 0; i < todo; i++, addr += PAGE_SIZE, pgt++) {
                if (!pgt || !(addr & L1_MASK))
                    pgt = need_pgt(addr);
		mmu_updates[i].ptr = virt_to_mach(pgt);
		mmu_updates[i].val = ((pgentry_t)(f[(done + i) * stride] + (done + i) * increment) << PAGE_SHIFT) | prot;
	    }

	    rc = HYPERVISOR_mmu_update(mmu_updates, todo, NULL, id);
	    if (rc < 0) {
		if (may_fail)
		    f[done * stride] |= 0xF0000000;
		else {
		    printk("Map %ld (%lx, ...) at %p failed: %d.\n", todo, f[done * stride] + done * increment, addr, rc);
                    do_exit();
		}
	    }
	}

	done += todo;
    }
}

unsigned long allocate_ondemand(unsigned long n, unsigned long alignment)
{
    unsigned long x;
    unsigned long y = 0;

    /* Find a properly aligned run of n contiguous frames */
    for (x = 0; x <= DEMAND_MAP_PAGES - n; x = (x + y + 1 + alignment - 1) & ~(alignment - 1)) {
        unsigned long addr = demand_map_area_start + x * PAGE_SIZE;
        pgentry_t *pgt = get_pgt(addr);
        for (y = 0; y < n; y++, addr += PAGE_SIZE) {
            if (!(addr & L1_MASK))
                pgt = get_pgt(addr);
            if (pgt) {
                if (*pgt & _PAGE_PRESENT)
                    break;
                pgt++;
            }
        }
        if (y == n)
            break;
    }
    if (y != n) {
        printk("Failed to find %ld frames!\n", n);
        return 0;
    }
    return demand_map_area_start + x * PAGE_SIZE;
}

void *map_frames_ex(unsigned long *f, unsigned long n, unsigned long stride,
	unsigned long increment, unsigned long alignment, domid_t id,
	int may_fail, unsigned long prot)
{
    unsigned long addr = allocate_ondemand(n, alignment);

    if (!addr)
        return NULL;

    /* Found it at x.  Map it in. */
    do_map_frames(addr, f, n, stride, increment, id, may_fail, prot);

    return (void *)addr;
}

static void clear_bootstrap(void)
{
    pte_t nullpte = { };

    /* Use first page as the CoW zero page */
    memset(&_text, 0, PAGE_SIZE);
    mfn_zero = virt_to_mfn((unsigned long) &_text);
    if (HYPERVISOR_update_va_mapping(0, nullpte, UVMF_INVLPG))
	printk("Unable to unmap NULL page\n");
}

void arch_init_p2m(unsigned long max_pfn)
{
#define L1_P2M_SHIFT    9
#define L2_P2M_SHIFT    18    
#define L3_P2M_SHIFT    27    
#define L1_P2M_ENTRIES  (1 << L1_P2M_SHIFT)    
#define L2_P2M_ENTRIES  (1 << (L2_P2M_SHIFT - L1_P2M_SHIFT))    
#define L3_P2M_ENTRIES  (1 << (L3_P2M_SHIFT - L2_P2M_SHIFT))    
#define L1_P2M_MASK     (L1_P2M_ENTRIES - 1)    
#define L2_P2M_MASK     (L2_P2M_ENTRIES - 1)    
#define L3_P2M_MASK     (L3_P2M_ENTRIES - 1)    
    
    unsigned long *l1_list = NULL, *l2_list = NULL, *l3_list;
    unsigned long pfn;
    
    l3_list = (unsigned long *)alloc_page(); 
    for(pfn=0; pfn<max_pfn; pfn++)
    {
        if(!(pfn % (L1_P2M_ENTRIES * L2_P2M_ENTRIES)))
        {
            l2_list = (unsigned long*)alloc_page();
            if((pfn >> L3_P2M_SHIFT) > 0)
            {
                printk("Error: Too many pfns.\n");
                do_exit();
            }
            l3_list[(pfn >> L2_P2M_SHIFT)] = virt_to_mfn(l2_list);  
        }
        if(!(pfn % (L1_P2M_ENTRIES)))
        {
            l1_list = (unsigned long*)alloc_page();
            l2_list[(pfn >> L1_P2M_SHIFT) & L2_P2M_MASK] = 
                virt_to_mfn(l1_list); 
        }

        l1_list[pfn & L1_P2M_MASK] = pfn_to_mfn(pfn); 
    }
    HYPERVISOR_shared_info->arch.pfn_to_mfn_frame_list_list = 
        virt_to_mfn(l3_list);
    HYPERVISOR_shared_info->arch.max_pfn = max_pfn;
}

void arch_init_mm(unsigned long* start_pfn_p, unsigned long* max_pfn_p)
{

    unsigned long start_pfn, max_pfn, virt_pfns;

    printk("  _text:        %p\n", &_text);
    printk("  _etext:       %p\n", &_etext);
    printk("  _erodata:     %p\n", &_erodata);
    printk("  _edata:       %p\n", &_edata);
    printk("  stack start:  %p\n", stack);
    printk("  _end:         %p\n", &_end);

    /* First page follows page table pages and 3 more pages (store page etc) */
    start_pfn = PFN_UP(to_phys(start_info.pt_base)) + 
                start_info.nr_pt_frames + 3;
    max_pfn = start_info.nr_pages;

    /* We need room for demand mapping and heap, clip available memory */
    virt_pfns = DEMAND_MAP_PAGES + HEAP_PAGES;
    if (max_pfn + virt_pfns + 1 < max_pfn)
        max_pfn = -(virt_pfns + 1);

    printk("  start_pfn:    %lx\n", start_pfn);
    printk("  max_pfn:      %lx\n", max_pfn);

    build_pagetable(&start_pfn, &max_pfn);
    clear_bootstrap();
    set_readonly(&_text, &_erodata);

    *start_pfn_p = start_pfn;
    *max_pfn_p = max_pfn;
}
