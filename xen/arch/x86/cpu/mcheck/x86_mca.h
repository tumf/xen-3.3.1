/*
 * MCA implementation for AMD K7/K8 CPUs
 * Copyright (c) 2007 Advanced Micro Devices, Inc. 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


/* The MCA/MCE MSRs should not be used anywhere else.
 * They are cpu family/model specific and are only for use
 * in terms of machine check handling.
 * So we define them here rather in <asm/msr.h>.
 */


/* Bitfield of the MSR_IA32_MCG_CAP register */
#define MCG_CAP_COUNT           0x00000000000000ffULL
#define MCG_CTL_P               0x0000000000000100ULL
/* Bits 9-63 are reserved */

/* Bitfield of the MSR_IA32_MCG_STATUS register */
#define MCG_STATUS_RIPV         0x0000000000000001ULL
#define MCG_STATUS_EIPV         0x0000000000000002ULL
#define MCG_STATUS_MCIP         0x0000000000000004ULL
/* Bits 3-63 are reserved */

/* Bitfield of MSR_K8_MCi_STATUS registers */
/* MCA error code */
#define MCi_STATUS_MCA          0x000000000000ffffULL
/* model-specific error code */
#define MCi_STATUS_MSEC         0x00000000ffff0000ULL
/* Other information */
#define MCi_STATUS_OTHER        0x01ffffff00000000ULL
/* processor context corrupt */
#define MCi_STATUS_PCC          0x0200000000000000ULL
/* MSR_K8_MCi_ADDR register valid */
#define MCi_STATUS_ADDRV        0x0400000000000000ULL
/* MSR_K8_MCi_MISC register valid */
#define MCi_STATUS_MISCV        0x0800000000000000ULL
/* error condition enabled */
#define MCi_STATUS_EN           0x1000000000000000ULL
/* uncorrected error */
#define MCi_STATUS_UC           0x2000000000000000ULL
/* status register overflow */
#define MCi_STATUS_OVER         0x4000000000000000ULL
/* valid */
#define MCi_STATUS_VAL          0x8000000000000000ULL

/* Bitfield of MSi_STATUS_OTHER field */
/* reserved bits */
#define MCi_STATUS_OTHER_RESERVED1      0x00001fff00000000ULL
/* uncorrectable ECC error */
#define MCi_STATUS_OTEHR_UC_ECC         0x0000200000000000ULL
/* correctable ECC error */
#define MCi_STATUS_OTHER_C_ECC          0x0000400000000000ULL
/* ECC syndrome of an ECC error */
#define MCi_STATUS_OTHER_ECC_SYNDROME   0x007f800000000000ULL
/* reserved bits */
#define MCi_STATUS_OTHER_RESERVED2      0x0180000000000000ULL

