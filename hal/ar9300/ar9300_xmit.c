/*
 * Copyright (c) 2008-2010, Atheros Communications Inc. 
 * All Rights Reserved.
 * 
 * Copyright (c) 2011 Qualcomm Atheros, Inc.
 * All Rights Reserved.
 * Qualcomm Atheros Confidential and Proprietary.
 * 
 */

#include "opt_ah.h"

#include "ah.h"
#include "ah_desc.h"
#include "ah_internal.h"

#include "ar9003/ar9300.h"
#include "ar9003/ar9300reg.h"
#include "ar9003/ar9300phy.h"
#include "ar9003/ar9300desc.h"

#define TU_TO_USEC(_tu)         ((_tu) << 10)
#define ONE_EIGHTH_TU_TO_USEC(_tu8)     ((_tu8) << 7)

/*
 * Update Tx FIFO trigger level.
 *
 * Set b_inc_trig_level to TRUE to increase the trigger level.
 * Set b_inc_trig_level to FALSE to decrease the trigger level.
 *
 * Returns TRUE if the trigger level was updated
 */
HAL_BOOL
ar9300_update_tx_trig_level(struct ath_hal *ah, HAL_BOOL b_inc_trig_level)
{
    struct ath_hal_9300 *ahp = AH9300(ah);
    u_int32_t txcfg, cur_level, new_level;
    HAL_INT omask;

    if (AH9300(ah)->ah_tx_trig_level >= MAX_TX_FIFO_THRESHOLD &&
        b_inc_trig_level)
    {
        return AH_FALSE;
    }

    /*
     * Disable interrupts while futzing with the fifo level.
     */
    omask = ar9300_set_interrupts(ah, ahp->ah_mask_reg &~ HAL_INT_GLOBAL, 0);

    txcfg = OS_REG_READ(ah, AR_TXCFG);
    cur_level = MS(txcfg, AR_FTRIG);
    new_level = cur_level;

    if (b_inc_trig_level)  {   /* increase the trigger level */
        if (cur_level < MAX_TX_FIFO_THRESHOLD) {
            new_level++;
        }
    } else if (cur_level > MIN_TX_FIFO_THRESHOLD) {
        new_level--;
    }

    if (new_level != cur_level) {
        /* Update the trigger level */
        OS_REG_WRITE(ah,
            AR_TXCFG, (txcfg &~ AR_FTRIG) | SM(new_level, AR_FTRIG));
    }

    /* re-enable chip interrupts */
    ar9300_set_interrupts(ah, omask, 0);

    AH9300(ah)->ah_tx_trig_level = new_level;

    return (new_level != cur_level);
}

/*
 * Returns the value of Tx Trigger Level
 */
u_int16_t
ar9300_get_tx_trig_level(struct ath_hal *ah)
{
    return (AH9300(ah)->ah_tx_trig_level);
}

/*
 * Set the properties of the tx queue with the parameters
 * from q_info.
 */
HAL_BOOL
ar9300_set_tx_queue_props(struct ath_hal *ah, int q, const HAL_TXQ_INFO *q_info)
{
    struct ath_hal_9300 *ahp = AH9300(ah);
    HAL_CAPABILITIES *p_cap = &AH_PRIVATE(ah)->ah_caps;

    if (q >= p_cap->halTotalQueues) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: invalid queue num %u\n", __func__, q);
        return AH_FALSE;
    }
    return ath_hal_setTxQProps(ah, &ahp->ah_txq[q], q_info);
}

/*
 * Return the properties for the specified tx queue.
 */
HAL_BOOL
ar9300_get_tx_queue_props(struct ath_hal *ah, int q, HAL_TXQ_INFO *q_info)
{
    struct ath_hal_9300 *ahp = AH9300(ah);
    HAL_CAPABILITIES *p_cap = &AH_PRIVATE(ah)->ah_caps;


    if (q >= p_cap->halTotalQueues) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: invalid queue num %u\n", __func__, q);
        return AH_FALSE;
    }
    return ath_hal_getTxQProps(ah, q_info, &ahp->ah_txq[q]);
}

enum {
    AH_TX_QUEUE_MINUS_OFFSET_BEACON = 1,
    AH_TX_QUEUE_MINUS_OFFSET_CAB    = 2,
    AH_TX_QUEUE_MINUS_OFFSET_UAPSD  = 3,
    AH_TX_QUEUE_MINUS_OFFSET_PAPRD  = 4,
};

/*
 * Allocate and initialize a tx DCU/QCU combination.
 */
int
ar9300_setup_tx_queue(struct ath_hal *ah, HAL_TX_QUEUE type,
        const HAL_TXQ_INFO *q_info)
{
    struct ath_hal_9300 *ahp = AH9300(ah);
    HAL_TX_QUEUE_INFO *qi;
    HAL_CAPABILITIES *p_cap = &AH_PRIVATE(ah)->ah_caps;
    int q;

    /* XXX move queue assignment to driver */
    switch (type) {
    case HAL_TX_QUEUE_BEACON:
        /* highest priority */
        q = p_cap->halTotalQueues - AH_TX_QUEUE_MINUS_OFFSET_BEACON;
        break;
    case HAL_TX_QUEUE_CAB:
        /* next highest priority */
        q = p_cap->halTotalQueues - AH_TX_QUEUE_MINUS_OFFSET_CAB;
        break;
    case HAL_TX_QUEUE_UAPSD:
        q = p_cap->halTotalQueues - AH_TX_QUEUE_MINUS_OFFSET_UAPSD;
        break;
    case HAL_TX_QUEUE_PAPRD:
        q = p_cap->halTotalQueues - AH_TX_QUEUE_MINUS_OFFSET_PAPRD;
        break;
    case HAL_TX_QUEUE_DATA:
        /*
         * don't infringe on top 4 queues, reserved for:
         * beacon, CAB, UAPSD, PAPRD
         */
        for (q = 0;
             q < p_cap->halTotalQueues - AH_TX_QUEUE_MINUS_OFFSET_PAPRD;
             q++)
        {
            if (ahp->ah_txq[q].tqi_type == HAL_TX_QUEUE_INACTIVE) {
                break;
            }
        }
        if (q == p_cap->halTotalQueues - 3) {
            HALDEBUG(ah, HAL_DEBUG_QUEUE,
                "%s: no available tx queue\n", __func__);
            return -1;
        }
        break;
    default:
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: bad tx queue type %u\n", __func__, type);
        return -1;
    }

    HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: queue %u\n", __func__, q);

    qi = &ahp->ah_txq[q];
    if (qi->tqi_type != HAL_TX_QUEUE_INACTIVE) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: tx queue %u already active\n", __func__, q);
        return -1;
    }

    OS_MEMZERO(qi, sizeof(HAL_TX_QUEUE_INFO));
    qi->tqi_type = type;

    if (q_info == AH_NULL) {
        /* by default enable OK+ERR+DESC+URN interrupts */
        qi->tqi_qflags = HAL_TXQ_TXOKINT_ENABLE
                        | HAL_TXQ_TXERRINT_ENABLE
                        | HAL_TXQ_TXDESCINT_ENABLE
                        | HAL_TXQ_TXURNINT_ENABLE;
        qi->tqi_aifs = INIT_AIFS;
        qi->tqi_cwmin = HAL_TXQ_USEDEFAULT;     /* NB: do at reset */
        qi->tqi_cwmax = INIT_CWMAX;
        qi->tqi_shretry = INIT_SH_RETRY;
        qi->tqi_lgretry = INIT_LG_RETRY;
        qi->tqi_physCompBuf = 0;
    } else {
        qi->tqi_physCompBuf = q_info->tqi_compBuf;
        (void) ar9300_set_tx_queue_props(ah, q, q_info);
    }
    /* NB: must be followed by ar9300_reset_tx_queue */
    return q;
}

/*
 * Update the h/w interrupt registers to reflect a tx q's configuration.
 */
static void
set_tx_q_interrupts(struct ath_hal *ah, HAL_TX_QUEUE_INFO *qi)
{
    struct ath_hal_9300 *ahp = AH9300(ah);

    HALDEBUG(ah, HAL_DEBUG_INTERRUPT,
            "%s: tx ok 0x%x err 0x%x eol 0x%x urn 0x%x\n",
            __func__,
            ahp->ah_tx_ok_interrupt_mask,
            ahp->ah_tx_err_interrupt_mask,
            ahp->ah_tx_eol_interrupt_mask,
            ahp->ah_tx_urn_interrupt_mask);

    OS_REG_WRITE(ah, AR_IMR_S0,
              SM(ahp->ah_tx_ok_interrupt_mask, AR_IMR_S0_QCU_TXOK));
    OS_REG_WRITE(ah, AR_IMR_S1,
              SM(ahp->ah_tx_err_interrupt_mask, AR_IMR_S1_QCU_TXERR)
            | SM(ahp->ah_tx_eol_interrupt_mask, AR_IMR_S1_QCU_TXEOL));
    OS_REG_RMW_FIELD(ah,
        AR_IMR_S2, AR_IMR_S2_QCU_TXURN, ahp->ah_tx_urn_interrupt_mask);
    ahp->ah_mask2Reg = OS_REG_READ(ah, AR_IMR_S2);
}

/*
 * Free a tx DCU/QCU combination.
 */
HAL_BOOL
ar9300_release_tx_queue(struct ath_hal *ah, u_int q)
{
    struct ath_hal_9300 *ahp = AH9300(ah);
    HAL_CAPABILITIES *p_cap = &AH_PRIVATE(ah)->ah_caps;
    HAL_TX_QUEUE_INFO *qi;

    if (q >= p_cap->halTotalQueues) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: invalid queue num %u\n", __func__, q);
        return AH_FALSE;
    }

    qi = &ahp->ah_txq[q];
    if (qi->tqi_type == HAL_TX_QUEUE_INACTIVE) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: inactive queue %u\n", __func__, q);
        return AH_FALSE;
    }

    HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: release queue %u\n", __func__, q);

    qi->tqi_type = HAL_TX_QUEUE_INACTIVE;
    ahp->ah_tx_ok_interrupt_mask &= ~(1 << q);
    ahp->ah_tx_err_interrupt_mask &= ~(1 << q);
    ahp->ah_tx_eol_interrupt_mask &= ~(1 << q);
    ahp->ah_tx_urn_interrupt_mask &= ~(1 << q);
    set_tx_q_interrupts(ah, qi);

    return AH_TRUE;
}

/*
 * Set the retry, aifs, cwmin/max, ready_time regs for specified queue
 * Assumes:
 *  phw_channel has been set to point to the current channel
 */
HAL_BOOL
ar9300_reset_tx_queue(struct ath_hal *ah, u_int q)
{
    struct ath_hal_9300     *ahp  = AH9300(ah);
//    struct ath_hal_private  *ap   = AH_PRIVATE(ah);
    HAL_CAPABILITIES        *p_cap = &AH_PRIVATE(ah)->ah_caps;
    const struct ieee80211_channel *chan = AH_PRIVATE(ah)->ah_curchan;
    HAL_TX_QUEUE_INFO       *qi;
    u_int32_t               cw_min, chan_cw_min, value;

    if (q >= p_cap->halTotalQueues) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: invalid queue num %u\n", __func__, q);
        return AH_FALSE;
    }

    qi = &ahp->ah_txq[q];
    if (qi->tqi_type == HAL_TX_QUEUE_INACTIVE) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: inactive queue %u\n", __func__, q);
        return AH_TRUE;         /* XXX??? */
    }

    HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: reset queue %u\n", __func__, q);

    if (qi->tqi_cwmin == HAL_TXQ_USEDEFAULT) {
        /*
         * Select cwmin according to channel type.
         * NB: chan can be NULL during attach
         */
        if (chan && IEEE80211_IS_CHAN_B(chan)) {
            chan_cw_min = INIT_CWMIN_11B;
        } else {
            chan_cw_min = INIT_CWMIN;
        }
        /* make sure that the CWmin is of the form (2^n - 1) */
        for (cw_min = 1; cw_min < chan_cw_min; cw_min = (cw_min << 1) | 1) {}
    } else {
        cw_min = qi->tqi_cwmin;
    }

    /* set cw_min/Max and AIFS values */
    if (q > 3 || (!AH9300(ah)->ah_fccaifs))
       /* values should not be overwritten if domain is FCC and manual rate 
         less than 24Mb is set, this check  is making sure this */
    {
        OS_REG_WRITE(ah, AR_DLCL_IFS(q), SM(cw_min, AR_D_LCL_IFS_CWMIN)
                | SM(qi->tqi_cwmax, AR_D_LCL_IFS_CWMAX)
                | SM(qi->tqi_aifs, AR_D_LCL_IFS_AIFS));
    }

    /* Set retry limit values */
    OS_REG_WRITE(ah, AR_DRETRY_LIMIT(q),
        SM(INIT_SSH_RETRY, AR_D_RETRY_LIMIT_STA_SH) |
        SM(INIT_SLG_RETRY, AR_D_RETRY_LIMIT_STA_LG) |
        SM(qi->tqi_shretry, AR_D_RETRY_LIMIT_FR_SH));

    /* enable early termination on the QCU */
    OS_REG_WRITE(ah, AR_QMISC(q), AR_Q_MISC_DCU_EARLY_TERM_REQ);

    /* enable DCU to wait for next fragment from QCU  */
    if (AR_SREV_WASP(ah) && (AH_PRIVATE((ah))->ah_macRev <= AR_SREV_REVISION_WASP_12)) {
        /* WAR for EV#85395: Wasp Rx overrun issue - reduces Tx queue backoff 
         * threshold to 1 to avoid Rx overruns - Fixed in Wasp 1.3 */
        OS_REG_WRITE(ah, AR_DMISC(q), 
            AR_D_MISC_CW_BKOFF_EN | AR_D_MISC_FRAG_WAIT_EN | 0x1);
    } else {
        OS_REG_WRITE(ah, AR_DMISC(q), 
            AR_D_MISC_CW_BKOFF_EN | AR_D_MISC_FRAG_WAIT_EN | 0x2);
    }

    /* multiqueue support */
    if (qi->tqi_cbrPeriod) {
        OS_REG_WRITE(ah,
            AR_QCBRCFG(q),
            SM(qi->tqi_cbrPeriod, AR_Q_CBRCFG_INTERVAL) |
                SM(qi->tqi_cbrOverflowLimit,
            AR_Q_CBRCFG_OVF_THRESH));
        OS_REG_WRITE(ah, AR_QMISC(q),
            OS_REG_READ(ah, AR_QMISC(q)) |
            AR_Q_MISC_FSP_CBR |
            (qi->tqi_cbrOverflowLimit ?
                AR_Q_MISC_CBR_EXP_CNTR_LIMIT_EN : 0));
    }

    if (qi->tqi_readyTime && (qi->tqi_type != HAL_TX_QUEUE_CAB)) {
        OS_REG_WRITE(ah, AR_QRDYTIMECFG(q),
            SM(qi->tqi_readyTime, AR_Q_RDYTIMECFG_DURATION) |
            AR_Q_RDYTIMECFG_EN);
    }

    OS_REG_WRITE(ah, AR_DCHNTIME(q), SM(qi->tqi_burstTime, AR_D_CHNTIME_DUR) |
                (qi->tqi_burstTime ? AR_D_CHNTIME_EN : 0));

    if (qi->tqi_burstTime &&
        (qi->tqi_qflags & HAL_TXQ_RDYTIME_EXP_POLICY_ENABLE))
    {
        OS_REG_WRITE(ah, AR_QMISC(q), OS_REG_READ(ah, AR_QMISC(q)) |
                     AR_Q_MISC_RDYTIME_EXP_POLICY);
    }

    if (qi->tqi_qflags & HAL_TXQ_BACKOFF_DISABLE) {
        OS_REG_WRITE(ah, AR_DMISC(q), OS_REG_READ(ah, AR_DMISC(q)) |
                    AR_D_MISC_POST_FR_BKOFF_DIS);
    }

    if (qi->tqi_qflags & HAL_TXQ_FRAG_BURST_BACKOFF_ENABLE) {
        OS_REG_WRITE(ah, AR_DMISC(q), OS_REG_READ(ah, AR_DMISC(q)) |
                    AR_D_MISC_FRAG_BKOFF_EN);
    }

    switch (qi->tqi_type) {
    case HAL_TX_QUEUE_BEACON:               /* beacon frames */
        OS_REG_WRITE(ah, AR_QMISC(q),
                    OS_REG_READ(ah, AR_QMISC(q))
                    | AR_Q_MISC_FSP_DBA_GATED
                    | AR_Q_MISC_BEACON_USE
                    | AR_Q_MISC_CBR_INCR_DIS1);

        OS_REG_WRITE(ah, AR_DMISC(q),
                    OS_REG_READ(ah, AR_DMISC(q))
                    | (AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL <<
                    AR_D_MISC_ARB_LOCKOUT_CNTRL_S)
                    | AR_D_MISC_BEACON_USE
                    | AR_D_MISC_POST_FR_BKOFF_DIS);
        /* XXX cwmin and cwmax should be 0 for beacon queue */
        if (AH_PRIVATE(ah)->ah_opmode != HAL_M_IBSS) {
            OS_REG_WRITE(ah, AR_DLCL_IFS(q), SM(0, AR_D_LCL_IFS_CWMIN)
                        | SM(0, AR_D_LCL_IFS_CWMAX)
                        | SM(qi->tqi_aifs, AR_D_LCL_IFS_AIFS));
        }
        break;
    case HAL_TX_QUEUE_CAB:                  /* CAB  frames */
        /*
         * No longer Enable AR_Q_MISC_RDYTIME_EXP_POLICY,
         * bug #6079.  There is an issue with the CAB Queue
         * not properly refreshing the Tx descriptor if
         * the TXE clear setting is used.
         */
        OS_REG_WRITE(ah, AR_QMISC(q),
                        OS_REG_READ(ah, AR_QMISC(q))
                        | AR_Q_MISC_FSP_DBA_GATED
                        | AR_Q_MISC_CBR_INCR_DIS1
                        | AR_Q_MISC_CBR_INCR_DIS0);

        value = TU_TO_USEC(qi->tqi_readyTime)
                - (ah->ah_config.ah_sw_beacon_response_time
                -  ah->ah_config.ah_dma_beacon_response_time)
                - ah->ah_config.ah_additional_swba_backoff;
        OS_REG_WRITE(ah, AR_QRDYTIMECFG(q), value | AR_Q_RDYTIMECFG_EN);

        OS_REG_WRITE(ah, AR_DMISC(q), OS_REG_READ(ah, AR_DMISC(q))
                    | (AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL <<
                                AR_D_MISC_ARB_LOCKOUT_CNTRL_S));
        break;
    case HAL_TX_QUEUE_PSPOLL:
        /*
         * We may configure ps_poll QCU to be TIM-gated in the
         * future; TIM_GATED bit is not enabled currently because
         * of a hardware problem in Oahu that overshoots the TIM
         * bitmap in beacon and may find matching associd bit in
         * non-TIM elements and send PS-poll PS poll processing
         * will be done in software
         */
        OS_REG_WRITE(ah, AR_QMISC(q),
                        OS_REG_READ(ah, AR_QMISC(q)) | AR_Q_MISC_CBR_INCR_DIS1);
        break;
    case HAL_TX_QUEUE_UAPSD:
        OS_REG_WRITE(ah, AR_DMISC(q), OS_REG_READ(ah, AR_DMISC(q))
                    | AR_D_MISC_POST_FR_BKOFF_DIS);
        break;
    default:                        /* NB: silence compiler */
        break;
    }

#ifndef AH_DISABLE_WME
    /*
     * Yes, this is a hack and not the right way to do it, but
     * it does get the lockout bits and backoff set for the
     * high-pri WME queues for testing.  We need to either extend
     * the meaning of queue_info->mode, or create something like
     * queue_info->dcumode.
     */
    if (qi->tqi_intFlags & HAL_TXQ_USE_LOCKOUT_BKOFF_DIS) {
        OS_REG_WRITE(ah, AR_DMISC(q),
            OS_REG_READ(ah, AR_DMISC(q)) |
                SM(AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL,
                    AR_D_MISC_ARB_LOCKOUT_CNTRL) |
                AR_D_MISC_POST_FR_BKOFF_DIS);
    }
#endif

    OS_REG_WRITE(ah, AR_Q_DESC_CRCCHK, AR_Q_DESC_CRCCHK_EN);

    /*
     * Always update the secondary interrupt mask registers - this
     * could be a new queue getting enabled in a running system or
     * hw getting re-initialized during a reset!
     *
     * Since we don't differentiate between tx interrupts corresponding
     * to individual queues - secondary tx mask regs are always unmasked;
     * tx interrupts are enabled/disabled for all queues collectively
     * using the primary mask reg
     */
    if (qi->tqi_qflags & HAL_TXQ_TXOKINT_ENABLE) {
        ahp->ah_tx_ok_interrupt_mask |=  (1 << q);
    } else {
        ahp->ah_tx_ok_interrupt_mask &= ~(1 << q);
    }
    if (qi->tqi_qflags & HAL_TXQ_TXERRINT_ENABLE) {
        ahp->ah_tx_err_interrupt_mask |=  (1 << q);
    } else {
        ahp->ah_tx_err_interrupt_mask &= ~(1 << q);
    }
    if (qi->tqi_qflags & HAL_TXQ_TXEOLINT_ENABLE) {
        ahp->ah_tx_eol_interrupt_mask |=  (1 << q);
    } else {
        ahp->ah_tx_eol_interrupt_mask &= ~(1 << q);
    }
    if (qi->tqi_qflags & HAL_TXQ_TXURNINT_ENABLE) {
        ahp->ah_tx_urn_interrupt_mask |=  (1 << q);
    } else {
        ahp->ah_tx_urn_interrupt_mask &= ~(1 << q);
    }
    set_tx_q_interrupts(ah, qi);

    return AH_TRUE;
}

/*
 * Get the TXDP for the specified queue
 */
u_int32_t
ar9300_get_tx_dp(struct ath_hal *ah, u_int q)
{
    HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);
    return OS_REG_READ(ah, AR_QTXDP(q));
}

/*
 * Set the tx_dp for the specified queue
 */
HAL_BOOL
ar9300_set_tx_dp(struct ath_hal *ah, u_int q, u_int32_t txdp)
{
    HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);
    HALASSERT(AH9300(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE);
    HALASSERT(txdp != 0);

    OS_REG_WRITE(ah, AR_QTXDP(q), txdp);

    return AH_TRUE;
}

/*
 * Transmit Enable is read-only now
 */
HAL_BOOL
ar9300_start_tx_dma(struct ath_hal *ah, u_int q)
{
    return AH_TRUE;
}

/*
 * Return the number of pending frames or 0 if the specified
 * queue is stopped.
 */
u_int32_t
ar9300_num_tx_pending(struct ath_hal *ah, u_int q)
{
    u_int32_t npend;

    HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);

    npend = OS_REG_READ(ah, AR_QSTS(q)) & AR_Q_STS_PEND_FR_CNT;
    if (npend == 0) {
        /*
         * Pending frame count (PFC) can momentarily go to zero
         * while TXE remains asserted.  In other words a PFC of
         * zero is not sufficient to say that the queue has stopped.
         */
        if (OS_REG_READ(ah, AR_Q_TXE) & (1 << q)) {
            npend = 1;              /* arbitrarily return 1 */
        }
    }
#ifdef DEBUG
    if (npend && (AH9300(ah)->ah_txq[q].tqi_type == HAL_TX_QUEUE_CAB)) {
        if (OS_REG_READ(ah, AR_Q_RDYTIMESHDN) & (1 << q)) {
            HALDEBUG(ah, HAL_DEBUG_QUEUE, "RTSD on CAB queue\n");
            /* Clear the ready_time shutdown status bits */
            OS_REG_WRITE(ah, AR_Q_RDYTIMESHDN, 1 << q);
        }
    }
#endif
    HALASSERT((npend == 0) ||
        (AH9300(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE));

    return npend;
}

/*
 * Stop transmit on the specified queue
 */
HAL_BOOL
ar9300_stop_tx_dma(struct ath_hal *ah, u_int q, u_int timeout)
{
    /*
     * Directly call abort.  It is better, hardware-wise, to stop all
     * queues at once than individual ones.
     */
    return ar9300_abort_tx_dma(ah);

#if 0
#define AH_TX_STOP_DMA_TIMEOUT 4000    /* usec */
#define AH_TIME_QUANTUM        100     /* usec */
    u_int wait;

    HALASSERT(q < AH_PRIVATE(ah)->ah_caps.hal_total_queues);

    HALASSERT(AH9300(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE);

    if (timeout == 0) {
        timeout = AH_TX_STOP_DMA_TIMEOUT;
    }

    OS_REG_WRITE(ah, AR_Q_TXD, 1 << q);

    for (wait = timeout / AH_TIME_QUANTUM; wait != 0; wait--) {
        if (ar9300_num_tx_pending(ah, q) == 0) {
            break;
        }
        OS_DELAY(AH_TIME_QUANTUM);        /* XXX get actual value */
    }

#ifdef AH_DEBUG
    if (wait == 0) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: queue %u DMA did not stop in 100 msec\n", __func__, q);
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: QSTS 0x%x Q_TXE 0x%x Q_TXD 0x%x Q_CBR 0x%x\n",
            __func__,
            OS_REG_READ(ah, AR_QSTS(q)),
            OS_REG_READ(ah, AR_Q_TXE),
            OS_REG_READ(ah, AR_Q_TXD),
            OS_REG_READ(ah, AR_QCBRCFG(q)));
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: Q_MISC 0x%x Q_RDYTIMECFG 0x%x Q_RDYTIMESHDN 0x%x\n",
            __func__,
            OS_REG_READ(ah, AR_QMISC(q)),
            OS_REG_READ(ah, AR_QRDYTIMECFG(q)),
            OS_REG_READ(ah, AR_Q_RDYTIMESHDN));
    }
#endif /* AH_DEBUG */

    /* 2413+ and up can kill packets at the PCU level */
    if (ar9300_num_tx_pending(ah, q)) {
        u_int32_t tsf_low, j;

        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: Num of pending TX Frames %d on Q %d\n",
                 __func__, ar9300_num_tx_pending(ah, q), q);

        /* Kill last PCU Tx Frame */
        /* TODO - save off and restore current values of Q1/Q2? */
        for (j = 0; j < 2; j++) {
            tsf_low = OS_REG_READ(ah, AR_TSF_L32);
            OS_REG_WRITE(ah, AR_QUIET2, SM(10, AR_QUIET2_QUIET_DUR));
            OS_REG_WRITE(ah, AR_QUIET_PERIOD, 100);
            OS_REG_WRITE(ah, AR_NEXT_QUIET_TIMER, tsf_low >> 10);
            OS_REG_SET_BIT(ah, AR_TIMER_MODE, AR_QUIET_TIMER_EN);

            if ((OS_REG_READ(ah, AR_TSF_L32) >> 10) == (tsf_low >> 10)) {
                break;
            }

            HALDEBUG(ah, HAL_DEBUG_QUEUE,
                "%s: TSF have moved while trying to set "
                "quiet time TSF: 0x%08x\n",
                __func__, tsf_low);
            /* TSF shouldn't count twice or reg access is taking forever */
            HALASSERT(j < 1);
        }

        OS_REG_SET_BIT(ah, AR_DIAG_SW, AR_DIAG_FORCE_CH_IDLE_HIGH);

        /* Allow the quiet mechanism to do its work */
        OS_DELAY(200);
        OS_REG_CLR_BIT(ah, AR_TIMER_MODE, AR_QUIET_TIMER_EN);

        /* Verify all transmit is dead */
        wait = timeout / AH_TIME_QUANTUM;
        while (ar9300_num_tx_pending(ah, q)) {
            if ((--wait) == 0) {
                HALDEBUG(ah, HAL_DEBUG_TX,
                    "%s: Failed to stop Tx DMA in %d msec "
                    "after killing last frame\n",
                    __func__, timeout / 1000);
                break;
            }
            OS_DELAY(AH_TIME_QUANTUM);
        }

        OS_REG_CLR_BIT(ah, AR_DIAG_SW, AR_DIAG_FORCE_CH_IDLE_HIGH);
    }

    OS_REG_WRITE(ah, AR_Q_TXD, 0);
    return (wait != 0);

#undef AH_TX_STOP_DMA_TIMEOUT
#undef AH_TIME_QUANTUM
#endif
}

/*
 * Really Stop transmit on the specified queue
 */
HAL_BOOL
ar9300_stop_tx_dma_indv_que(struct ath_hal *ah, u_int q, u_int timeout)
{
#define AH_TX_STOP_DMA_TIMEOUT 4000    /* usec */
#define AH_TIME_QUANTUM        100     /* usec */
    u_int wait;

    HALASSERT(q < AH_PRIVATE(ah)->ah_caps.hal_total_queues);

    HALASSERT(AH9300(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE);

    if (timeout == 0) {
        timeout = AH_TX_STOP_DMA_TIMEOUT;
    }

    OS_REG_WRITE(ah, AR_Q_TXD, 1 << q);

    for (wait = timeout / AH_TIME_QUANTUM; wait != 0; wait--) {
        if (ar9300_num_tx_pending(ah, q) == 0) {
            break;
        }
        OS_DELAY(AH_TIME_QUANTUM);        /* XXX get actual value */
    }

#ifdef AH_DEBUG
    if (wait == 0) {
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: queue %u DMA did not stop in 100 msec\n", __func__, q);
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: QSTS 0x%x Q_TXE 0x%x Q_TXD 0x%x Q_CBR 0x%x\n",
            __func__,
            OS_REG_READ(ah, AR_QSTS(q)),
            OS_REG_READ(ah, AR_Q_TXE),
            OS_REG_READ(ah, AR_Q_TXD),
            OS_REG_READ(ah, AR_QCBRCFG(q)));
        HALDEBUG(ah, HAL_DEBUG_QUEUE,
            "%s: Q_MISC 0x%x Q_RDYTIMECFG 0x%x Q_RDYTIMESHDN 0x%x\n",
            __func__,
            OS_REG_READ(ah, AR_QMISC(q)),
            OS_REG_READ(ah, AR_QRDYTIMECFG(q)),
            OS_REG_READ(ah, AR_Q_RDYTIMESHDN));
    }
#endif /* AH_DEBUG */

    /* 2413+ and up can kill packets at the PCU level */
    if (ar9300_num_tx_pending(ah, q)) {
        u_int32_t tsf_low, j;

        HALDEBUG(ah, HAL_DEBUG_QUEUE, "%s: Num of pending TX Frames %d on Q %d\n",
                 __func__, ar9300_num_tx_pending(ah, q), q);

        /* Kill last PCU Tx Frame */
        /* TODO - save off and restore current values of Q1/Q2? */
        for (j = 0; j < 2; j++) {
            tsf_low = OS_REG_READ(ah, AR_TSF_L32);
            OS_REG_WRITE(ah, AR_QUIET2, SM(10, AR_QUIET2_QUIET_DUR));
            OS_REG_WRITE(ah, AR_QUIET_PERIOD, 100);
            OS_REG_WRITE(ah, AR_NEXT_QUIET_TIMER, tsf_low >> 10);
            OS_REG_SET_BIT(ah, AR_TIMER_MODE, AR_QUIET_TIMER_EN);

            if ((OS_REG_READ(ah, AR_TSF_L32) >> 10) == (tsf_low >> 10)) {
                break;
            }

            HALDEBUG(ah, HAL_DEBUG_QUEUE,
                "%s: TSF have moved while trying to set "
                "quiet time TSF: 0x%08x\n",
                __func__, tsf_low);
            /* TSF shouldn't count twice or reg access is taking forever */
            HALASSERT(j < 1);
        }

        OS_REG_SET_BIT(ah, AR_DIAG_SW, AR_DIAG_FORCE_CH_IDLE_HIGH);

        /* Allow the quiet mechanism to do its work */
        OS_DELAY(200);
        OS_REG_CLR_BIT(ah, AR_TIMER_MODE, AR_QUIET_TIMER_EN);

        /* Verify all transmit is dead */
        wait = timeout / AH_TIME_QUANTUM;
        while (ar9300_num_tx_pending(ah, q)) {
            if ((--wait) == 0) {
                HALDEBUG(ah, HAL_DEBUG_TX,
                    "%s: Failed to stop Tx DMA in %d msec "
                    "after killing last frame\n",
                    __func__, timeout / 1000);
                break;
            }
            OS_DELAY(AH_TIME_QUANTUM);
        }

        OS_REG_CLR_BIT(ah, AR_DIAG_SW, AR_DIAG_FORCE_CH_IDLE_HIGH);
    }

    OS_REG_WRITE(ah, AR_Q_TXD, 0);
    return (wait != 0);

#undef AH_TX_STOP_DMA_TIMEOUT
#undef AH_TIME_QUANTUM
}

/*
 * Abort transmit on all queues
 */
#define AR9300_ABORT_LOOPS     1000
#define AR9300_ABORT_WAIT      5
HAL_BOOL
ar9300_abort_tx_dma(struct ath_hal *ah)
{
    int i, q;

    /*
     * set txd on all queues
     */
    OS_REG_WRITE(ah, AR_Q_TXD, AR_Q_TXD_M);

    /*
     * set tx abort bits (also disable rx)
     */
    OS_REG_SET_BIT(ah, AR_PCU_MISC, AR_PCU_FORCE_QUIET_COLL | AR_PCU_CLEAR_VMF);
    OS_REG_SET_BIT(ah, AR_DIAG_SW, (AR_DIAG_FORCE_CH_IDLE_HIGH | AR_DIAG_RX_DIS |
                   AR_DIAG_RX_ABORT | AR_DIAG_FORCE_RX_CLEAR));
    OS_REG_SET_BIT(ah, AR_D_GBL_IFS_MISC, AR_D_GBL_IFS_MISC_IGNORE_BACKOFF);

    /* Let TXE (all queues) clear before waiting on any pending frames */
    for (i = 0; i < AR9300_ABORT_LOOPS; i++) {
        if (OS_REG_READ(ah, AR_Q_TXE) == 0) {
            break;
        }
        OS_DELAY(AR9300_ABORT_WAIT);
    }
    if (i == AR9300_ABORT_LOOPS) {
        HALDEBUG(ah, HAL_DEBUG_TX, "%s[%d] reached max wait on TXE\n",
                 __func__, __LINE__);
    }

    /*
     * wait on all tx queues
     */
    for (q = 0; q < AR_NUM_QCU; q++) {
        for (i = 0; i < AR9300_ABORT_LOOPS; i++) {
            if (!ar9300_num_tx_pending(ah, q)) {
                break;
            }
            OS_DELAY(AR9300_ABORT_WAIT);
        }
        if (i == AR9300_ABORT_LOOPS) {
            HALDEBUG(ah, HAL_DEBUG_TX,
                     "%s[%d] reached max wait on pending tx, q %d\n",
                     __func__, __LINE__, q);
            return AH_FALSE;
        }
    }

    /*
     * clear tx abort bits
     */
    OS_REG_CLR_BIT(ah, AR_PCU_MISC, AR_PCU_FORCE_QUIET_COLL | AR_PCU_CLEAR_VMF);
    OS_REG_CLR_BIT(ah, AR_DIAG_SW, (AR_DIAG_FORCE_CH_IDLE_HIGH | AR_DIAG_RX_DIS |
                   AR_DIAG_RX_ABORT | AR_DIAG_FORCE_RX_CLEAR));
    OS_REG_CLR_BIT(ah, AR_D_GBL_IFS_MISC, AR_D_GBL_IFS_MISC_IGNORE_BACKOFF);

    /*
     * clear txd
     */
    OS_REG_WRITE(ah, AR_Q_TXD, 0);

    return AH_TRUE;
}

/*
 * Determine which tx queues need interrupt servicing.
 */
void
ar9300_get_tx_intr_queue(struct ath_hal *ah, u_int32_t *txqs)
{
    HALDEBUG(AH_NULL, HAL_DEBUG_UNMASKABLE,
                 "ar9300_get_tx_intr_queue: Should not be called\n");
#if 0
    struct ath_hal_9300 *ahp = AH9300(ah);
    *txqs &= ahp->ah_intr_txqs;
    ahp->ah_intr_txqs &= ~(*txqs);
#endif
}

void
ar9300_reset_tx_status_ring(struct ath_hal *ah)
{
    struct ath_hal_9300 *ahp = AH9300(ah);

    ahp->ts_tail = 0;

    /* Zero out the status descriptors */
    OS_MEMZERO((void *)ahp->ts_ring, ahp->ts_size * sizeof(struct ar9300_txs));
    HALDEBUG(ah, HAL_DEBUG_QUEUE,
        "%s: TS Start 0x%x End 0x%x Virt %p, Size %d\n", __func__,
        ahp->ts_paddr_start, ahp->ts_paddr_end, ahp->ts_ring, ahp->ts_size);

    OS_REG_WRITE(ah, AR_Q_STATUS_RING_START, ahp->ts_paddr_start);
    OS_REG_WRITE(ah, AR_Q_STATUS_RING_END, ahp->ts_paddr_end);
}

void
ar9300_setup_tx_status_ring(struct ath_hal *ah, void *ts_start,
    u_int32_t ts_paddr_start, u_int16_t size)
{
    struct ath_hal_9300 *ahp = AH9300(ah);

    ahp->ts_paddr_start = ts_paddr_start;
    ahp->ts_paddr_end = ts_paddr_start + (size * sizeof(struct ar9300_txs));
    ahp->ts_size = size;
    ahp->ts_ring = (struct ar9300_txs *)ts_start;

    ar9300_reset_tx_status_ring(ah);
}
