/* SPDX-License-Identifier: MIT */

#include "chickens.h"
#include "cpu_regs.h"
#include "uart.h"
#include "utils.h"

#define reg_clr(reg, bits)      msr(reg, mrs(reg) & ~(bits))
#define reg_set(reg, bits)      msr(reg, mrs(reg) | bits)
#define reg_mask(reg, clr, set) msr(reg, (mrs(reg) & ~(clr)) | set)

/* Part IDs in MIDR_EL1 */
#define MIDR_PART_M1_ICESTORM  34
#define MIDR_PART_M1_FIRESTORM 35

void init_m1_common(void)
{
    int core = mrs(MPIDR_EL1) & 0xff;

    // Unknown, related to SMP?
    msr(s3_4_c15_c5_0, core);
    msr(s3_4_c15_c1_4, 0x100);
    sysop("isb");

    // Disables a bunch of memory subsystem errors. This patches up some faults, but we'd rather
    // fix the underlying problems.
    // msr(SYS_L2C_ERR_STS, 0);
}

void init_m1_icestorm(void)
{
    // "Sibling Merge in LLC can cause UC load to violate ARM Memory Ordering Rules."
    reg_set(SYS_HID5, HID5_DISABLE_FILL_2C_MERGE);

    reg_clr(SYS_EHID9, EHID9_DEV_THROTTLE_2_ENABLE);

    // "Prevent store-to-load forwarding for UC memory to avoid barrier ordering
    // violation"
    reg_set(SYS_EHID10, HID10_FORCE_WAIT_STATE_DRAIN_UC | HID10_DISABLE_ZVA_TEMPORAL_TSO);

    // FIXME: do we actually need this?
    reg_set(SYS_EHID20, EHID20_TRAP_SMC);

    reg_set(SYS_EHID20, EHID20_FORCE_NONSPEC_IF_OLDEST_REDIR_VALID_AND_OLDER |
                            EHID20_FORCE_NONSPEC_IF_SPEC_FLUSH_POINTER_NE_BLK_RTR_POINTER);

    reg_mask(SYS_EHID20, EHID20_FORCE_NONSPEC_TARGETED_TIMER_SEL_MASK,
             EHID20_FORCE_NONSPEC_TARGETED_TIMER_SEL(3));

    init_m1_common();
}

void init_m1_firestorm(void)
{
    // "Cross-beat Crypto(AES/PMUL) ICache fusion is not disabled for branch
    // uncondtional "recoded instruction."
    reg_set(SYS_HID0,
            HID0_SAME_PG_POWER_OPTIMIZATION | HID0_FETCH_WIDTH_DISABLE | HID0_CACHE_FUSION_DISABLE);

    // FIXME: do we actually need this?
    reg_set(SYS_HID1, HID1_TRAP_SMC);

    reg_clr(SYS_HID3, HID3_DEV_PCIE_THROTTLE_ENABLE | HID3_DISABLE_ARBITER_FIX_BIF_CRD);

    // "Post-silicon tuning of STNT widget contiguous counter threshold"
    reg_mask(SYS_HID4, HID4_STNT_COUNTER_THRESHOLD_MASK, HID4_STNT_COUNTER_THRESHOLD(3));

    // "Sibling Merge in LLC can cause UC load to violate ARM Memory Ordering
    // Rules."
    reg_set(SYS_HID5, HID5_DISABLE_FILL_2C_MERGE);

    reg_mask(SYS_HID6, HID6_UP_CRD_TKN_INIT_C2_MASK, HID6_UP_CRD_TKN_INIT_C2(0));

    reg_set(SYS_HID7, HID7_FORCE_NONSPEC_IF_STEPPING |
                          HID7_FORCE_NONSPEC_IF_SPEC_FLUSH_POINTER_INVALID_AND_MP_VALID);

    reg_mask(SYS_HID7, HID7_FORCE_NONSPEC_TARGET_TIMER_SEL_MASK,
             HID7_FORCE_NONSPEC_TARGET_TIMER_SEL(3));

    reg_set(SYS_HID9,
            HID9_TSO_ALLOW_DC_ZVA_WC | HID9_TSO_SERIALIZE_VLD_MICROOPS | HID9_FIX_BUG_51667805);

    reg_set(SYS_HID11, HID11_DISABLE_LD_NT_WIDGET);

    // "configure dummy cycles to work around incorrect temp sensor readings on
    // NEX power gating"
    reg_mask(SYS_HID13, HID13_PRE_CYCLES_MASK, HID13_PRE_CYCLES(4));

    // Best bit names...
    // Maybe: "RF bank and Multipass conflict forward progress widget does not
    // handle 3+ cycle livelock"
    reg_set(SYS_HID16, HID16_SPAREBIT0 | HID16_SPAREBIT3 | HID16_ENABLE_MPX_PICK_45 |
                           HID16_ENABLE_MP_CYCLONE_7);

    reg_set(SYS_HID18, HID18_HVC_SPECULATION_DISABLE);

    reg_clr(SYS_HID21, HID21_ENABLE_LDREX_FILL_REPLY);

    init_m1_common();
}

const char *init_cpu(void)
{
    const char *cpu = "Unknown";

    msr(OSLAR_EL1, 0);

    /* This is performed unconditionally on all cores (necessary?) */
    if (is_ecore())
        reg_set(SYS_EHID4, HID4_DISABLE_DC_MVA | HID4_DISABLE_DC_SW_L2_OPS);
    else
        reg_set(SYS_HID4, HID4_DISABLE_DC_MVA | HID4_DISABLE_DC_SW_L2_OPS);

    int part = (mrs(MIDR_EL1) >> 4) & 0xfff;

    switch (part) {
        case MIDR_PART_M1_FIRESTORM:
            cpu = "M1 Firestorm";
            init_m1_firestorm();
            break;

        case MIDR_PART_M1_ICESTORM:
            cpu = "M1 Icestorm";
            init_m1_icestorm();
            break;

        default:
            uart_puts("Unknown CPU type");
            break;
    }

    /* Unmask external IRQs, set WFI mode to up (2) */
    reg_mask(SYS_CYC_OVRD, CYC_OVRD_FIQ_MODE_MASK | CYC_OVRD_IRQ_MODE_MASK | CYC_OVRD_WFI_MODE_MASK,
             CYC_OVRD_FIQ_MODE(0) | CYC_OVRD_IRQ_MODE(0) | CYC_OVRD_WFI_MODE(2));

    /* Enable branch prediction state retention across ACC sleep */
    reg_mask(SYS_ACC_CFG, ACC_CFG_BP_SLEEP_MASK, ACC_CFG_BP_SLEEP(3));

    return cpu;
}
