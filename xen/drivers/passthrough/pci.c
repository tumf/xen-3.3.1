/*
 * Copyright (C) 2008,  Netronome Systems, Inc.
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
 */

#include <xen/sched.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include <xen/list.h>
#include <xen/prefetch.h>
#include <xen/iommu.h>
#include <xen/delay.h>
#include <xen/keyhandler.h>


LIST_HEAD(alldevs_list);
rwlock_t pcidevs_lock = RW_LOCK_UNLOCKED;

struct pci_dev *alloc_pdev(u8 bus, u8 devfn)
{
    struct pci_dev *pdev;

    list_for_each_entry ( pdev, &alldevs_list, alldevs_list )
        if ( pdev->bus == bus && pdev->devfn == devfn )
            return pdev;

    pdev = xmalloc(struct pci_dev);
    if ( !pdev )
        return NULL;

    *((u8*) &pdev->bus) = bus;
    *((u8*) &pdev->devfn) = devfn;
    pdev->domain = NULL;
    spin_lock_init(&pdev->lock);
    INIT_LIST_HEAD(&pdev->msi_list);
    list_add(&pdev->alldevs_list, &alldevs_list);

    return pdev;
}

void free_pdev(struct pci_dev *pdev)
{
    list_del(&pdev->alldevs_list);
    xfree(pdev);
}

struct pci_dev *pci_lock_pdev(int bus, int devfn)
{
    struct pci_dev *pdev;

    read_lock(&pcidevs_lock);
    list_for_each_entry ( pdev, &alldevs_list, alldevs_list )
        if ( (pdev->bus == bus || bus == -1) &&
             (pdev->devfn == devfn || devfn == -1) )
    {
        spin_lock(&pdev->lock);
        read_unlock(&pcidevs_lock);
        return pdev;
    }
    read_unlock(&pcidevs_lock);

    return NULL;
}

struct pci_dev *pci_lock_domain_pdev(struct domain *d, int bus, int devfn)
{
    struct pci_dev *pdev;

    read_lock(&pcidevs_lock);
    list_for_each_entry ( pdev, &d->arch.pdev_list, domain_list )
    {
        spin_lock(&pdev->lock);
        if ( (pdev->bus == bus || bus == -1) &&
             (pdev->devfn == devfn || devfn == -1) &&
             (pdev->domain == d) )
        {
            read_unlock(&pcidevs_lock);
            return pdev;
        }
        spin_unlock(&pdev->lock);
    }
    read_unlock(&pcidevs_lock);

    return NULL;
}

int pci_add_device(u8 bus, u8 devfn)
{
    struct pci_dev *pdev;
    int ret = -ENOMEM;

    write_lock(&pcidevs_lock);
    pdev = alloc_pdev(bus, devfn);
    if ( !pdev )
        goto out;

    ret = 0;
    spin_lock(&pdev->lock);
    if ( !pdev->domain )
    {
        pdev->domain = dom0;
        ret = iommu_add_device(pdev);
        if ( ret )
        {
            spin_unlock(&pdev->lock);
            goto out;
        }
        list_add(&pdev->domain_list, &dom0->arch.pdev_list);
    }
    spin_unlock(&pdev->lock);
    printk(XENLOG_DEBUG "PCI add device %02x:%02x.%x\n", bus,
           PCI_SLOT(devfn), PCI_FUNC(devfn));

out:
    write_unlock(&pcidevs_lock);
    return ret;
}

int pci_remove_device(u8 bus, u8 devfn)
{
    struct pci_dev *pdev;
    int ret = -ENODEV;;

    write_lock(&pcidevs_lock);
    list_for_each_entry ( pdev, &alldevs_list, alldevs_list )
        if ( pdev->bus == bus && pdev->devfn == devfn )
        {
            spin_lock(&pdev->lock);
            ret = iommu_remove_device(pdev);
            if ( pdev->domain )
                list_del(&pdev->domain_list);
            pci_cleanup_msi(pdev);
            free_pdev(pdev);
            printk(XENLOG_DEBUG "PCI remove device %02x:%02x.%x\n", bus,
                   PCI_SLOT(devfn), PCI_FUNC(devfn));
            break;
        }

    write_unlock(&pcidevs_lock);
    return ret;
}

static void pci_clean_dpci_irqs(struct domain *d)
{
    struct hvm_irq_dpci *hvm_irq_dpci = domain_get_irq_dpci(d);
    uint32_t i;
    struct list_head *digl_list, *tmp;
    struct dev_intx_gsi_link *digl;

    if ( !iommu_enabled )
        return;

    if ( !is_hvm_domain(d) && !need_iommu(d) )
        return;

    if ( hvm_irq_dpci != NULL )
    {
        for ( i = 0; i < NR_IRQS; i++ )
        {
            if ( !(hvm_irq_dpci->mirq[i].flags & HVM_IRQ_DPCI_VALID) )
                continue;

            pirq_guest_unbind(d, i);
            kill_timer(&hvm_irq_dpci->hvm_timer[irq_to_vector(i)]);

            list_for_each_safe ( digl_list, tmp,
                                 &hvm_irq_dpci->mirq[i].digl_list )
            {
                digl = list_entry(digl_list,
                                  struct dev_intx_gsi_link, list);
                list_del(&digl->list);
                xfree(digl);
            }
        }

        d->arch.hvm_domain.irq.dpci = NULL;
        xfree(hvm_irq_dpci);
    }
}

void pci_release_devices(struct domain *d)
{
    struct pci_dev *pdev;
    u8 bus, devfn;

    pci_clean_dpci_irqs(d);
    while ( (pdev = pci_lock_domain_pdev(d, -1, -1)) )
    {
        pci_cleanup_msi(pdev);
        bus = pdev->bus; devfn = pdev->devfn;
        spin_unlock(&pdev->lock);
        deassign_device(d, bus, devfn);
    }
}

static void dump_pci_devices(unsigned char ch)
{
    struct pci_dev *pdev;
    struct msi_desc *msi;

    printk("==== PCI devices ====\n");
    read_lock(&pcidevs_lock);

    list_for_each_entry ( pdev, &alldevs_list, alldevs_list )
    {
        spin_lock(&pdev->lock);
        printk("%02x:%02x.%x - dom %-3d - MSIs < ",
               pdev->bus, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn),
               pdev->domain ? pdev->domain->domain_id : -1);
        list_for_each_entry ( msi, &pdev->msi_list, list )
               printk("%d ", msi->vector);
        printk(">\n");
        spin_unlock(&pdev->lock);
    }

    read_unlock(&pcidevs_lock);
}

static int __init setup_dump_pcidevs(void)
{
    register_keyhandler('Q', dump_pci_devices, "dump PCI devices");
    return 0;
}
__initcall(setup_dump_pcidevs);



/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
