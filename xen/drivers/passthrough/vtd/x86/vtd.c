/*
 * Copyright (c) 2008, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Copyright (C) Allen Kay <allen.m.kay@intel.com>
 * Copyright (C) Weidong Han <weidong.han@intel.com>
 */

#include <xen/sched.h>
#include <xen/domain_page.h>
#include <asm/paging.h>
#include <xen/iommu.h>
#include "../iommu.h"
#include "../dmar.h"
#include "../vtd.h"

void *map_vtd_domain_page(u64 maddr)
{
    return map_domain_page(maddr >> PAGE_SHIFT_4K);
}

void unmap_vtd_domain_page(void *va)
{
    unmap_domain_page(va);
}

/* Allocate page table, return its machine address */
u64 alloc_pgtable_maddr(void)
{
    struct page_info *pg;
    u64 *vaddr;

    pg = alloc_domheap_page(NULL, 0);
    vaddr = map_domain_page(page_to_mfn(pg));
    if ( !vaddr )
        return 0;
    memset(vaddr, 0, PAGE_SIZE);

    iommu_flush_cache_page(vaddr);
    unmap_domain_page(vaddr);

    return page_to_maddr(pg);
}

void free_pgtable_maddr(u64 maddr)
{
    if ( maddr != 0 )
        free_domheap_page(maddr_to_page(maddr));
}

unsigned int get_clflush_size(void)
{
    return ((cpuid_ebx(1) >> 8) & 0xff) * 8;
}

struct hvm_irq_dpci *domain_get_irq_dpci(struct domain *domain)
{
    if ( !domain )
        return NULL;

    return domain->arch.hvm_domain.irq.dpci;
}

int domain_set_irq_dpci(struct domain *domain, struct hvm_irq_dpci *dpci)
{
    if ( !domain || !dpci )
        return 0;

    domain->arch.hvm_domain.irq.dpci = dpci;
    return 1;
}

void hvm_dpci_isairq_eoi(struct domain *d, unsigned int isairq)
{
    struct hvm_irq *hvm_irq = &d->arch.hvm_domain.irq;
    struct hvm_irq_dpci *dpci = domain_get_irq_dpci(d);
    struct dev_intx_gsi_link *digl, *tmp;
    int i;

    ASSERT(isairq < NR_ISAIRQS);
    if ( !vtd_enabled || !dpci ||
         !test_bit(isairq, dpci->isairq_map) )
        return;

    /* Multiple mirq may be mapped to one isa irq */
    for ( i = 0; i < NR_IRQS; i++ )
    {
        if ( !dpci->mirq[i].flags & HVM_IRQ_DPCI_VALID )
            continue;

        list_for_each_entry_safe ( digl, tmp,
            &dpci->mirq[i].digl_list, list )
        {
            if ( hvm_irq->pci_link.route[digl->link] == isairq )
            {
                hvm_pci_intx_deassert(d, digl->device, digl->intx);
                spin_lock(&dpci->dirq_lock);
                if ( --dpci->mirq[i].pending == 0 )
                {
                    spin_unlock(&dpci->dirq_lock);
                    stop_timer(&dpci->hvm_timer[domain_irq_to_vector(d, i)]);
                    pirq_guest_eoi(d, i);
                }
                else
                    spin_unlock(&dpci->dirq_lock);
            }
        }
    }
}
