/*
 * hvm/pmtimer.c: emulation of the ACPI PM timer 
 *
 * Copyright (c) 2007, XenSource inc.
 * Copyright (c) 2006, Intel Corporation.
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

#include <asm/hvm/vpt.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>

/* Slightly more readable port I/O addresses for the registers we intercept */
#define PM1a_STS_ADDR (ACPI_PM1A_EVT_BLK_ADDRESS)
#define PM1a_EN_ADDR  (ACPI_PM1A_EVT_BLK_ADDRESS + 2)
#define TMR_VAL_ADDR  (ACPI_PM_TMR_BLK_ADDRESS)

/* The interesting bits of the PM1a_STS register */
#define TMR_STS    (1 << 0)
#define PWRBTN_STS (1 << 5)
#define GBL_STS    (1 << 8)

/* The same in PM1a_EN */
#define TMR_EN     (1 << 0)
#define PWRBTN_EN  (1 << 5)
#define GBL_EN     (1 << 8)

/* Mask of bits in PM1a_STS that can generate an SCI.  Although the ACPI
 * spec lists other bits, the PIIX4, which we are emulating, only
 * supports these three.  For now, we only use TMR_STS; in future we
 * will let qemu set the other bits */
#define SCI_MASK (TMR_STS|PWRBTN_STS|GBL_STS) 

/* SCI IRQ number (must match SCI_INT number in ACPI FADT in hvmloader) */
#define SCI_IRQ 9

/* We provide a 32-bit counter (must match the TMR_VAL_EXT bit in the FADT) */
#define TMR_VAL_MASK  (0xffffffff)
#define TMR_VAL_MSB   (0x80000000)

/* Dispatch SCIs based on the PM1a_STS and PM1a_EN registers */
static void pmt_update_sci(PMTState *s)
{
    ASSERT(spin_is_locked(&s->lock));

    if ( s->pm.pm1a_en & s->pm.pm1a_sts & SCI_MASK )
        hvm_isa_irq_assert(s->vcpu->domain, SCI_IRQ);
    else
        hvm_isa_irq_deassert(s->vcpu->domain, SCI_IRQ);
}

/* Set the correct value in the timer, accounting for time elapsed
 * since the last time we did that. */
static void pmt_update_time(PMTState *s)
{
    uint64_t curr_gtime;
    uint32_t msb = s->pm.tmr_val & TMR_VAL_MSB;
    
    ASSERT(spin_is_locked(&s->lock));

    /* Update the timer */
    curr_gtime = hvm_get_guest_time(s->vcpu);
    s->pm.tmr_val += ((curr_gtime - s->last_gtime) * s->scale) >> 32;
    s->pm.tmr_val &= TMR_VAL_MASK;
    s->last_gtime = curr_gtime;
    
    /* If the counter's MSB has changed, set the status bit */
    if ( (s->pm.tmr_val & TMR_VAL_MSB) != msb )
    {
        s->pm.pm1a_sts |= TMR_STS;
        pmt_update_sci(s);
    }
}

/* This function should be called soon after each time the MSB of the
 * pmtimer register rolls over, to make sure we update the status
 * registers and SCI at least once per rollover */
static void pmt_timer_callback(void *opaque)
{
    PMTState *s = opaque;
    uint32_t pmt_cycles_until_flip;
    uint64_t time_until_flip;

    spin_lock(&s->lock);

    /* Recalculate the timer and make sure we get an SCI if we need one */
    pmt_update_time(s);

    /* How close are we to the next MSB flip? */
    pmt_cycles_until_flip = TMR_VAL_MSB - (s->pm.tmr_val & (TMR_VAL_MSB - 1));

    /* Overall time between MSB flips */
    time_until_flip = (1000000000ULL << 23) / FREQUENCE_PMTIMER;

    /* Reduced appropriately */
    time_until_flip = (time_until_flip * pmt_cycles_until_flip) >> 23;

    /* Wake up again near the next bit-flip */
    set_timer(&s->timer, NOW() + time_until_flip + MILLISECS(1));

    spin_unlock(&s->lock);
}

/* Handle port I/O to the PM1a_STS and PM1a_EN registers */
static int handle_evt_io(
    int dir, uint32_t port, uint32_t bytes, uint32_t *val)
{
    struct vcpu *v = current;
    PMTState *s = &v->domain->arch.hvm_domain.pl_time.vpmt;
    uint32_t addr, data, byte;
    int i;

    spin_lock(&s->lock);

    if ( dir == IOREQ_WRITE )
    {
        /* Handle this I/O one byte at a time */
        for ( i = bytes, addr = port, data = *val;
              i > 0;
              i--, addr++, data >>= 8 )
        {
            byte = data & 0xff;
            switch ( addr )
            {
                /* PM1a_STS register bits are write-to-clear */
            case PM1a_STS_ADDR:
                s->pm.pm1a_sts &= ~byte;
                break;
            case PM1a_STS_ADDR + 1:
                s->pm.pm1a_sts &= ~(byte << 8);
                break;
                
            case PM1a_EN_ADDR:
                s->pm.pm1a_en = (s->pm.pm1a_en & 0xff00) | byte;
                break;
            case PM1a_EN_ADDR + 1:
                s->pm.pm1a_en = (s->pm.pm1a_en & 0xff) | (byte << 8);
                break;
                
            default:
                gdprintk(XENLOG_WARNING, 
                         "Bad ACPI PM register write: %x bytes (%x) at %x\n", 
                         bytes, *val, port);
            }
        }
        /* Fix up the SCI state to match the new register state */
        pmt_update_sci(s);
    }
    else /* p->dir == IOREQ_READ */
    {
        data = s->pm.pm1a_sts | (((uint32_t) s->pm.pm1a_en) << 16);
        data >>= 8 * (port - PM1a_STS_ADDR);
        if ( bytes == 1 ) data &= 0xff;
        else if ( bytes == 2 ) data &= 0xffff;
        *val = data;
    }

    spin_unlock(&s->lock);

    return X86EMUL_OKAY;
}


/* Handle port I/O to the TMR_VAL register */
static int handle_pmt_io(
    int dir, uint32_t port, uint32_t bytes, uint32_t *val)
{
    struct vcpu *v = current;
    PMTState *s = &v->domain->arch.hvm_domain.pl_time.vpmt;

    if ( bytes != 4 )
    {
        gdprintk(XENLOG_WARNING, "HVM_PMT bad access\n");
        return X86EMUL_OKAY;
    }
    
    if ( dir == IOREQ_READ )
    {
        spin_lock(&s->lock);
        pmt_update_time(s);
        *val = s->pm.tmr_val;
        spin_unlock(&s->lock);
        return X86EMUL_OKAY;
    }

    return X86EMUL_UNHANDLEABLE;
}

static int pmtimer_save(struct domain *d, hvm_domain_context_t *h)
{
    PMTState *s = &d->arch.hvm_domain.pl_time.vpmt;
    uint32_t x, msb = s->pm.tmr_val & TMR_VAL_MSB;
    int rc;

    spin_lock(&s->lock);

    /* Update the counter to the guest's current time.  We always save
     * with the domain paused, so the saved time should be after the
     * last_gtime, but just in case, make sure we only go forwards */
    x = ((s->vcpu->arch.hvm_vcpu.guest_time - s->last_gtime) * s->scale) >> 32;
    if ( x < 1UL<<31 )
        s->pm.tmr_val += x;
    if ( (s->pm.tmr_val & TMR_VAL_MSB) != msb )
        s->pm.pm1a_sts |= TMR_STS;
    /* No point in setting the SCI here because we'll already have saved the 
     * IRQ and *PIC state; we'll fix it up when we restore the domain */

    rc = hvm_save_entry(PMTIMER, 0, h, &s->pm);

    spin_unlock(&s->lock);

    return rc;
}

static int pmtimer_load(struct domain *d, hvm_domain_context_t *h)
{
    PMTState *s = &d->arch.hvm_domain.pl_time.vpmt;

    spin_lock(&s->lock);

    /* Reload the registers */
    if ( hvm_load_entry(PMTIMER, h, &s->pm) )
    {
        spin_unlock(&s->lock);
        return -EINVAL;
    }

    /* Calculate future counter values from now. */
    s->last_gtime = hvm_get_guest_time(s->vcpu);

    /* Set the SCI state from the registers */ 
    pmt_update_sci(s);

    spin_unlock(&s->lock);
    
    return 0;
}

HVM_REGISTER_SAVE_RESTORE(PMTIMER, pmtimer_save, pmtimer_load, 
                          1, HVMSR_PER_DOM);

void pmtimer_init(struct vcpu *v)
{
    PMTState *s = &v->domain->arch.hvm_domain.pl_time.vpmt;

    spin_lock_init(&s->lock);

    s->scale = ((uint64_t)FREQUENCE_PMTIMER << 32) / SYSTEM_TIME_HZ;
    s->vcpu = v;

    /* Intercept port I/O (need two handlers because PM1a_CNT is between
     * PM1a_EN and TMR_VAL and is handled by qemu) */
    register_portio_handler(v->domain, TMR_VAL_ADDR, 4, handle_pmt_io);
    register_portio_handler(v->domain, PM1a_STS_ADDR, 4, handle_evt_io);

    /* Set up callback to fire SCIs when the MSB of TMR_VAL changes */
    init_timer(&s->timer, pmt_timer_callback, s, v->processor);
    pmt_timer_callback(s);
}


void pmtimer_deinit(struct domain *d)
{
    PMTState *s = &d->arch.hvm_domain.pl_time.vpmt;
    kill_timer(&s->timer);
}

void pmtimer_reset(struct domain *d)
{
    /* Reset the counter. */
    d->arch.hvm_domain.pl_time.vpmt.pm.tmr_val = 0;
}
