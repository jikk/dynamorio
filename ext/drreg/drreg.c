/* **********************************************************
 * Copyright (c) 2013-2019 Google, Inc.   All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* DynamoRIO Register Management Extension: a mediator for
 * selecting, preserving, and using registers among multiple
 * instrumentation components.
 */

/* XXX i#511: currently the whole interface is tied to drmgr.
 * Should we also provide an interface that works on standalone instrlists?
 * Distinguish by name, "drregi_*" or sthg.
 */

#include "dr_api.h"
#include "drmgr.h"
#include "drvector.h"
#include "drreg.h"
#include "../ext_utils.h"
#include <string.h>
#include <limits.h>
#include <stddef.h> /* offsetof */

#ifdef DEBUG
#    define ASSERT(x, msg) DR_ASSERT_MSG(x, msg)
#    define LOG(dc, mask, level, ...) dr_log(dc, mask, level, __VA_ARGS__)
#else
#    define ASSERT(x, msg)            /* nothing */
#    define LOG(dc, mask, level, ...) /* nothing */
#endif

#ifdef WINDOWS
#    define DISPLAY_ERROR(msg) dr_messagebox(msg)
#else
#    define DISPLAY_ERROR(msg) dr_fprintf(STDERR, "%s\n", msg);
#endif

#define PRE instrlist_meta_preinsert

/* This is an arbitrary hard-coded upper limit of how many slots drreg is able to track.
 * Note, the client is responsible for reserving enough slots for its use.
 */
#define ARBITRARY_UPPER_LIMIT (SPILL_SLOT_MAX + DR_NUM_GPR_REGS + 1)
#define MAX_SPILLS ARBITRARY_UPPER_LIMIT

/* We choose the number of available slots for spilling SIMDs to arbitrarily match
 * double their theoretical max number for a given build.
 *
 * We add an additional slot for temporary storage used by drreg. For example,
 * it is used to handle cross-app spilling. Note that this is in contrast to GPRs,
 * which require allocated thread storage for cross-app spilling as DR slots are not
 * guaranteed preserve stored data in such cases.
 */
#define MAX_SIMD_SPILLS (DR_NUM_SIMD_VECTOR_REGS * 2)

#ifdef X86
/* Defines whether SIMD and indirect spilling is supported by drreg. */
#    define SIMD_SUPPORTED
#endif

#ifdef SIMD_SUPPORTED
#    define XMM_REG_SIZE 16
#    define YMM_REG_SIZE 32
#    define ZMM_REG_SIZE 64
#    define SIMD_REG_SIZE ZMM_REG_SIZE
#else
/* FIXME i#3844: NYI on ARM */
#endif

#define AFLAGS_SLOT 0 /* always */

/* Liveness states for gprs */
#define REG_DEAD ((void *)(ptr_uint_t)0)
#define REG_LIVE ((void *)(ptr_uint_t)1)
#define REG_UNKNOWN ((void *)(ptr_uint_t)2) /* only used outside drmgr insert phase */

#ifdef SIMD_SUPPORTED
/* Liveness states for SIMD (not for mmx) */
#    define SIMD_XMM_DEAD \
        ((void *)(ptr_uint_t)0) /* first 16 bytes are dead, rest are live */
#    define SIMD_YMM_DEAD \
        ((void *)(ptr_uint_t)1) /* first 32 bytes are dead, rest are live */
#    define SIMD_ZMM_DEAD \
        ((void *)(ptr_uint_t)2) /* first 64 bytes are dead, rest are live */
#    define SIMD_XMM_LIVE \
        ((void *)(ptr_uint_t)3) /* first 16 bytes are live, rest are dead */
#    define SIMD_YMM_LIVE \
        ((void *)(ptr_uint_t)4) /* first 32 bytes are live, rest are dead */
#    define SIMD_ZMM_LIVE \
        ((void *)(ptr_uint_t)5) /* first 64 bytes are live, rest are dead */
#endif
#define SIMD_UNKNOWN ((void *)(ptr_uint_t)6)

typedef struct _reg_info_t {
    /* XXX: better to flip around and store bitvector of registers per instr
     * in a single drvector_t?
     */
    /* The live vector holds one entry per app instr in the bb.
     * For registers, each vector entry holds REG_{LIVE,DEAD}.
     * For aflags, each vector entry holds a ptr_uint_t with the EFLAGS_READ_ARITH bits
     * telling which arithmetic flags are live at that point.
     */
    drvector_t live;
    bool in_use;
    uint app_uses; /* # of uses in this bb by app */
    /* With lazy restore, and b/c we must set native to false, we need to record
     * whether we spilled or not (we could instead record live_idx at time of
     * reservation).
     */
    bool ever_spilled;

    /* Where is the app value for this reg? */
    bool native;   /* app value is in original app reg */
    reg_id_t xchg; /* if !native && != REG_NULL, value was exchanged w/ this dead reg */
    int slot;      /* if !native && xchg==REG_NULL, value is in this TLS slot # */
} reg_info_t;

/* We use this in per_thread_t.slot_use[] and other places */
#define DR_REG_EFLAGS DR_REG_INVALID

#define GPR_IDX(reg) ((reg)-DR_REG_START_GPR)

/* The applicable register range is what's used internally to iterate over all possible
 * SIMD registers for a given build. Regs are resized to zmm when testing via SIMD_IDX().
 */
#ifdef SIMD_SUPPORTED
#    define DR_REG_APPLICABLE_START_SIMD DR_REG_START_ZMM
#    define DR_REG_APPLICABLE_STOP_SIMD DR_REG_STOP_ZMM
#    define SIMD_IDX(reg) ((reg_resize_to_opsz(reg, OPSZ_64)) - DR_REG_START_ZMM)
#else
/* FIXME i#3844: NYI on ARM */
#endif

typedef struct _per_thread_t {
    instr_t *cur_instr;
    int live_idx;
    reg_info_t reg[DR_NUM_GPR_REGS];
    reg_info_t simd_reg[DR_NUM_SIMD_VECTOR_REGS];
    byte *simd_spill_start; /* storage returned by allocator (may not be aligned) */
    byte *simd_spills;      /* aligned storage for SIMD data */
    reg_info_t aflags;
    reg_id_t slot_use[MAX_SPILLS]; /* holds the reg_id_t of which reg is inside */
    reg_id_t simd_slot_use[MAX_SIMD_SPILLS]; /* importantly, this can store partial SIMD
                                                registers  */
    int pending_unreserved;      /* count of to-be-lazily-restored unreserved gpr regs */
    int simd_pending_unreserved; /* count of to-be-lazily-restored unreserved SIMD regs */
    /* We store the linear address of our TLS for access from another thread: */
    byte *tls_seg_base;
    /* bb-local values */
    drreg_bb_properties_t bb_props;
    bool bb_has_internal_flow;
} per_thread_t;

static drreg_options_t ops;

static int tls_idx = -1;
/* The raw tls segment offset of the pointer to the SIMD block indirect spill area. */
static uint tls_simd_offs;
/* The raw tls segment offset of tls slots for gpr registers. */
static uint tls_slot_offs;
static reg_id_t tls_seg;

#ifdef DEBUG
static uint stats_max_slot;
#endif

static per_thread_t *
get_tls_data(void *drcontext);

static drreg_status_t
drreg_restore_reg_now(void *drcontext, instrlist_t *ilist, instr_t *inst,
                      per_thread_t *pt, reg_id_t reg);

static void
drreg_move_aflags_from_reg(void *drcontext, instrlist_t *ilist, instr_t *where,
                           per_thread_t *pt, bool stateful);

static drreg_status_t
drreg_restore_aflags(void *drcontext, instrlist_t *ilist, instr_t *where,
                     per_thread_t *pt, bool release);

static drreg_status_t
drreg_spill_aflags(void *drcontext, instrlist_t *ilist, instr_t *where, per_thread_t *pt);

static drreg_status_t
drreg_reserve_reg_internal(void *drcontext, drreg_spill_class_t spill_class,
                           instrlist_t *ilist, instr_t *where, drvector_t *reg_allowed,
                           bool only_if_no_spill, OUT reg_id_t *reg_out);

static void
drreg_report_error(drreg_status_t res, const char *msg)
{
    if (ops.error_callback != NULL) {
        if ((*ops.error_callback)(res))
            return;
    }
    ASSERT(false, msg);
    DISPLAY_ERROR(msg);
    dr_abort();
}

#ifdef DEBUG
static inline app_pc
get_where_app_pc(instr_t *where)
{
    if (where == NULL)
        return NULL;
    return instr_get_app_pc(where);
}
#endif

/***************************************************************************
 * SPILLING AND RESTORING
 */

static uint
find_free_slot(per_thread_t *pt)
{
    uint i;
    /* 0 is always reserved for AFLAGS_SLOT */
    ASSERT(AFLAGS_SLOT == 0, "AFLAGS_SLOT is not 0");
    for (i = AFLAGS_SLOT + 1; i < MAX_SPILLS; i++) {
        if (pt->slot_use[i] == DR_REG_NULL)
            return i;
    }
    return MAX_SPILLS;
}

#ifdef SIMD_SUPPORTED
static uint
find_simd_free_slot(per_thread_t *pt)
{
    ASSERT(ops.num_spill_simd_slots > 0,
           "cannot find free SIMD slots if non were initially requested");
    uint i;
    for (i = 0; i < ops.num_spill_simd_slots; i++) {
        if (pt->simd_slot_use[i] == DR_REG_NULL)
            return i;
    }
    return MAX_SIMD_SPILLS;
}
#endif

/* Up to caller to update pt->reg, including .ever_spilled.
 * This routine updates pt->slot_use.
 *
 * This routine is used for gpr spills as such registers can be directly
 * stored in tls slots.
 */
static void
spill_reg_directly(void *drcontext, per_thread_t *pt, reg_id_t reg, uint slot,
                   instrlist_t *ilist, instr_t *where)
{
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX " %s %d\n", __FUNCTION__, pt->live_idx,
        get_where_app_pc(where), get_register_name(reg), slot);
    ASSERT(pt->slot_use[slot] == DR_REG_NULL || pt->slot_use[slot] == reg ||
               /* aflags can be saved and restored using different regs */
               slot == AFLAGS_SLOT,
           "internal tracking error");
    if (slot == AFLAGS_SLOT)
        pt->aflags.ever_spilled = true;
    pt->slot_use[slot] = reg;
    if (slot < ops.num_spill_slots) {
        dr_insert_write_raw_tls(drcontext, ilist, where, tls_seg,
                                tls_slot_offs + slot * sizeof(reg_t), reg);
    } else {
        dr_spill_slot_t DR_slot = (dr_spill_slot_t)(slot - ops.num_spill_slots);
        dr_save_reg(drcontext, ilist, where, reg, DR_slot);
    }
#ifdef DEBUG
    if (slot > stats_max_slot)
        stats_max_slot = slot; /* racy but that's ok */
#endif
}

#ifdef SIMD_SUPPORTED
static void
load_indirect_block(void *drcontext, per_thread_t *pt, uint slot, instrlist_t *ilist,
                    instr_t *where, reg_id_t scratch_block_reg)
{
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX " %s %d\n", __FUNCTION__, pt->live_idx,
        get_where_app_pc(where), get_register_name(scratch_block_reg), slot);
    /* Simply load the pointer of the block to the passed register. */
    dr_insert_read_raw_tls(drcontext, ilist, where, tls_seg, slot, scratch_block_reg);
}
#endif

#ifdef SIMD_SUPPORTED
/* Up to caller to update pt->simd_reg, including .ever_spilled.
 * This routine updates pt->simd_slot_use.
 *
 * This routine is used for simd spills as such registers are indirectly
 * stored in a separately allocated area pointed to by a hidden tls slot.
 */
static void
spill_reg_indirectly(void *drcontext, per_thread_t *pt, reg_id_t reg, uint slot,
                     instrlist_t *ilist, instr_t *where)
{
    reg_id_t scratch_block_reg = DR_REG_NULL;
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX " %s %d\n", __FUNCTION__, pt->live_idx,
        get_where_app_pc(where), get_register_name(reg), slot);
    ASSERT(reg_is_vector_simd(reg), "not applicable register");
    ASSERT(pt->simd_slot_use[slot] == DR_REG_NULL ||
               reg_resize_to_opsz(pt->simd_slot_use[slot], OPSZ_64) ==
                   reg_resize_to_opsz(reg, OPSZ_64),
           "internal tracking error");
    drreg_status_t res = drreg_reserve_reg_internal(
        drcontext, DRREG_GPR_SPILL_CLASS, ilist, where, NULL, false, &scratch_block_reg);
    if (res != DRREG_SUCCESS)
        drreg_report_error(res, "failed to reserve tmp register");
    ASSERT(scratch_block_reg != DR_REG_NULL, "invalid register");
    ASSERT(pt->simd_spills != NULL, "SIMD spill storage cannot be NULL");
    ASSERT(slot < ops.num_spill_simd_slots,
           "using slots that is out-of-bounds to the number of SIMD slots requested");
    load_indirect_block(drcontext, pt, tls_simd_offs, ilist, where, scratch_block_reg);
    /* TODO i#3844: This needs to be updated according to its larger simd size when
     * supporting ymm and zmm registers in the future.
     */
    pt->simd_slot_use[slot] = reg;
    if (reg_is_strictly_xmm(reg)) {
        opnd_t mem_opnd = opnd_create_base_disp(scratch_block_reg, DR_REG_NULL, 1,
                                                slot * SIMD_REG_SIZE, OPSZ_16);
        opnd_t spill_reg_opnd = opnd_create_reg(reg);
        PRE(ilist, where, INSTR_CREATE_movdqa(drcontext, mem_opnd, spill_reg_opnd));
    } else if (reg_is_strictly_ymm(reg)) {
        /* The callers should catch this when checking the spill class. */
        ASSERT(false, "internal error: ymm registers are not supported yet.");
    } else if (reg_is_strictly_zmm(reg)) {
        /* The callers should catch this when checking the spill class. */
        ASSERT(false, "internal error: zmm registers are not supported yet.");
    } else {
        ASSERT(false, "internal error: not applicable register");
    }
    res = drreg_unreserve_register(drcontext, ilist, where, scratch_block_reg);
    if (res != DRREG_SUCCESS)
        drreg_report_error(res, "failed to unreserve tmp register");
}
#endif

/* Up to caller to update pt->reg. This routine updates pt->slot_use if release==true. */
static void
restore_reg_directly(void *drcontext, per_thread_t *pt, reg_id_t reg, uint slot,
                     instrlist_t *ilist, instr_t *where, bool release)
{
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX " %s slot=%d release=%d\n", __FUNCTION__,
        pt->live_idx, get_where_app_pc(where), get_register_name(reg), slot, release);
    ASSERT(pt->slot_use[slot] == reg ||
               /* aflags can be saved and restored using different regs */
               (slot == AFLAGS_SLOT && pt->slot_use[slot] != DR_REG_NULL),
           "internal tracking error");
    if (release)
        pt->slot_use[slot] = DR_REG_NULL;
    if (slot < ops.num_spill_slots) {
        dr_insert_read_raw_tls(drcontext, ilist, where, tls_seg,
                               tls_slot_offs + slot * sizeof(reg_t), reg);
    } else {
        dr_spill_slot_t DR_slot = (dr_spill_slot_t)(slot - ops.num_spill_slots);
        dr_restore_reg(drcontext, ilist, where, reg, DR_slot);
    }
}

#ifdef SIMD_SUPPORTED
/* Up to caller to update pt->simd_reg. This routine updates pt->simd_slot_use if
 * release==true. */
static void
restore_reg_indirectly(void *drcontext, per_thread_t *pt, reg_id_t reg, uint slot,
                       instrlist_t *ilist, instr_t *where, bool release)
{
    reg_id_t scratch_block_reg = DR_REG_NULL;
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX " %s slot=%d release=%d\n", __FUNCTION__,
        pt->live_idx, get_where_app_pc(where), get_register_name(reg), slot, release);
    ASSERT(reg_is_vector_simd(reg), "not applicable register");
    ASSERT(pt->simd_slot_use[slot] != DR_REG_NULL &&
               reg_resize_to_opsz(pt->simd_slot_use[slot], OPSZ_64) ==
                   reg_resize_to_opsz(reg, OPSZ_64) &&
               pt->simd_slot_use[slot] >= reg,
           "internal tracking error");
    drreg_status_t res = drreg_reserve_reg_internal(
        drcontext, DRREG_GPR_SPILL_CLASS, ilist, where, NULL, false, &scratch_block_reg);
    if (res != DRREG_SUCCESS)
        drreg_report_error(res, "failed to reserve scratch block register");
    ASSERT(scratch_block_reg != DR_REG_NULL, "invalid register");
    ASSERT(pt->simd_spills != NULL, "SIMD spill storage cannot be NULL");
    ASSERT(slot < ops.num_spill_simd_slots,
           "using slots that is out-of-bounds to the number of SIMD slots requested");
    load_indirect_block(drcontext, pt, tls_simd_offs, ilist, where, scratch_block_reg);
    if (release && pt->simd_slot_use[slot] == reg)
        pt->simd_slot_use[slot] = DR_REG_NULL;

    if (reg_is_strictly_xmm(reg)) {
        opnd_t mem_opnd = opnd_create_base_disp(scratch_block_reg, DR_REG_NULL, 0,
                                                slot * SIMD_REG_SIZE, OPSZ_16);
        opnd_t restore_reg_opnd = opnd_create_reg(reg);
        PRE(ilist, where, INSTR_CREATE_movdqa(drcontext, restore_reg_opnd, mem_opnd));
    } else if (reg_is_strictly_ymm(reg)) {
        /* The callers should catch this when checking the spill class. */
        ASSERT(false, "internal error: ymm registers are not supported yet.");
    } else if (reg_is_strictly_zmm(reg)) {
        /* The callers should catch this when checking the spill class. */
        ASSERT(false, "internal error: zmm registers are not supported yet.");
    } else {
        ASSERT(false, "internal error: not an applicable register.");
    }
    res = drreg_unreserve_register(drcontext, ilist, where, scratch_block_reg);
    if (res != DRREG_SUCCESS)
        drreg_report_error(res, "failed to unreserve tmp register");
}
#endif

static reg_t
get_directly_spilled_value(void *drcontext, uint slot)
{
    if (slot < ops.num_spill_slots) {
        per_thread_t *pt = get_tls_data(drcontext);
        return *(reg_t *)(pt->tls_seg_base + tls_slot_offs + slot * sizeof(reg_t));
    } else {
        dr_spill_slot_t DR_slot = (dr_spill_slot_t)(slot - ops.num_spill_slots);
        return dr_read_saved_reg(drcontext, DR_slot);
    }
}

#ifdef SIMD_SUPPORTED
static bool
get_indirectly_spilled_value(void *drcontext, reg_id_t reg, uint slot,
                             OUT byte *value_buf, size_t buf_size)
{
    ASSERT(value_buf != NULL, "value buffer not initialised");
    if (value_buf == NULL)
        return false;
    /* Get the size of the register so we can ensure that the buffer size is adequate. */
    size_t reg_size = opnd_size_in_bytes(reg_get_size(reg));
    ASSERT(buf_size >= reg_size, "value buffer too small in size");
    if (buf_size < reg_size)
        return false;
    if (reg_is_vector_simd(reg)) {
        per_thread_t *pt = get_tls_data(drcontext);
        ASSERT(pt->simd_spills != NULL, "SIMD spill storage cannot be NULL");
        if (reg_is_strictly_xmm(reg)) {
            memcpy(value_buf, pt->simd_spills + (slot * SIMD_REG_SIZE), reg_size);
            return true;
        } else if (reg_is_strictly_ymm(reg)) {
            /* The callers should catch this when checking the spill class. */
            ASSERT(false, "internal error: ymm registers are not supported yet.");
        } else if (reg_is_strictly_zmm(reg)) {
            /* The callers should catch this when checking the spill class. */
            ASSERT(false, "internal error: zmm registers are not supported yet.");
        } else {
            ASSERT(false, "internal error: not an applicable register.");
        }
    }
    ASSERT(false, "not an applicable register.");
    return false;
}
#endif

drreg_status_t
drreg_max_slots_used(OUT uint *max)
{
#ifdef DEBUG
    if (max == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;
    *max = stats_max_slot;
    return DRREG_SUCCESS;
#else
    return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
#endif
}

/***************************************************************************
 * ANALYSIS AND CROSS-APP-INSTR
 */

#ifdef SIMD_SUPPORTED

static bool
is_partial_simd_read(instr_t *instr, reg_id_t reg)
{
    int i;
    opnd_t opnd;

    for (i = 0; i < instr_num_srcs(instr); i++) {
        opnd = instr_get_src(instr, i);
        if (opnd_is_reg(opnd) && opnd_get_reg(opnd) == reg &&
            opnd_get_size(opnd) < reg_get_size(reg))
            return true;
    }

    return false;
}

/* Returns true if state has been set. */
static bool
determine_simd_liveness_state(void *drcontext, instr_t *inst, reg_id_t reg, void **value)
{
    ASSERT(value != NULL, "cannot be NULL");
    ASSERT(reg_is_vector_simd(reg), "must be a vector SIMD register");

    /* Reason over partial registers in SIMD case to achieve efficient spilling. */
    reg_id_t xmm_reg = reg_resize_to_opsz(reg, OPSZ_16);
    reg_id_t ymm_reg = reg_resize_to_opsz(reg, OPSZ_32);
    reg_id_t zmm_reg = reg_resize_to_opsz(reg, OPSZ_64);

    /* It is important to give precedence to bigger registers.
     * If both ZMM0 and YMM0 are read and therefore live, then
     * SIMD_ZMM_LIVE must be assigned and not SIMD_YMM_LIVE.
     *
     * The inverse also needs to be maintained. If both
     * ZMM0 and YMM0 are dead, then SIMD_ZMM_DEAD must be
     * assigned and not SIMD_YMM_DEAD.
     *
     * This is important to achieve efficient spilling/restoring.
     */
    if (instr_reads_from_reg(inst, zmm_reg, DR_QUERY_INCLUDE_COND_SRCS)) {
        if ((instr_reads_from_exact_reg(inst, zmm_reg, DR_QUERY_INCLUDE_COND_SRCS) ||
             is_partial_simd_read(inst, zmm_reg)) &&
            (*value <= SIMD_ZMM_LIVE || *value == SIMD_UNKNOWN)) {
            *value = SIMD_ZMM_LIVE;
        } else if ((instr_reads_from_exact_reg(inst, ymm_reg,
                                               DR_QUERY_INCLUDE_COND_SRCS) ||
                    is_partial_simd_read(inst, ymm_reg)) &&
                   (*value <= SIMD_YMM_LIVE || *value == SIMD_UNKNOWN)) {
            *value = SIMD_YMM_LIVE;
        } else if ((instr_reads_from_exact_reg(inst, xmm_reg,
                                               DR_QUERY_INCLUDE_COND_SRCS) ||
                    is_partial_simd_read(inst, xmm_reg)) &&
                   (*value <= SIMD_XMM_LIVE || *value == SIMD_UNKNOWN)) {
            *value = SIMD_XMM_LIVE;
        } else {
            DR_ASSERT_MSG(false, "failed to handle SIMD read");
            *value = SIMD_ZMM_LIVE;
        }
        return true;
    }

    if (instr_writes_to_reg(inst, zmm_reg, DR_QUERY_INCLUDE_COND_SRCS)) {
        if (instr_writes_to_exact_reg(inst, zmm_reg, DR_QUERY_INCLUDE_COND_SRCS)) {
            *value = SIMD_ZMM_DEAD;
            return true;
        } else if (instr_writes_to_exact_reg(inst, ymm_reg, DR_QUERY_INCLUDE_COND_SRCS) &&
                   (*value < SIMD_YMM_DEAD || *value >= SIMD_XMM_LIVE)) {
            *value = SIMD_YMM_DEAD;
            return true;
        } else if (instr_writes_to_exact_reg(inst, xmm_reg, DR_QUERY_INCLUDE_COND_SRCS) &&
                   *value >= SIMD_XMM_LIVE) {
            *value = SIMD_XMM_DEAD;
            return true;
        }
        /* We may partially write to above registers, which does not make them dead.
         */
    }
    return false;
}
#endif

static void
count_app_uses(per_thread_t *pt, opnd_t opnd)
{
    int i;
    for (i = 0; i < opnd_num_regs_used(opnd); i++) {
        reg_id_t reg = opnd_get_reg_used(opnd, i);
        if (reg_is_gpr(reg)) {
            reg = reg_to_pointer_sized(reg);
            pt->reg[GPR_IDX(reg)].app_uses++;
            /* Tools that instrument memory uses (memtrace, Dr. Memory, etc.)
             * want to double-count memory opnd uses, as they need to restore
             * the app value to get the memory address into a register there.
             * We go ahead and do that for all tools.
             */
            if (opnd_is_memory_reference(opnd))
                pt->reg[GPR_IDX(reg)].app_uses++;
        }
#ifdef SIMD_SUPPORTED
        else if (reg_is_vector_simd(reg)) {
            pt->simd_reg[SIMD_IDX(reg)].app_uses++;
        }
#endif
    }
}

/* This event has to go last, to handle labels inserted by other components:
 * else our indices get off, and we can't simply skip labels in the
 * per-instr event b/c we need the liveness to advance at the label
 * but not after the label.
 */
static dr_emit_flags_t
drreg_event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                        bool translating, OUT void **user_data)
{
    per_thread_t *pt = get_tls_data(drcontext);
    instr_t *inst;
    ptr_uint_t aflags_new, aflags_cur = 0;
    uint index = 0;
    reg_id_t reg;

    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++)
        pt->reg[GPR_IDX(reg)].app_uses = 0;
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++)
        pt->simd_reg[SIMD_IDX(reg)].app_uses = 0;
#endif
    /* pt->bb_props is set to 0 at thread init and after each bb */
    pt->bb_has_internal_flow = false;

    /* Reverse scan is more efficient.  This means our indices are also reversed. */
    for (inst = instrlist_last(bb); inst != NULL; inst = instr_get_prev(inst)) {
        /* We consider both meta and app instrs, to handle rare cases of meta instrs
         * being inserted during app2app for corner cases. An example are app2app
         * emulation functions like drx_expand_scatter_gather().
         */

        bool xfer =
            (instr_is_cti(inst) || instr_is_interrupt(inst) || instr_is_syscall(inst));

        if (!pt->bb_has_internal_flow && (instr_is_ubr(inst) || instr_is_cbr(inst)) &&
            opnd_is_instr(instr_get_target(inst))) {
            /* i#1954: we disable some opts in the presence of control flow. */
            pt->bb_has_internal_flow = true;
            LOG(drcontext, DR_LOG_ALL, 2,
                "%s @%d." PFX ": disabling lazy restores due to intra-bb control flow\n",
                __FUNCTION__, index, get_where_app_pc(inst));
        }

        /* GPR liveness */
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ":", __FUNCTION__, index,
            get_where_app_pc(inst));
        for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
            void *value = REG_LIVE;
            /* DRi#1849: COND_SRCS here includes addressing regs in dsts */
            if (instr_reads_from_reg(inst, reg, DR_QUERY_INCLUDE_COND_SRCS))
                value = REG_LIVE;
            /* make sure we don't consider writes to sub-regs */
            else if (instr_writes_to_exact_reg(inst, reg, DR_QUERY_INCLUDE_COND_SRCS)
                     /* a write to a 32-bit reg for amd64 zeroes the top 32 bits */
                     IF_X86_64(||
                               instr_writes_to_exact_reg(inst, reg_64_to_32(reg),
                                                         DR_QUERY_INCLUDE_COND_SRCS)))
                value = REG_DEAD;
            else if (xfer)
                value = REG_LIVE;
            else if (index > 0)
                value = drvector_get_entry(&pt->reg[GPR_IDX(reg)].live, index - 1);
            LOG(drcontext, DR_LOG_ALL, 3, " %s=%d", get_register_name(reg),
                (int)(ptr_uint_t)value);
            drvector_set_entry(&pt->reg[GPR_IDX(reg)].live, index, value);
        }
#ifdef SIMD_SUPPORTED
        /* SIMD liveness */
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ":", __FUNCTION__, index,
            get_where_app_pc(inst));
        for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD;
             reg++) {
            void *value = SIMD_UNKNOWN;
            if (!determine_simd_liveness_state(drcontext, inst, reg, &value)) {
                if (xfer)
                    value = SIMD_ZMM_LIVE;
                else if (index > 0) {
                    value =
                        drvector_get_entry(&pt->simd_reg[SIMD_IDX(reg)].live, index - 1);
                }
            }
            LOG(drcontext, DR_LOG_ALL, 3, " %s=%d", get_register_name(reg),
                (int)(ptr_uint_t)value);
            drvector_set_entry(&pt->simd_reg[SIMD_IDX(reg)].live, index, value);
        }
#endif
        /* aflags liveness */
        aflags_new = instr_get_arith_flags(inst, DR_QUERY_INCLUDE_COND_SRCS);
        if (xfer)
            aflags_cur = EFLAGS_READ_ARITH; /* assume flags are read before written */
        else {
            uint aflags_read, aflags_w2r;
            if (index == 0)
                aflags_cur = EFLAGS_READ_ARITH; /* assume flags are read before written */
            else {
                aflags_cur =
                    (uint)(ptr_uint_t)drvector_get_entry(&pt->aflags.live, index - 1);
            }
            aflags_read = (aflags_new & EFLAGS_READ_ARITH);
            /* if a flag is read by inst, set the read bit */
            aflags_cur |= (aflags_new & EFLAGS_READ_ARITH);
            /* if a flag is written and not read by inst, clear the read bit */
            aflags_w2r = EFLAGS_WRITE_TO_READ(aflags_new & EFLAGS_WRITE_ARITH);
            aflags_cur &= ~(aflags_w2r & ~aflags_read);
        }
        LOG(drcontext, DR_LOG_ALL, 3, " flags=%d\n", aflags_cur);
        drvector_set_entry(&pt->aflags.live, index, (void *)(ptr_uint_t)aflags_cur);

        if (instr_is_app(inst)) {
            int i;
            for (i = 0; i < instr_num_dsts(inst); i++)
                count_app_uses(pt, instr_get_dst(inst, i));
            for (i = 0; i < instr_num_srcs(inst); i++)
                count_app_uses(pt, instr_get_src(inst, i));
        }

        index++;
    }

    pt->live_idx = index;

    return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t
drreg_event_bb_insert_early(void *drcontext, void *tag, instrlist_t *bb, instr_t *inst,
                            bool for_trace, bool translating, void *user_data)
{
    per_thread_t *pt = get_tls_data(drcontext);
    pt->cur_instr = inst;
    pt->live_idx--; /* counts backward */
    return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t
drreg_event_bb_insert_late(void *drcontext, void *tag, instrlist_t *bb, instr_t *inst,
                           bool for_trace, bool translating, void *user_data)
{
    per_thread_t *pt = get_tls_data(drcontext);
    reg_id_t reg;
    instr_t *next = instr_get_next(inst);
    bool restored_for_read[DR_NUM_GPR_REGS];
#ifdef SIMD_SUPPORTED
    bool restored_for_simd_read[DR_NUM_SIMD_VECTOR_REGS];
#endif
    drreg_status_t res;
    dr_pred_type_t pred = instrlist_get_auto_predicate(bb);

    /* XXX i#2585: drreg should predicate spills and restores as appropriate */
    instrlist_set_auto_predicate(bb, DR_PRED_NONE);
    /* For unreserved regs still spilled, we lazily do the restore here.  We also
     * update reserved regs wrt app uses.
     * The instruction list presented to us here are app instrs but may contain meta
     * instrs if any were inserted in app2app. Any such meta instr here will be treated
     * like an app instr.
     */

    /* Before each app read, or at end of bb, restore aflags to app value */
    uint aflags = (uint)(ptr_uint_t)drvector_get_entry(&pt->aflags.live, pt->live_idx);
    if (!pt->aflags.native &&
        (drmgr_is_last_instr(drcontext, inst) ||
         TESTANY(EFLAGS_READ_ARITH, instr_get_eflags(inst, DR_QUERY_DEFAULT)) ||
         /* Writing just a subset needs to combine with the original unwritten */
         (TESTANY(EFLAGS_WRITE_ARITH, instr_get_eflags(inst, DR_QUERY_INCLUDE_ALL)) &&
          aflags != 0 /*0 means everything is dead*/) ||
         /* DR slots are not guaranteed across app instrs */
         pt->aflags.slot >= (int)ops.num_spill_slots)) {
        /* Restore aflags to app value */
        LOG(drcontext, DR_LOG_ALL, 3,
            "%s @%d." PFX " aflags=0x%x use=%d: lazily restoring aflags\n", __FUNCTION__,
            pt->live_idx, get_where_app_pc(inst), aflags, pt->aflags.in_use);
        res = drreg_restore_aflags(drcontext, bb, inst, pt, false /*keep slot*/);
        if (res != DRREG_SUCCESS)
            drreg_report_error(res, "failed to restore flags before app read");
        if (!pt->aflags.in_use) {
            pt->aflags.native = true;
            pt->slot_use[AFLAGS_SLOT] = DR_REG_NULL;
        }
    }

    /* Before each app read, or at end of bb, restore spilled registers to app values: */
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++) {
        restored_for_simd_read[SIMD_IDX(reg)] = false;
        if (!pt->simd_reg[SIMD_IDX(reg)].native) {
            ASSERT(ops.num_spill_simd_slots > 0, "requested SIMD slots cannot be zero");
            if (drmgr_is_last_instr(drcontext, inst) ||
                /* This covers reads from all simds, because the applicable range
                 * resembles zmm, and all other x86 simds are included in zmm.
                 */
                instr_reads_from_reg(inst, reg, DR_QUERY_INCLUDE_ALL) ||
                /* FIXME i#3844: For ymm and zmm support, we're missing support to restore
                 * upon a partial simd write. For example a write to xmm while zmm is
                 * clobbered, or a partial write with an evex mask.
                 */
                /* i#1954: for complex bbs we must restore before the next app instr. */
                (!pt->simd_reg[SIMD_IDX(reg)].in_use &&
                 ((pt->bb_has_internal_flow &&
                   !TEST(DRREG_IGNORE_CONTROL_FLOW, pt->bb_props)) ||
                  TEST(DRREG_CONTAINS_SPANNING_CONTROL_FLOW, pt->bb_props)))) {
                if (!pt->simd_reg[SIMD_IDX(reg)].in_use) {
                    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": lazily restoring %s\n",
                        __FUNCTION__, pt->live_idx, get_where_app_pc(inst),
                        get_register_name(reg));
                    res = drreg_restore_reg_now(drcontext, bb, inst, pt, reg);
                    if (res != DRREG_SUCCESS)
                        drreg_report_error(res, "lazy restore failed");
                    ASSERT(pt->simd_pending_unreserved > 0, "should not go negative");
                    pt->simd_pending_unreserved--;
                } else {
                    reg_id_t spilled_reg =
                        pt->simd_slot_use[pt->simd_reg[SIMD_IDX(reg)].slot];
                    ASSERT(spilled_reg != DR_REG_NULL, "invalid spilled reg");
                    uint tmp_slot = find_simd_free_slot(pt);
                    if (tmp_slot == MAX_SIMD_SPILLS) {
                        drreg_report_error(DRREG_ERROR_OUT_OF_SLOTS,
                                           "failed to preserve tool val around app read");
                    }
                    LOG(drcontext, DR_LOG_ALL, 3,
                        "%s @%d." PFX ": restoring %s for app read\n", __FUNCTION__,
                        pt->live_idx, get_where_app_pc(inst), get_register_name(reg));
                    spill_reg_indirectly(drcontext, pt, spilled_reg, tmp_slot, bb, inst);
                    restore_reg_indirectly(drcontext, pt, spilled_reg,
                                           pt->simd_reg[SIMD_IDX(reg)].slot, bb, inst,
                                           false /*keep slot*/);
                    restore_reg_indirectly(drcontext, pt, spilled_reg, tmp_slot, bb, next,
                                           true);
                    /* We keep .native==false */
                    /* Share the tool val spill if this inst writes, too. */
                    restored_for_simd_read[SIMD_IDX(reg)] = true;
                }
            }
        }
    }
#endif
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
        restored_for_read[GPR_IDX(reg)] = false;
        if (!pt->reg[GPR_IDX(reg)].native) {
            if (drmgr_is_last_instr(drcontext, inst) ||
                instr_reads_from_reg(inst, reg, DR_QUERY_INCLUDE_ALL) ||
                /* Treat a partial write as a read, to restore rest of reg */
                (instr_writes_to_reg(inst, reg, DR_QUERY_INCLUDE_ALL) &&
                 !instr_writes_to_exact_reg(inst, reg, DR_QUERY_INCLUDE_ALL)) ||
                /* Treat a conditional write as a read and a write to handle the
                 * condition failing and our write handling saving the wrong value.
                 */
                (instr_writes_to_reg(inst, reg, DR_QUERY_INCLUDE_ALL) &&
                 !instr_writes_to_reg(inst, reg, DR_QUERY_DEFAULT)) ||
                /* i#1954: for complex bbs we must restore before the next app instr. */
                (!pt->reg[GPR_IDX(reg)].in_use &&
                 ((pt->bb_has_internal_flow &&
                   !TEST(DRREG_IGNORE_CONTROL_FLOW, pt->bb_props)) ||
                  TEST(DRREG_CONTAINS_SPANNING_CONTROL_FLOW, pt->bb_props))) ||
                /* If we're out of our own slots and are using a DR slot, we have to
                 * restore now b/c DR slots are not guaranteed across app instrs.
                 */
                pt->reg[GPR_IDX(reg)].slot >= (int)ops.num_spill_slots) {
                if (!pt->reg[GPR_IDX(reg)].in_use) {
                    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": lazily restoring %s\n",
                        __FUNCTION__, pt->live_idx, get_where_app_pc(inst),
                        get_register_name(reg));
                    res = drreg_restore_reg_now(drcontext, bb, inst, pt, reg);
                    if (res != DRREG_SUCCESS)
                        drreg_report_error(res, "lazy restore failed");
                    ASSERT(pt->pending_unreserved > 0, "should not go negative");
                    pt->pending_unreserved--;
                } else if (pt->aflags.xchg == reg) {
                    /* Bail on keeping the flags in the reg. */
                    drreg_move_aflags_from_reg(drcontext, bb, inst, pt, true);
                } else {
                    /* We need to move the tool's value somewhere else.
                     * We use a separate slot for that (and we document that
                     * tools should request an extra slot for each cross-app-instr
                     * register).
                     * XXX: optimize via xchg w/ a dead reg.
                     */
                    uint tmp_slot = find_free_slot(pt);
                    if (tmp_slot == MAX_SPILLS) {
                        drreg_report_error(DRREG_ERROR_OUT_OF_SLOTS,
                                           "failed to preserve tool val around app read");
                    }
                    /* The approach:
                     *   + spill reg (tool val) to new slot
                     *   + restore to reg (app val) from app slot
                     *   + <app instr>
                     *   + restore to reg (tool val) from new slot
                     * XXX: if we change this, we need to update
                     * drreg_event_restore_state().
                     */
                    LOG(drcontext, DR_LOG_ALL, 3,
                        "%s @%d." PFX ": restoring %s for app read\n", __FUNCTION__,
                        pt->live_idx, get_where_app_pc(inst), get_register_name(reg));
                    spill_reg_directly(drcontext, pt, reg, tmp_slot, bb, inst);
                    restore_reg_directly(drcontext, pt, reg, pt->reg[GPR_IDX(reg)].slot,
                                         bb, inst, false /*keep slot*/);
                    restore_reg_directly(drcontext, pt, reg, tmp_slot, bb, next, true);
                    /* Share the tool val spill if this instruction writes, too. */
                    restored_for_read[GPR_IDX(reg)] = true;
                    /* We keep .native==false */
                }
            }
        }
    }

    /* After aflags write by app, update spilled app value */
    if (TESTANY(EFLAGS_WRITE_ARITH, instr_get_eflags(inst, DR_QUERY_INCLUDE_ALL)) &&
        /* Is everything written later? */
        (pt->live_idx == 0 ||
         (ptr_uint_t)drvector_get_entry(&pt->aflags.live, pt->live_idx - 1) != 0)) {
        if (pt->aflags.in_use) {
            LOG(drcontext, DR_LOG_ALL, 3,
                "%s @%d." PFX ": re-spilling aflags after app write\n", __FUNCTION__,
                pt->live_idx, get_where_app_pc(inst));
            res = drreg_spill_aflags(drcontext, bb, next /*after*/, pt);
            if (res != DRREG_SUCCESS) {
                drreg_report_error(res, "failed to spill aflags after app write");
            }
            pt->aflags.native = false;
        } else if (!pt->aflags.native ||
                   pt->slot_use[AFLAGS_SLOT] !=
                       DR_REG_NULL IF_X86(
                           ||
                           (pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use &&
                            pt->aflags.xchg == DR_REG_XAX))) {
            /* give up slot */
            LOG(drcontext, DR_LOG_ALL, 3,
                "%s @%d." PFX ": giving up aflags slot after app write\n", __FUNCTION__,
                pt->live_idx, get_where_app_pc(inst));
#ifdef X86
            if (pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use &&
                pt->aflags.xchg == DR_REG_XAX)
                drreg_move_aflags_from_reg(drcontext, bb, inst, pt, true);
#endif
            pt->slot_use[AFLAGS_SLOT] = DR_REG_NULL;
            pt->aflags.native = true;
        }
    }

    /* After each app write, update spilled app values: */
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++) {
        if (pt->simd_reg[SIMD_IDX(reg)].in_use) {
            void *state =
                drvector_get_entry(&pt->simd_reg[SIMD_IDX(reg)].live, pt->live_idx - 1);
            reg_id_t spilled_reg = pt->simd_slot_use[pt->simd_reg[SIMD_IDX(reg)].slot];
            ASSERT(spilled_reg != DR_REG_NULL, "invalid spilled reg");

            if (instr_writes_to_reg(inst, reg, DR_QUERY_INCLUDE_ALL) &&
                /* Don't bother if reg is dead beyond this write */
                (ops.conservative || pt->live_idx == 0 ||
                 !((reg_is_strictly_xmm(spilled_reg) && state >= SIMD_XMM_DEAD &&
                    state <= SIMD_ZMM_DEAD) ||
                   (reg_is_strictly_ymm(spilled_reg) && state >= SIMD_YMM_DEAD &&
                    state <= SIMD_ZMM_DEAD) ||
                   (reg_is_strictly_zmm(spilled_reg) && state == SIMD_ZMM_DEAD)))) {
                ASSERT(ops.num_spill_simd_slots > 0,
                       "requested SIMD slots cannot be zero");
                uint tmp_slot = MAX_SIMD_SPILLS;
                if (!restored_for_simd_read[SIMD_IDX(reg)]) {
                    tmp_slot = find_simd_free_slot(pt);
                    if (tmp_slot == MAX_SIMD_SPILLS) {
                        drreg_report_error(DRREG_ERROR_OUT_OF_SLOTS,
                                           "failed to preserve tool val wrt app write");
                    }
                    spill_reg_indirectly(drcontext, pt, spilled_reg, tmp_slot, bb, inst);
                }

                instr_t *where =
                    restored_for_simd_read[SIMD_IDX(reg)] ? instr_get_prev(next) : next;
                spill_reg_indirectly(drcontext, pt, spilled_reg,
                                     pt->simd_reg[SIMD_IDX(reg)].slot, bb, where);
                pt->simd_reg[SIMD_IDX(reg)].ever_spilled = true;
                if (!restored_for_simd_read[SIMD_IDX(reg)]) {
                    restore_reg_indirectly(drcontext, pt, spilled_reg, tmp_slot, bb,
                                           next /*after*/, true);
                }
            }
        } else if (!pt->simd_reg[SIMD_IDX(reg)].native &&
                   instr_writes_to_reg(inst, reg, DR_QUERY_INCLUDE_ALL)) {
            /* For an unreserved reg that's written, just drop the slot, even
             * if it was spilled at an earlier reservation point.
             */
            if (pt->simd_reg[SIMD_IDX(reg)].ever_spilled)
                pt->simd_reg[SIMD_IDX(reg)].ever_spilled = false; /* no need to restore */
            res = drreg_restore_reg_now(drcontext, bb, inst, pt, reg);
            if (res != DRREG_SUCCESS)
                drreg_report_error(res, "slot release on app write failed");
            pt->simd_pending_unreserved--;
        }
    }
#endif
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
        if (pt->reg[GPR_IDX(reg)].in_use) {
            if (instr_writes_to_reg(inst, reg, DR_QUERY_INCLUDE_ALL) &&
                /* Don't bother if reg is dead beyond this write */
                (ops.conservative || pt->live_idx == 0 ||
                 drvector_get_entry(&pt->reg[GPR_IDX(reg)].live, pt->live_idx - 1) ==
                     REG_LIVE ||
                 pt->aflags.xchg == reg)) {
                uint tmp_slot = MAX_SPILLS;
                if (pt->aflags.xchg == reg) {
                    /* Bail on keeping the flags in the reg. */
                    drreg_move_aflags_from_reg(drcontext, bb, inst, pt, true);
                    continue;
                }
                if (pt->reg[GPR_IDX(reg)].xchg != DR_REG_NULL) {
                    /* XXX i#511: NYI */
                    drreg_report_error(DRREG_ERROR_FEATURE_NOT_AVAILABLE, "xchg NYI");
                }
                /* Approach (we share 1st and last w/ read, if reads and writes):
                 *   + spill reg (tool val) to new slot
                 *   + <app instr>
                 *   + spill reg (app val) to app slot
                 *   + restore to reg from new slot (tool val)
                 * XXX: if we change this, we need to update
                 * drreg_event_restore_state().
                 */
                LOG(drcontext, DR_LOG_ALL, 3,
                    "%s @%d." PFX ": re-spilling %s after app write\n", __FUNCTION__,
                    pt->live_idx, get_where_app_pc(inst), get_register_name(reg));
                if (!restored_for_read[GPR_IDX(reg)]) {
                    tmp_slot = find_free_slot(pt);
                    if (tmp_slot == MAX_SPILLS) {
                        drreg_report_error(DRREG_ERROR_OUT_OF_SLOTS,
                                           "failed to preserve tool val wrt app write");
                    }
                    spill_reg_directly(drcontext, pt, reg, tmp_slot, bb, inst);
                }
                spill_reg_directly(drcontext, pt, reg, pt->reg[GPR_IDX(reg)].slot, bb,
                                   /* If reads and writes, make sure tool-restore and
                                    * app-spill are in the proper order.
                                    */
                                   restored_for_read[GPR_IDX(reg)] ? instr_get_prev(next)
                                                                   : next /*after*/);
                pt->reg[GPR_IDX(reg)].ever_spilled = true;
                if (!restored_for_read[GPR_IDX(reg)]) {
                    restore_reg_directly(drcontext, pt, reg, tmp_slot, bb, next /*after*/,
                                         true);
                }
            }
        } else if (!pt->reg[GPR_IDX(reg)].native &&
                   instr_writes_to_reg(inst, reg, DR_QUERY_INCLUDE_ALL)) {
            /* For an unreserved reg that's written, just drop the slot, even
             * if it was spilled at an earlier reservation point.
             */
            LOG(drcontext, DR_LOG_ALL, 3,
                "%s @%d." PFX ": dropping slot for unreserved reg %s after app write\n",
                __FUNCTION__, pt->live_idx, get_where_app_pc(inst),
                get_register_name(reg));
            if (pt->reg[GPR_IDX(reg)].ever_spilled)
                pt->reg[GPR_IDX(reg)].ever_spilled = false; /* no need to restore */
            res = drreg_restore_reg_now(drcontext, bb, inst, pt, reg);
            if (res != DRREG_SUCCESS)
                drreg_report_error(res, "slot release on app write failed");
            pt->pending_unreserved--;
        }
    }

    if (drmgr_is_last_instr(drcontext, inst))
        pt->bb_props = 0;

#ifdef DEBUG
    if (drmgr_is_last_instr(drcontext, inst)) {
        uint i;
        for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
            ASSERT(!pt->aflags.in_use, "user failed to unreserve aflags");
            ASSERT(pt->aflags.native, "user failed to unreserve aflags");
            ASSERT(!pt->reg[GPR_IDX(reg)].in_use, "user failed to unreserve a register");
            ASSERT(pt->reg[GPR_IDX(reg)].native, "user failed to unreserve a register");
        }
        for (i = 0; i < MAX_SPILLS; i++) {
            ASSERT(pt->slot_use[i] == DR_REG_NULL, "user failed to unreserve a register");
        }
        for (i = 0; i < MAX_SIMD_SPILLS; i++) {
            ASSERT(pt->simd_slot_use[i] == DR_REG_NULL,
                   "user failed to unreserve a register");
        }
    }
#endif
    instrlist_set_auto_predicate(bb, pred);
    return DR_EMIT_DEFAULT;
}

/***************************************************************************
 * USE OUTSIDE INSERT PHASE
 */

/* For use outside drmgr's insert phase where we don't know the bounds of the
 * app instrs, we fall back to a more expensive liveness analysis on each
 * insertion.
 *
 * XXX: we'd want to add a new API for instru2instru that takes in
 * both the save and restore points at once to allow keeping aflags in
 * eax and other optimizations.
 */
static drreg_status_t
drreg_forward_analysis(void *drcontext, instr_t *start)
{
    per_thread_t *pt = get_tls_data(drcontext);
    instr_t *inst;
    ptr_uint_t aflags_new, aflags_cur = 0;
    reg_id_t reg;

    /* We just use index 0 of the live vectors */
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
        pt->reg[GPR_IDX(reg)].app_uses = 0;
        drvector_set_entry(&pt->reg[GPR_IDX(reg)].live, 0, REG_UNKNOWN);
    }
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++) {
        pt->simd_reg[SIMD_IDX(reg)].app_uses = 0;
        drvector_set_entry(&pt->simd_reg[SIMD_IDX(reg)].live, 0, SIMD_UNKNOWN);
        /* TODO i#3827: investigate and confirm that this is correct. */
        pt->simd_reg[SIMD_IDX(reg)].ever_spilled = false;
    }
#endif
    /* We have to consider meta instrs as well */
    for (inst = start; inst != NULL; inst = instr_get_next(inst)) {
        if (instr_is_cti(inst) || instr_is_interrupt(inst) || instr_is_syscall(inst))
            break;

        /* GPR liveness */
        for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
            void *value = REG_UNKNOWN;
            if (drvector_get_entry(&pt->reg[GPR_IDX(reg)].live, 0) != REG_UNKNOWN)
                continue;
            /* DRi#1849: COND_SRCS here includes addressing regs in dsts */
            if (instr_reads_from_reg(inst, reg, DR_QUERY_INCLUDE_COND_SRCS))
                value = REG_LIVE;
            /* make sure we don't consider writes to sub-regs */
            else if (instr_writes_to_exact_reg(inst, reg, DR_QUERY_INCLUDE_COND_SRCS)
                     /* a write to a 32-bit reg for amd64 zeroes the top 32 bits */
                     IF_X86_64(||
                               instr_writes_to_exact_reg(inst, reg_64_to_32(reg),
                                                         DR_QUERY_INCLUDE_COND_SRCS)))
                value = REG_DEAD;
            if (value != REG_UNKNOWN)
                drvector_set_entry(&pt->reg[GPR_IDX(reg)].live, 0, value);
        }
#ifdef SIMD_SUPPORTED
        /* SIMD liveness */
        for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD;
             reg++) {
            void *value = SIMD_UNKNOWN;
            if (drvector_get_entry(&pt->simd_reg[SIMD_IDX(reg)].live, 0) != SIMD_UNKNOWN)
                continue;

            determine_simd_liveness_state(drcontext, inst, reg, &value);
            if (value != SIMD_UNKNOWN)
                drvector_set_entry(&pt->simd_reg[SIMD_IDX(reg)].live, 0, value);
        }
#endif
        /* aflags liveness */
        aflags_new = instr_get_arith_flags(inst, DR_QUERY_INCLUDE_COND_SRCS);
        /* reading and writing counts only as reading */
        aflags_new &= (~(EFLAGS_READ_TO_WRITE(aflags_new)));
        /* reading doesn't count if already written */
        aflags_new &= (~(EFLAGS_WRITE_TO_READ(aflags_cur)));
        aflags_cur |= aflags_new;

        if (instr_is_app(inst)) {
            int i;
            for (i = 0; i < instr_num_dsts(inst); i++)
                count_app_uses(pt, instr_get_dst(inst, i));
            for (i = 0; i < instr_num_srcs(inst); i++)
                count_app_uses(pt, instr_get_src(inst, i));
        }
    }

    pt->live_idx = 0;

    /* If we could not determine state (i.e. unknown), we set the state to live. */
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
        if (drvector_get_entry(&pt->reg[GPR_IDX(reg)].live, 0) == REG_UNKNOWN)
            drvector_set_entry(&pt->reg[GPR_IDX(reg)].live, 0, REG_LIVE);
    }
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++) {
        if (drvector_get_entry(&pt->simd_reg[SIMD_IDX(reg)].live, 0) == SIMD_UNKNOWN)
            drvector_set_entry(&pt->simd_reg[SIMD_IDX(reg)].live, 0, SIMD_ZMM_LIVE);
    }
#endif
    drvector_set_entry(&pt->aflags.live, 0,
                       (void *)(ptr_uint_t)
                       /* set read bit if not written */
                       (EFLAGS_READ_ARITH & (~(EFLAGS_WRITE_TO_READ(aflags_cur)))));
    return DRREG_SUCCESS;
}

/***************************************************************************
 * REGISTER RESERVATION
 */

drreg_status_t
drreg_init_and_fill_vector_ex(drvector_t *vec, drreg_spill_class_t spill_class,
                              bool allowed)
{
    reg_id_t reg;
    uint size;
    if (vec == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;

    if (spill_class == DRREG_GPR_SPILL_CLASS) {
        size = DR_NUM_GPR_REGS;
    } else if (spill_class == DRREG_SIMD_XMM_SPILL_CLASS) {
#ifdef X86
        size = DR_NUM_SIMD_VECTOR_REGS;
#else
        return DRREG_ERROR_INVALID_PARAMETER;
#endif
    } else if (spill_class == DRREG_SIMD_YMM_SPILL_CLASS ||
               spill_class == DRREG_SIMD_ZMM_SPILL_CLASS) {
#ifdef X86
        /* TODO i#3844: support on x86. */
        return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
#else
        return DRREG_ERROR_INVALID_PARAMETER;
#endif
    } else {
        return DRREG_ERROR;
    }

    drvector_init(vec, size, false /*!synch*/, NULL);
    for (reg = 0; reg < size; reg++)
        drvector_set_entry(vec, reg, allowed ? (void *)(ptr_uint_t)1 : NULL);
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_init_and_fill_vector(drvector_t *vec, bool allowed)
{
    return drreg_init_and_fill_vector_ex(vec, DRREG_GPR_SPILL_CLASS, allowed);
}

drreg_status_t
drreg_set_vector_entry(drvector_t *vec, reg_id_t reg, bool allowed)
{
    reg_id_t start_reg;

    if (reg_is_gpr(reg)) {
        if (vec == NULL || reg < DR_REG_START_GPR || reg > DR_REG_STOP_GPR)
            return DRREG_ERROR_INVALID_PARAMETER;
        start_reg = DR_REG_START_GPR;
    }
#ifdef SIMD_SUPPORTED
    else if (reg_is_vector_simd(reg)) {
        /* We assume the SIMD range is contiguous and no further out of range checks
         * are performed as it is done above for gprs.
         */
        if (vec == NULL)
            return DRREG_ERROR_INVALID_PARAMETER;
        start_reg = DR_REG_APPLICABLE_START_SIMD;
        reg = reg_resize_to_opsz(reg, OPSZ_64);
    }
#endif
    else {
        return DRREG_ERROR;
    }

    drvector_set_entry(vec, reg - start_reg, allowed ? (void *)(ptr_uint_t)1 : NULL);
    return DRREG_SUCCESS;
}

/* Assumes liveness info is already set up in per_thread_t. Liveness should have either
 * been computed by a forward liveness scan upon every insertion if called outside of
 * insertion phase, see drreg_forward_analysis(). Or if called inside insertion
 * phase, at the end of drmgr's analysis phase once, see drreg_event_bb_analysis().
 * Please note that drreg is not yet able to properly handle multiple users if they use
 * drreg from in and outside of the insertion phase, xref i#3823.
 */
static drreg_status_t
drreg_reserve_gpr_internal(void *drcontext, instrlist_t *ilist, instr_t *where,
                           drvector_t *reg_allowed, bool only_if_no_spill,
                           OUT reg_id_t *reg_out)
{
    per_thread_t *pt = get_tls_data(drcontext);
    uint slot = MAX_SPILLS;
    uint min_uses = UINT_MAX;
    reg_id_t reg = DR_REG_STOP_GPR + 1, best_reg = DR_REG_NULL;
    bool already_spilled = false;
    if (reg_out == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;

    /* First, try to use a previously unreserved but not yet lazily restored reg.
     * This must be first to avoid accumulating slots beyond the requested max.
     * Because we drop an unreserved reg when the app writes to it, we should
     * never pick an unreserved and unspilled yet not currently dead reg over
     * some other dead reg.
     */
    if (pt->pending_unreserved > 0) {
        for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
            uint idx = GPR_IDX(reg);
            if (!pt->reg[idx].native && !pt->reg[idx].in_use &&
                (reg_allowed == NULL || drvector_get_entry(reg_allowed, idx) != NULL) &&
                (!only_if_no_spill || pt->reg[idx].ever_spilled ||
                 drvector_get_entry(&pt->reg[idx].live, pt->live_idx) == REG_DEAD)) {
                slot = pt->reg[idx].slot;
                pt->pending_unreserved--;
                already_spilled = pt->reg[idx].ever_spilled;
                LOG(drcontext, DR_LOG_ALL, 3,
                    "%s @%d." PFX ": using un-restored %s slot %d\n", __FUNCTION__,
                    pt->live_idx, get_where_app_pc(where), get_register_name(reg), slot);
                break;
            }
        }
    }

    if (reg > DR_REG_STOP_GPR) {
        /* Look for a dead register, or the least-used register */
        for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
            uint idx = GPR_IDX(reg);
            if (pt->reg[idx].in_use)
                continue;
            if (reg ==
                dr_get_stolen_reg() IF_ARM(|| reg == DR_REG_PC)
                /* Avoid xsp, even if it appears dead in things like OP_sysenter.
                 * On AArch64 use of SP is very restricted.
                 */
                IF_NOT_ARM(|| reg == DR_REG_XSP))
                continue;
            if (reg_allowed != NULL && drvector_get_entry(reg_allowed, idx) == NULL)
                continue;
            /* If we had a hint as to local vs whole-bb we could downgrade being
             * dead right now as a priority
             */
            if (drvector_get_entry(&pt->reg[idx].live, pt->live_idx) == REG_DEAD)
                break;
            if (only_if_no_spill)
                continue;
            if (pt->reg[idx].app_uses < min_uses) {
                best_reg = reg;
                min_uses = pt->reg[idx].app_uses;
            }
        }
    }
    if (reg > DR_REG_STOP_GPR) {
        if (best_reg != DR_REG_NULL)
            reg = best_reg;
        else {
#ifdef X86
            /* If aflags was unreserved but is still in xax, give it up rather than
             * fail to reserve a new register.
             */
            if (!pt->aflags.in_use && pt->reg[GPR_IDX(DR_REG_XAX)].in_use &&
                pt->aflags.xchg == DR_REG_XAX &&
                (reg_allowed == NULL ||
                 drvector_get_entry(reg_allowed, GPR_IDX(DR_REG_XAX)) != NULL)) {
                LOG(drcontext, DR_LOG_ALL, 3,
                    "%s @%d." PFX ": taking xax from unreserved aflags\n", __FUNCTION__,
                    pt->live_idx, get_where_app_pc(where));
                drreg_move_aflags_from_reg(drcontext, ilist, where, pt, true);
                reg = DR_REG_XAX;
            } else
#endif
                return DRREG_ERROR_REG_CONFLICT;
        }
    }
    if (slot == MAX_SPILLS) {
        slot = find_free_slot(pt);
        if (slot == MAX_SPILLS)
            return DRREG_ERROR_OUT_OF_SLOTS;
    }

    ASSERT(!pt->reg[GPR_IDX(reg)].in_use, "overlapping uses");
    pt->reg[GPR_IDX(reg)].in_use = true;
    if (!already_spilled) {
        /* Even if dead now, we need to own a slot in case reserved past dead point */
        if (ops.conservative ||
            drvector_get_entry(&pt->reg[GPR_IDX(reg)].live, pt->live_idx) == REG_LIVE) {
            LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": spilling %s to slot %d\n",
                __FUNCTION__, pt->live_idx, get_where_app_pc(where),
                get_register_name(reg), slot);
            spill_reg_directly(drcontext, pt, reg, slot, ilist, where);
            pt->reg[GPR_IDX(reg)].ever_spilled = true;
        } else {
            LOG(drcontext, DR_LOG_ALL, 3,
                "%s @%d." PFX ": no need to spill %s to slot %d\n", __FUNCTION__,
                pt->live_idx, get_where_app_pc(where), get_register_name(reg), slot);
            pt->slot_use[slot] = reg;
            pt->reg[GPR_IDX(reg)].ever_spilled = false;
        }
    } else {
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": %s already spilled to slot %d\n",
            __FUNCTION__, pt->live_idx, get_where_app_pc(where), get_register_name(reg),
            slot);
    }
    pt->reg[GPR_IDX(reg)].native = false;
    pt->reg[GPR_IDX(reg)].xchg = DR_REG_NULL;
    pt->reg[GPR_IDX(reg)].slot = slot;
    *reg_out = reg;
    return DRREG_SUCCESS;
}

#ifdef SIMD_SUPPORTED
static void *
get_simd_dead_state(const drreg_spill_class_t spill_class)
{
    if (spill_class == DRREG_SIMD_XMM_SPILL_CLASS) {
        return SIMD_XMM_DEAD;
    } else if (spill_class == DRREG_SIMD_YMM_SPILL_CLASS) {
        return SIMD_YMM_DEAD;
    } else if (spill_class == DRREG_SIMD_ZMM_SPILL_CLASS) {
        return SIMD_ZMM_DEAD;
    }
    ASSERT(false, "cannot determine dead state");
    return SIMD_UNKNOWN;
}
#endif

#ifdef SIMD_SUPPORTED
static drreg_spill_class_t
get_spill_class(const reg_id_t reg)
{

    if (reg_is_gpr(reg)) {
        return DRREG_GPR_SPILL_CLASS;
    } else if (reg_is_strictly_xmm(reg)) {
        return DRREG_SIMD_XMM_SPILL_CLASS;
    } else if (reg_is_strictly_ymm(reg)) {
        return DRREG_SIMD_YMM_SPILL_CLASS;
    } else if (reg_is_strictly_zmm(reg)) {
        return DRREG_SIMD_ZMM_SPILL_CLASS;
    } else {
        ASSERT(false, "unsupported or invalid spill class");
    }
    return DRREG_INVALID_SPILL_CLASS;
}
#endif

#ifdef SIMD_SUPPORTED
/* Makes the same assumptions about liveness info being already computed as
 * drreg_reserve_gpr_internal().
 */
static drreg_status_t
drreg_find_for_simd_reservation(void *drcontext, const drreg_spill_class_t spill_class,
                                instrlist_t *ilist, instr_t *where,
                                drvector_t *reg_allowed, bool only_if_no_spill,
                                OUT uint *slot_out, OUT reg_id_t *reg_out,
                                OUT bool *already_spilled_out)
{
    per_thread_t *pt = get_tls_data(drcontext);
    uint min_uses = UINT_MAX;
    uint slot = MAX_SIMD_SPILLS;
    reg_id_t best_reg = DR_REG_NULL;
    bool already_spilled = false;
    if (ops.num_spill_simd_slots == 0)
        return DRREG_ERROR;
    reg_id_t reg = (reg_id_t)DR_REG_APPLICABLE_STOP_SIMD + 1;
    void *dead_state = get_simd_dead_state(spill_class);
    if (dead_state == SIMD_UNKNOWN)
        return DRREG_ERROR;
    if (pt->simd_pending_unreserved > 0) {
        for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD;
             reg++) {
            uint idx = SIMD_IDX(reg);
            if (!pt->simd_reg[idx].native && !pt->simd_reg[idx].in_use &&
                (reg_allowed == NULL || drvector_get_entry(reg_allowed, idx) != NULL) &&
                (!only_if_no_spill || pt->simd_reg[idx].ever_spilled ||
                 (drvector_get_entry(&pt->simd_reg[idx].live, pt->live_idx) >=
                      dead_state &&
                  drvector_get_entry(&pt->simd_reg[idx].live, pt->live_idx) <=
                      SIMD_ZMM_DEAD))) {
                slot = pt->simd_reg[idx].slot;
                pt->simd_pending_unreserved--;
                reg_id_t spilled_reg = pt->simd_slot_use[slot];
                already_spilled = pt->simd_reg[idx].ever_spilled &&
                    get_spill_class(spilled_reg) == spill_class;
                break;
            }
        }
    }
    if (reg > DR_REG_APPLICABLE_STOP_SIMD) {
        /* Look for a dead register, or the least-used register */
        for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD;
             ++reg) {
            uint idx = SIMD_IDX(reg);
            if (pt->simd_reg[idx].in_use)
                continue;
            if (reg_allowed != NULL && drvector_get_entry(reg_allowed, idx) == NULL)
                continue;
            if (drvector_get_entry(&pt->simd_reg[idx].live, pt->live_idx) >= dead_state &&
                drvector_get_entry(&pt->simd_reg[idx].live, pt->live_idx) <=
                    SIMD_ZMM_DEAD)
                break;
            if (only_if_no_spill)
                continue;
            if (pt->simd_reg[idx].app_uses < min_uses) {
                best_reg = reg;
                min_uses = pt->simd_reg[idx].app_uses;
            }
        }
    }
    if (reg > DR_REG_APPLICABLE_STOP_SIMD) {
        if (best_reg != DR_REG_NULL)
            reg = best_reg;
        else
            return DRREG_ERROR_REG_CONFLICT;
    }
    if (slot == MAX_SIMD_SPILLS) {
        slot = find_simd_free_slot(pt);
        if (slot == MAX_SIMD_SPILLS)
            return DRREG_ERROR_OUT_OF_SLOTS;
    }
    if (spill_class == DRREG_SIMD_XMM_SPILL_CLASS) {
        reg = reg_resize_to_opsz(reg, OPSZ_16);
    } else if (spill_class == DRREG_SIMD_YMM_SPILL_CLASS) {
        reg = reg_resize_to_opsz(reg, OPSZ_32);
    } else if (spill_class == DRREG_SIMD_ZMM_SPILL_CLASS) {
        reg = reg_resize_to_opsz(reg, OPSZ_64);
    } else {
        return DRREG_ERROR;
    }
    *slot_out = slot;
    *reg_out = reg;
    *already_spilled_out = already_spilled;
    return DRREG_SUCCESS;
}
#endif

#ifdef SIMD_SUPPORTED
/* Makes the same assumptions about liveness info being already computed as
 * drreg_reserve_gpr_internal().
 */
static drreg_status_t
drreg_reserve_simd_reg_internal(void *drcontext, drreg_spill_class_t spill_class,
                                instrlist_t *ilist, instr_t *where,
                                drvector_t *reg_allowed, bool only_if_no_spill,
                                OUT reg_id_t *reg_out)
{
    per_thread_t *pt = get_tls_data(drcontext);
    uint slot = 0;
    reg_id_t reg = DR_REG_NULL;
    bool already_spilled = false;
    drreg_status_t res;
    res =
        drreg_find_for_simd_reservation(drcontext, spill_class, ilist, where, reg_allowed,
                                        only_if_no_spill, &slot, &reg, &already_spilled);
    if (res != DRREG_SUCCESS)
        return res;

    /* We found a suitable reg, now we need to spill. */
    ASSERT(!pt->simd_reg[SIMD_IDX(reg)].in_use, "overlapping uses");
    pt->simd_reg[SIMD_IDX(reg)].in_use = true;
    if (!already_spilled) {
        /* Even if dead now, we need to own a slot in case reserved past dead point */
        if (ops.conservative ||
            (drvector_get_entry(&pt->simd_reg[SIMD_IDX(reg)].live, pt->live_idx) >=
                 SIMD_XMM_LIVE &&
             drvector_get_entry(&pt->simd_reg[SIMD_IDX(reg)].live, pt->live_idx) <=
                 SIMD_ZMM_LIVE)) {
            LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": spilling %s to slot %d\n",
                __FUNCTION__, pt->live_idx, get_where_app_pc(where),
                get_register_name(reg), slot);
            spill_reg_indirectly(drcontext, pt, reg, slot, ilist, where);
            pt->simd_reg[SIMD_IDX(reg)].ever_spilled = true;
        } else {
            LOG(drcontext, DR_LOG_ALL, 3,
                "%s @%d." PFX ": no need to spill %s to slot %d\n", __FUNCTION__,
                pt->live_idx, get_where_app_pc(where), get_register_name(reg), slot);
            pt->simd_slot_use[slot] = reg;
            pt->simd_reg[SIMD_IDX(reg)].ever_spilled = false;
        }
    } else {
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": %s already spilled to slot %d\n",
            __FUNCTION__, pt->live_idx, get_where_app_pc(where), get_register_name(reg),
            slot);
    }
    pt->simd_reg[SIMD_IDX(reg)].native = false;
    pt->simd_reg[SIMD_IDX(reg)].xchg = DR_REG_NULL;
    pt->simd_reg[SIMD_IDX(reg)].slot = slot;
    *reg_out = reg;
    return DRREG_SUCCESS;
}
#endif

/* Assumes liveness info is already set up in per_thread_t. */
static drreg_status_t
drreg_reserve_reg_internal(void *drcontext, drreg_spill_class_t spill_class,
                           instrlist_t *ilist, instr_t *where, drvector_t *reg_allowed,
                           bool only_if_no_spill, OUT reg_id_t *reg_out)
{
    if (reg_out == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;

    if (spill_class == DRREG_GPR_SPILL_CLASS) {
        return drreg_reserve_gpr_internal(drcontext, ilist, where, reg_allowed,
                                          only_if_no_spill, reg_out);
    }
#ifdef SIMD_SUPPORTED
    else if (spill_class == DRREG_SIMD_XMM_SPILL_CLASS ||
             spill_class == DRREG_SIMD_YMM_SPILL_CLASS ||
             spill_class == DRREG_SIMD_ZMM_SPILL_CLASS) {
        return drreg_reserve_simd_reg_internal(drcontext, spill_class, ilist, where,
                                               reg_allowed, only_if_no_spill, reg_out);
    }
#else
    /* FIXME i#3844: NYI on ARM */
#endif
    else {
        /* The caller should have catched this and return an error or invalid parameter
         * error.
         */
        ASSERT(false, "internal error: invalid spill class");
    }
    return DRREG_ERROR;
}

drreg_status_t
drreg_reserve_register_ex(void *drcontext, drreg_spill_class_t spill_class,
                          instrlist_t *ilist, instr_t *where, drvector_t *reg_allowed,
                          OUT reg_id_t *reg_out)
{
    dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
    drreg_status_t res;
#ifdef ARM
    if (spill_class == DRREG_SIMD_XMM_SPILL_CLASS)
        return DRREG_ERROR_INVALID_PARAMETER;
#endif
    if (spill_class == DRREG_SIMD_YMM_SPILL_CLASS ||
        spill_class == DRREG_SIMD_ZMM_SPILL_CLASS) {
#ifdef X86
        /* TODO i#3844: support on x86. */
        return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
#else
        return DRREG_ERROR_INVALID_PARAMETER;
#endif
    }
    if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
        res = drreg_forward_analysis(drcontext, where);
        if (res != DRREG_SUCCESS)
            return res;
    }
    /* FIXME i#3827: ever_spilled is not being reset. */
    /* XXX i#2585: drreg should predicate spills and restores as appropriate */
    instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
    res = drreg_reserve_reg_internal(drcontext, spill_class, ilist, where, reg_allowed,
                                     false, reg_out);
    instrlist_set_auto_predicate(ilist, pred);
    return res;
}

drreg_status_t
drreg_reserve_register(void *drcontext, instrlist_t *ilist, instr_t *where,
                       drvector_t *reg_allowed, OUT reg_id_t *reg_out)
{
    return drreg_reserve_register_ex(drcontext, DRREG_GPR_SPILL_CLASS, ilist, where,
                                     reg_allowed, reg_out);
}

drreg_status_t
drreg_reserve_dead_register(void *drcontext, instrlist_t *ilist, instr_t *where,
                            drvector_t *reg_allowed, OUT reg_id_t *reg_out)
{
    return drreg_reserve_dead_register_ex(drcontext, DRREG_GPR_SPILL_CLASS, ilist, where,
                                          reg_allowed, reg_out);
}

drreg_status_t
drreg_reserve_dead_register_ex(void *drcontext, drreg_spill_class_t spill_class,
                               instrlist_t *ilist, instr_t *where,
                               drvector_t *reg_allowed, OUT reg_id_t *reg_out)
{
    dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
    drreg_status_t res;
#ifdef ARM
    if (spill_class == DRREG_SIMD_XMM_SPILL_CLASS)
        return DRREG_ERROR_INVALID_PARAMETER;
#endif
    if (spill_class == DRREG_SIMD_YMM_SPILL_CLASS ||
        spill_class == DRREG_SIMD_ZMM_SPILL_CLASS) {
#ifdef X86
        /* TODO i#3844: support on x86. */
        return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
#else
        return DRREG_ERROR_INVALID_PARAMETER;
#endif
    }
    if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
        res = drreg_forward_analysis(drcontext, where);
        if (res != DRREG_SUCCESS)
            return res;
    }
    /* XXX i#2585: drreg should predicate spills and restores as appropriate */
    instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
    res = drreg_reserve_reg_internal(drcontext, spill_class, ilist, where, reg_allowed,
                                     true, reg_out);
    instrlist_set_auto_predicate(ilist, pred);
    return res;
}

drreg_status_t
drreg_restore_app_value(void *drcontext, instrlist_t *ilist, instr_t *where,
                        reg_id_t app_reg, reg_id_t dst_reg, bool stateful)
{
    per_thread_t *pt = get_tls_data(drcontext);
    dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);

    if (reg_is_gpr(app_reg) &&
        (!reg_is_pointer_sized(app_reg) || !reg_is_pointer_sized(dst_reg)))
        return DRREG_ERROR_INVALID_PARAMETER;

    /* XXX i#2585: drreg should predicate spills and restores as appropriate */
    instrlist_set_auto_predicate(ilist, DR_PRED_NONE);

    /* check if app_reg is stolen reg */
    if (app_reg == dr_get_stolen_reg()) {
        /* DR will refuse to load into the same reg (the caller must use
         * opnd_replace_reg() with a scratch reg in that case).
         */
        if (dst_reg == app_reg) {
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_ERROR_INVALID_PARAMETER;
        }
        if (dr_insert_get_stolen_reg_value(drcontext, ilist, where, dst_reg)) {
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_SUCCESS;
        }
        ASSERT(false, "internal error on getting stolen reg app value");
        instrlist_set_auto_predicate(ilist, pred);
        return DRREG_ERROR;
    }
    if (reg_is_gpr(app_reg)) {
        /* Check if app_reg is an unspilled reg. */
        if (pt->reg[GPR_IDX(app_reg)].native) {
            LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": reg %s already native\n",
                __FUNCTION__, pt->live_idx, get_where_app_pc(where),
                get_register_name(app_reg));
            if (dst_reg != app_reg) {
                PRE(ilist, where,
                    XINST_CREATE_move(drcontext, opnd_create_reg(dst_reg),
                                      opnd_create_reg(app_reg)));
            }
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_SUCCESS;
        }
        /* We may have lost the app value for a dead reg. */
        if (!pt->reg[GPR_IDX(app_reg)].ever_spilled) {
            LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": reg %s never spilled\n",
                __FUNCTION__, pt->live_idx, get_where_app_pc(where),
                get_register_name(app_reg));
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_ERROR_NO_APP_VALUE;
        }
        /* Restore the app value back to app_reg. */
        if (pt->reg[GPR_IDX(app_reg)].xchg != DR_REG_NULL) {
            /* XXX i#511: NYI */
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
        }
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": getting app value for %s\n",
            __FUNCTION__, pt->live_idx, get_where_app_pc(where),
            get_register_name(app_reg));
        /* XXX i#511: if we add .xchg support for GPR's we'll need to check them all
         * here.
         */
        if (pt->aflags.xchg == app_reg) {
            /* Bail on keeping the flags in the reg. */
            drreg_move_aflags_from_reg(drcontext, ilist, where, pt, stateful);
        } else {
            restore_reg_directly(drcontext, pt, app_reg, pt->reg[GPR_IDX(app_reg)].slot,
                                 ilist, where,
                                 stateful && !pt->reg[GPR_IDX(app_reg)].in_use);
            if (stateful && !pt->reg[GPR_IDX(app_reg)].in_use)
                pt->reg[GPR_IDX(app_reg)].native = true;
        }
    }
#ifdef SIMD_SUPPORTED
    else if (reg_is_vector_simd(app_reg)) {
        if (!reg_is_vector_simd(dst_reg))
            return DRREG_ERROR_INVALID_PARAMETER;

        /* Check if app_reg is an unspilled reg. */
        if (pt->simd_reg[SIMD_IDX(app_reg)].native) {
            if (dst_reg != app_reg) {
                PRE(ilist, where,
                    INSTR_CREATE_movdqa(drcontext, opnd_create_reg(dst_reg),
                                        opnd_create_reg(app_reg)));
            }
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_SUCCESS;
        }
        /* We may have lost the app value for a dead reg. */
        if (!pt->simd_reg[SIMD_IDX(app_reg)].ever_spilled) {
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_ERROR_NO_APP_VALUE;
        }
        /* Restore the app value back to app_reg. */
        if (pt->simd_reg[SIMD_IDX(app_reg)].xchg != DR_REG_NULL) {
            /* XXX i#511: NYI */
            instrlist_set_auto_predicate(ilist, pred);
            return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
        }
        restore_reg_indirectly(drcontext, pt, app_reg,
                               pt->simd_reg[SIMD_IDX(app_reg)].slot, ilist, where,
                               stateful && !pt->simd_reg[SIMD_IDX(app_reg)].in_use);
        if (stateful && !pt->simd_reg[SIMD_IDX(app_reg)].in_use)
            pt->simd_reg[SIMD_IDX(app_reg)].native = true;
    }
#endif
    instrlist_set_auto_predicate(ilist, pred);
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_get_app_value(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t app_reg,
                    reg_id_t dst_reg)
{
    return drreg_restore_app_value(drcontext, ilist, where, app_reg, dst_reg, true);
}

drreg_status_t
drreg_restore_app_values(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t opnd,
                         INOUT reg_id_t *swap)
{
    drreg_status_t res;
    bool no_app_value = false;
    int num_op = opnd_num_regs_used(opnd);
    dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
    int i;

    /* XXX i#2585: drreg should predicate spills and restores as appropriate */
    instrlist_set_auto_predicate(ilist, DR_PRED_NONE);

    /* First restore SIMD registers. */
    for (i = 0; i < num_op; i++) {
        reg_id_t reg = opnd_get_reg_used(opnd, i);
        reg_id_t dst;
        if (!reg_is_vector_simd(reg))
            continue;

        dst = reg;
        res = drreg_get_app_value(drcontext, ilist, where, reg, dst);
        if (res == DRREG_ERROR_NO_APP_VALUE)
            no_app_value = true;
        else if (res != DRREG_SUCCESS) {
            instrlist_set_auto_predicate(ilist, pred);
            return res;
        }
    }
    /* Now restore GPRs. */
    for (i = 0; i < num_op; i++) {
        reg_id_t reg = opnd_get_reg_used(opnd, i);
        reg_id_t dst;
        if (!reg_is_gpr(reg))
            continue;
        reg = reg_to_pointer_sized(reg);
        dst = reg;
        if (reg == dr_get_stolen_reg()) {
            if (swap == NULL) {
                instrlist_set_auto_predicate(ilist, pred);
                return DRREG_ERROR_INVALID_PARAMETER;
            }
            if (*swap == DR_REG_NULL) {
                res = drreg_reserve_register(drcontext, ilist, where, NULL, &dst);
                if (res != DRREG_SUCCESS) {
                    instrlist_set_auto_predicate(ilist, pred);
                    return res;
                }
            } else
                dst = *swap;
            if (!opnd_replace_reg(&opnd, reg, dst)) {
                instrlist_set_auto_predicate(ilist, pred);
                return DRREG_ERROR;
            }
            *swap = dst;
        }
        res = drreg_get_app_value(drcontext, ilist, where, reg, dst);
        if (res == DRREG_ERROR_NO_APP_VALUE)
            no_app_value = true;
        else if (res != DRREG_SUCCESS) {
            instrlist_set_auto_predicate(ilist, pred);
            return res;
        }
    }
    instrlist_set_auto_predicate(ilist, pred);
    return (no_app_value ? DRREG_ERROR_NO_APP_VALUE : DRREG_SUCCESS);
}

drreg_status_t
drreg_statelessly_restore_app_value(void *drcontext, instrlist_t *ilist, reg_id_t reg,
                                    instr_t *where_restore, instr_t *where_respill,
                                    bool *restore_needed OUT, bool *respill_needed OUT)
{
    per_thread_t *pt = get_tls_data(drcontext);
    drreg_status_t res;
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX " %s\n", __FUNCTION__, pt->live_idx,
        get_where_app_pc(where_restore), get_register_name(reg));
    if (where_restore == NULL || where_respill == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;
    if (reg == DR_REG_NULL) {
        res = drreg_restore_aflags(drcontext, ilist, where_restore, pt, false);
    } else {
        if (reg_is_gpr(reg) && (!reg_is_pointer_sized(reg) || reg == dr_get_stolen_reg()))
            return DRREG_ERROR_INVALID_PARAMETER;
        res = drreg_restore_app_value(drcontext, ilist, where_restore, reg, reg, false);
    }
    if (restore_needed != NULL)
        *restore_needed = (res == DRREG_SUCCESS);
    if (res != DRREG_SUCCESS && res != DRREG_ERROR_NO_APP_VALUE)
        return res;
        /* XXX i#511: if we add .xchg support for GPR's we'll need to check them all here.
         */
#ifdef X86
    if (reg != DR_REG_NULL && pt->aflags.xchg == reg) {
        pt->slot_use[AFLAGS_SLOT] = DR_REG_XAX; /* appease assert */
        restore_reg_directly(drcontext, pt, DR_REG_XAX, AFLAGS_SLOT, ilist, where_respill,
                             false);
        pt->slot_use[AFLAGS_SLOT] = DR_REG_NULL;
        if (respill_needed != NULL)
            *respill_needed = true;
    } else
#endif
        if (respill_needed != NULL)
        *respill_needed = false;
    return res;
}

static drreg_status_t
drreg_restore_reg_now(void *drcontext, instrlist_t *ilist, instr_t *inst,
                      per_thread_t *pt, reg_id_t reg)
{
    if (reg_is_gpr(reg)) {
        if (pt->reg[GPR_IDX(reg)].ever_spilled) {
            if (pt->reg[GPR_IDX(reg)].xchg != DR_REG_NULL) {
                /* XXX i#511: NYI */
                return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
            }
            LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": restoring %s\n", __FUNCTION__,
                pt->live_idx, get_where_app_pc(inst), get_register_name(reg));
            restore_reg_directly(drcontext, pt, reg, pt->reg[GPR_IDX(reg)].slot, ilist,
                                 inst, true);
        } else {
            /* still need to release slot */
            LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": %s never spilled\n",
                __FUNCTION__, pt->live_idx, get_where_app_pc(inst),
                get_register_name(reg));
            pt->slot_use[pt->reg[GPR_IDX(reg)].slot] = DR_REG_NULL;
        }
        pt->reg[GPR_IDX(reg)].native = true;
    }
#ifdef SIMD_SUPPORTED
    else if (reg_is_vector_simd(reg)) {
        if (pt->simd_reg[SIMD_IDX(reg)].ever_spilled) {
            reg_id_t spilled_reg = pt->simd_slot_use[pt->simd_reg[SIMD_IDX(reg)].slot];
            restore_reg_indirectly(drcontext, pt, spilled_reg,
                                   pt->simd_reg[SIMD_IDX(reg)].slot, ilist, inst, true);
        } else {
            pt->simd_slot_use[pt->simd_reg[SIMD_IDX(reg)].slot] = DR_REG_NULL;
        }
        pt->simd_reg[SIMD_IDX(reg)].native = true;
    }
#endif
    else {
        ASSERT(false, "internal error: not an applicable register.");
    }
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_unreserve_register(void *drcontext, instrlist_t *ilist, instr_t *where,
                         reg_id_t reg)
{
    per_thread_t *pt = get_tls_data(drcontext);

    if (reg_is_gpr(reg)) {
        if (!pt->reg[GPR_IDX(reg)].in_use)
            return DRREG_ERROR_INVALID_PARAMETER;
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX " %s\n", __FUNCTION__, pt->live_idx,
            get_where_app_pc(where), get_register_name(reg));
        if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
            /* We have no way to lazily restore. We do not bother at this point
             * to try and eliminate back-to-back spill/restore pairs.
             */
            dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
            drreg_status_t res;

            /* XXX i#2585: drreg should predicate spills and restores as appropriate. */
            instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
            res = drreg_restore_reg_now(drcontext, ilist, where, pt, reg);
            instrlist_set_auto_predicate(ilist, pred);
            if (res != DRREG_SUCCESS)
                return res;
        } else {
            /* We lazily restore in drreg_event_bb_insert_late(), in case
             * someone else wants a local scratch.
             */
            pt->pending_unreserved++;
        }
        pt->reg[GPR_IDX(reg)].in_use = false;
    }
#ifdef SIMD_SUPPORTED
    else if (reg_is_vector_simd(reg)) {
        if (!pt->simd_reg[SIMD_IDX(reg)].in_use)
            return DRREG_ERROR_INVALID_PARAMETER;

        if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
            /* We have no way to lazily restore.  We do not bother at this point
             * to try and eliminate back-to-back spill/restore pairs.
             */
            dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
            drreg_status_t res;
            /* XXX i#2585: drreg should predicate spills and restores as appropriate */
            instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
            res = drreg_restore_reg_now(drcontext, ilist, where, pt, reg);
            instrlist_set_auto_predicate(ilist, pred);
            if (res != DRREG_SUCCESS)
                return res;
        } else {
            /* We lazily restore in drreg_event_bb_insert_late(), in case
             * someone else wants a local scratch.
             */
            pt->simd_pending_unreserved++;
        }
        pt->simd_reg[SIMD_IDX(reg)].in_use = false;
    }
#endif
    else {
        ASSERT(false, "internal error: not applicable register");
    }
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_reservation_info(void *drcontext, reg_id_t reg, opnd_t *opnd OUT,
                       bool *is_dr_slot OUT, uint *tls_offs OUT)
{
    drreg_reserve_info_t info = {
        sizeof(info),
    };
    per_thread_t *pt = get_tls_data(drcontext);
    drreg_status_t res;
    if ((reg < DR_REG_START_GPR || reg > DR_REG_STOP_GPR ||
         !pt->reg[GPR_IDX(reg)].in_use))
        return DRREG_ERROR_INVALID_PARAMETER;
#ifdef SIMD_SUPPORTED
    if (reg_is_vector_simd(reg) && !pt->simd_reg[SIMD_IDX(reg)].in_use)
        return DRREG_ERROR_INVALID_PARAMETER;
#endif
    res = drreg_reservation_info_ex(drcontext, reg, &info);
    if (res != DRREG_SUCCESS)
        return res;
    if (opnd != NULL)
        *opnd = info.opnd;
    if (is_dr_slot != NULL)
        *is_dr_slot = info.is_dr_slot;
    if (tls_offs != NULL)
        *tls_offs = info.tls_offs;
    return DRREG_SUCCESS;
}

static void
set_reservation_info(drreg_reserve_info_t *info, per_thread_t *pt, void *drcontext,
                     reg_id_t reg, reg_info_t *reg_info)
{
    info->reserved = reg_info->in_use;
    info->holds_app_value = reg_info->native;
    if (reg_info->native) {
        info->app_value_retained = false;
        info->opnd = opnd_create_null();
        info->is_dr_slot = false;
        info->tls_offs = -1;
    } else if (reg_info->xchg != DR_REG_NULL) {
        info->app_value_retained = true;
        info->opnd = opnd_create_reg(reg_info->xchg);
        info->is_dr_slot = false;
        info->tls_offs = -1;
    } else {
        info->app_value_retained = reg_info->ever_spilled;
        uint slot = reg_info->slot;
        if ((reg == DR_REG_NULL && !reg_info->native &&
             pt->slot_use[slot] != DR_REG_NULL) ||
            (reg != DR_REG_NULL && pt->slot_use[slot] == reg)) {
            if (slot < ops.num_spill_slots) {
                info->opnd = dr_raw_tls_opnd(drcontext, tls_seg, tls_slot_offs);
                info->is_dr_slot = false;
                info->tls_offs = tls_slot_offs + slot * sizeof(reg_t);
            } else {
                dr_spill_slot_t DR_slot = (dr_spill_slot_t)(slot - ops.num_spill_slots);
                if (DR_slot < dr_max_opnd_accessible_spill_slot())
                    info->opnd = dr_reg_spill_slot_opnd(drcontext, DR_slot);
                else {
                    /* Multi-step so no single opnd */
                    info->opnd = opnd_create_null();
                }
                info->is_dr_slot = true;
                info->tls_offs = DR_slot;
            }
        } else {
            info->opnd = opnd_create_null();
            info->is_dr_slot = false;
            info->tls_offs = -1;
        }
    }
}

drreg_status_t
drreg_reservation_info_ex(void *drcontext, reg_id_t reg, drreg_reserve_info_t *info OUT)
{
    per_thread_t *pt;
    reg_info_t *reg_info;

    if (info == NULL || info->size != sizeof(drreg_reserve_info_t))
        return DRREG_ERROR_INVALID_PARAMETER;

    pt = get_tls_data(drcontext);

    if (reg == DR_REG_NULL) {
        reg_info = &pt->aflags;
#ifdef SIMD_SUPPORTED
    } else if (reg_is_vector_simd(reg)) {
        reg_info = &pt->simd_reg[SIMD_IDX(reg)];
#endif
    } else {
        if (reg < DR_REG_START_GPR || reg > DR_REG_STOP_GPR)
            return DRREG_ERROR_INVALID_PARAMETER;
        reg_info = &pt->reg[GPR_IDX(reg)];
    }
    set_reservation_info(info, pt, drcontext, reg, reg_info);
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_is_register_dead(void *drcontext, reg_id_t reg, instr_t *inst, bool *dead)
{
    per_thread_t *pt = get_tls_data(drcontext);
    if (dead == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;
    if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
        drreg_status_t res = drreg_forward_analysis(drcontext, inst);
        if (res != DRREG_SUCCESS)
            return res;
        ASSERT(pt->live_idx == 0, "non-drmgr-insert always uses 0 index");
    }

    if (reg_is_gpr(reg))
        *dead = drvector_get_entry(&pt->reg[GPR_IDX(reg)].live, pt->live_idx) == REG_DEAD;
#ifdef SIMD_SUPPORTED
    else if (reg_is_vector_simd(reg)) {
        *dead = drvector_get_entry(&pt->simd_reg[SIMD_IDX(reg)].live, pt->live_idx) ==
            SIMD_ZMM_DEAD;
    }
#endif
    else
        return DRREG_ERROR;

    return DRREG_SUCCESS;
}

drreg_status_t
drreg_set_bb_properties(void *drcontext, drreg_bb_properties_t flags)
{
    per_thread_t *pt = get_tls_data(drcontext);
    if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_APP2APP &&
        drmgr_current_bb_phase(drcontext) == DRMGR_PHASE_ANALYSIS &&
        drmgr_current_bb_phase(drcontext) == DRMGR_PHASE_INSERTION)
        return DRREG_ERROR_FEATURE_NOT_AVAILABLE;
    /* XXX: interactions with multiple callers gets messy...for now we just or-in */
    pt->bb_props |= flags;
    LOG(drcontext, DR_LOG_ALL, 2, "%s: bb flags are now 0x%x\n", __FUNCTION__,
        pt->bb_props);
    return DRREG_SUCCESS;
}

/***************************************************************************
 * ARITHMETIC FLAGS
 */

/* The caller should only call if aflags are currently in xax.
 * If aflags are in use, moves them to TLS.
 * If not, restores aflags if necessary and restores xax.
 */
static void
drreg_move_aflags_from_reg(void *drcontext, instrlist_t *ilist, instr_t *where,
                           per_thread_t *pt, bool stateful)
{
#ifdef X86
    if (pt->aflags.in_use || !stateful) {
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": moving aflags from xax to slot\n",
            __FUNCTION__, pt->live_idx, get_where_app_pc(where));
        spill_reg_directly(drcontext, pt, DR_REG_XAX, AFLAGS_SLOT, ilist, where);
    } else if (!pt->aflags.native) {
        drreg_status_t res;
        LOG(drcontext, DR_LOG_ALL, 3,
            "%s @%d." PFX ": lazily restoring aflags for app xax\n", __FUNCTION__,
            pt->live_idx, get_where_app_pc(where));
        res = drreg_restore_aflags(drcontext, ilist, where, pt, true /*release*/);
        if (res != DRREG_SUCCESS)
            drreg_report_error(res, "failed to restore flags before app xax");
        pt->aflags.native = true;
        pt->slot_use[AFLAGS_SLOT] = DR_REG_NULL;
    }
    LOG(drcontext, DR_LOG_ALL, 3,
        "%s @%d." PFX ": restoring xax spilled for aflags in slot %d\n", __FUNCTION__,
        pt->live_idx, get_where_app_pc(where),
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].slot);
    if (ops.conservative ||
        drvector_get_entry(&pt->reg[DR_REG_XAX - DR_REG_START_GPR].live, pt->live_idx) ==
            REG_LIVE) {
        restore_reg_directly(drcontext, pt, DR_REG_XAX,
                             pt->reg[DR_REG_XAX - DR_REG_START_GPR].slot, ilist, where,
                             stateful);
    } else if (stateful)
        pt->slot_use[pt->reg[DR_REG_XAX - DR_REG_START_GPR].slot] = DR_REG_NULL;
    if (stateful) {
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use = false;
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].native = true;
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].ever_spilled = false;
        pt->aflags.xchg = DR_REG_NULL;
    }
#endif
}

/* May modify pt->aflags.xchg */
static drreg_status_t
drreg_spill_aflags(void *drcontext, instrlist_t *ilist, instr_t *where, per_thread_t *pt)
{
#ifdef X86
    uint aflags = (uint)(ptr_uint_t)drvector_get_entry(&pt->aflags.live, pt->live_idx);
    reg_id_t xax_swap = DR_REG_NULL;
    drreg_status_t res;
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX "\n", __FUNCTION__, pt->live_idx,
        get_where_app_pc(where));
    /* It may be in-use for ourselves, storing the flags in xax. */
    if (pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use && pt->aflags.xchg != DR_REG_XAX) {
        /* No way to tell whoever is using xax that we need it, so we pick an
         * unreserved reg, spill it, and put xax there temporarily.  We store
         * aflags in our dedicated aflags tls slot and don't try to keep it in
         * this reg.
         */
        res = drreg_reserve_reg_internal(drcontext, DRREG_GPR_SPILL_CLASS, ilist, where,
                                         NULL, false, &xax_swap);
        if (res != DRREG_SUCCESS)
            return res;
        LOG(drcontext, DR_LOG_ALL, 3, "  xax is in use: using %s temporarily\n",
            get_register_name(xax_swap));
        PRE(ilist, where,
            INSTR_CREATE_xchg(drcontext, opnd_create_reg(DR_REG_XAX),
                              opnd_create_reg(xax_swap)));
    }
    if (!pt->reg[DR_REG_XAX - DR_REG_START_GPR].native) {
        /* xax is unreserved but not restored */
        ASSERT(pt->slot_use[pt->reg[DR_REG_XAX - DR_REG_START_GPR].slot] == DR_REG_XAX,
               "xax tracking error");
        LOG(drcontext, DR_LOG_ALL, 3, "  using un-restored xax in slot %d\n",
            pt->reg[DR_REG_XAX - DR_REG_START_GPR].slot);
    } else if (pt->aflags.xchg != DR_REG_XAX) {
        uint xax_slot = find_free_slot(pt);
        if (xax_slot == MAX_SPILLS)
            return DRREG_ERROR_OUT_OF_SLOTS;
        if (ops.conservative ||
            drvector_get_entry(&pt->reg[DR_REG_XAX - DR_REG_START_GPR].live,
                               pt->live_idx) == REG_LIVE)
            spill_reg_directly(drcontext, pt, DR_REG_XAX, xax_slot, ilist, where);
        else
            pt->slot_use[xax_slot] = DR_REG_XAX;
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].slot = xax_slot;
        ASSERT(pt->slot_use[xax_slot] == DR_REG_XAX, "slot should be for xax");
    }
    PRE(ilist, where, INSTR_CREATE_lahf(drcontext));
    if (TEST(EFLAGS_READ_OF, aflags)) {
        PRE(ilist, where,
            INSTR_CREATE_setcc(drcontext, OP_seto, opnd_create_reg(DR_REG_AL)));
    }
    if (xax_swap != DR_REG_NULL) {
        PRE(ilist, where,
            INSTR_CREATE_xchg(drcontext, opnd_create_reg(xax_swap),
                              opnd_create_reg(DR_REG_XAX)));
        spill_reg_directly(drcontext, pt, xax_swap, AFLAGS_SLOT, ilist, where);
        res = drreg_unreserve_register(drcontext, ilist, where, xax_swap);
        if (res != DRREG_SUCCESS)
            return res; /* XXX: undo already-inserted instrs? */
    } else {
        /* As an optimization we keep the flags in xax itself until forced to move
         * them to the aflags TLS slot.
         */
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use = true;
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].native = false;
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].ever_spilled = true;
        pt->aflags.xchg = DR_REG_XAX;
    }

#elif defined(AARCHXX)
    drreg_status_t res = DRREG_SUCCESS;
    reg_id_t scratch;
    res = drreg_reserve_reg_internal(drcontext, DRREG_GPR_SPILL_CLASS, ilist, where, NULL,
                                     false, &scratch);
    if (res != DRREG_SUCCESS)
        return res;
    dr_save_arith_flags_to_reg(drcontext, ilist, where, scratch);
    spill_reg_directly(drcontext, pt, scratch, AFLAGS_SLOT, ilist, where);
    res = drreg_unreserve_register(drcontext, ilist, where, scratch);
    if (res != DRREG_SUCCESS)
        return res; /* XXX: undo already-inserted instrs? */
#endif
    return DRREG_SUCCESS;
}

static drreg_status_t
drreg_restore_aflags(void *drcontext, instrlist_t *ilist, instr_t *where,
                     per_thread_t *pt, bool release)
{
#ifdef X86
    uint aflags = (uint)(ptr_uint_t)drvector_get_entry(&pt->aflags.live, pt->live_idx);
    uint temp_slot = 0;
    reg_id_t xax_swap = DR_REG_NULL;
    drreg_status_t res;
    LOG(drcontext, DR_LOG_ALL, 3,
        "%s @%d." PFX ": release=%d xax-in-use=%d,slot=%d xchg=%s\n", __FUNCTION__,
        pt->live_idx, get_where_app_pc(where), release,
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use,
        pt->reg[DR_REG_XAX - DR_REG_START_GPR].slot, get_register_name(pt->aflags.xchg));
    if (pt->aflags.native)
        return DRREG_SUCCESS;
    if (pt->aflags.xchg == DR_REG_XAX) {
        ASSERT(pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use, "eflags-in-xax error");
    } else {
        temp_slot = find_free_slot(pt);
        if (temp_slot == MAX_SPILLS)
            return DRREG_ERROR_OUT_OF_SLOTS;
        if (pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use) {
            /* We pick an unreserved reg, spill it, and put xax there temporarily. */
            res = drreg_reserve_reg_internal(drcontext, DRREG_GPR_SPILL_CLASS, ilist,
                                             where, NULL, false, &xax_swap);
            if (res != DRREG_SUCCESS)
                return res;
            LOG(drcontext, DR_LOG_ALL, 3, "  xax is in use: using %s temporarily\n",
                get_register_name(xax_swap));
            PRE(ilist, where,
                INSTR_CREATE_xchg(drcontext, opnd_create_reg(DR_REG_XAX),
                                  opnd_create_reg(xax_swap)));
        } else if (ops.conservative ||
                   drvector_get_entry(&pt->reg[DR_REG_XAX - DR_REG_START_GPR].live,
                                      pt->live_idx) == REG_LIVE)
            spill_reg_directly(drcontext, pt, DR_REG_XAX, temp_slot, ilist, where);
        restore_reg_directly(drcontext, pt, DR_REG_XAX, AFLAGS_SLOT, ilist, where,
                             release);
    }
    if (TEST(EFLAGS_READ_OF, aflags)) {
        /* i#2351: DR's "add 0x7f, %al" is destructive.  Instead we use a
         * cmp so we can avoid messing up the value in al, which is
         * required for keeping the flags in xax.
         */
        PRE(ilist, where,
            INSTR_CREATE_cmp(drcontext, opnd_create_reg(DR_REG_AL),
                             OPND_CREATE_INT8(-127)));
    }
    PRE(ilist, where, INSTR_CREATE_sahf(drcontext));
    if (xax_swap != DR_REG_NULL) {
        PRE(ilist, where,
            INSTR_CREATE_xchg(drcontext, opnd_create_reg(xax_swap),
                              opnd_create_reg(DR_REG_XAX)));
        res = drreg_unreserve_register(drcontext, ilist, where, xax_swap);
        if (res != DRREG_SUCCESS)
            return res; /* XXX: undo already-inserted instrs? */
    } else if (pt->aflags.xchg == DR_REG_XAX) {
        if (release) {
            pt->aflags.xchg = DR_REG_NULL;
            pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use = false;
        }
    } else {
        if (ops.conservative ||
            drvector_get_entry(&pt->reg[DR_REG_XAX - DR_REG_START_GPR].live,
                               pt->live_idx) == REG_LIVE) {
            restore_reg_directly(drcontext, pt, DR_REG_XAX, temp_slot, ilist, where,
                                 true);
        }
    }
#elif defined(AARCHXX)
    drreg_status_t res = DRREG_SUCCESS;
    reg_id_t scratch;
    res = drreg_reserve_reg_internal(drcontext, DRREG_GPR_SPILL_CLASS, ilist, where, NULL,
                                     false, &scratch);
    if (res != DRREG_SUCCESS)
        return res;
    restore_reg_directly(drcontext, pt, scratch, AFLAGS_SLOT, ilist, where, release);
    dr_restore_arith_flags_from_reg(drcontext, ilist, where, scratch);
    res = drreg_unreserve_register(drcontext, ilist, where, scratch);
    if (res != DRREG_SUCCESS)
        return res; /* XXX: undo already-inserted instrs? */
#endif
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_reserve_aflags(void *drcontext, instrlist_t *ilist, instr_t *where)
{
    per_thread_t *pt = get_tls_data(drcontext);
    dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
    drreg_status_t res;
    uint aflags;
    if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
        res = drreg_forward_analysis(drcontext, where);
        if (res != DRREG_SUCCESS)
            return res;
        ASSERT(pt->live_idx == 0, "non-drmgr-insert always uses 0 index");
    }
    aflags = (uint)(ptr_uint_t)drvector_get_entry(&pt->aflags.live, pt->live_idx);
    /* Just like scratch regs, flags are exclusively owned */
    if (pt->aflags.in_use)
        return DRREG_ERROR_IN_USE;
    if (!TESTANY(EFLAGS_READ_ARITH, aflags)) {
        /* If the flags were not yet lazily restored and are now dead, clear the slot */
        if (!pt->aflags.native)
            pt->slot_use[AFLAGS_SLOT] = DR_REG_NULL;
        pt->aflags.in_use = true;
        pt->aflags.native = true;
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": aflags are dead\n", __FUNCTION__,
            pt->live_idx, get_where_app_pc(where));
        return DRREG_SUCCESS;
    }
    /* Check for a prior reservation not yet lazily restored */
    if (!pt->aflags.native IF_X86(||
                                  (pt->reg[DR_REG_XAX - DR_REG_START_GPR].in_use &&
                                   pt->aflags.xchg == DR_REG_XAX))) {
        LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": using un-restored aflags\n",
            __FUNCTION__, pt->live_idx, get_where_app_pc(where));
        ASSERT(pt->aflags.xchg != DR_REG_NULL || pt->slot_use[AFLAGS_SLOT] != DR_REG_NULL,
               "lost slot reservation");
        pt->aflags.native = false;
        pt->aflags.in_use = true;
        return DRREG_SUCCESS;
    }

    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX ": spilling aflags\n", __FUNCTION__,
        pt->live_idx, get_where_app_pc(where));
    /* drreg_spill_aflags writes to this, so clear first.  The inconsistent combo
     * xchg-null but xax-in-use won't happen b/c we'll use un-restored above.
     */
    pt->aflags.xchg = DR_REG_NULL;
    /* XXX i#2585: drreg should predicate spills and restores as appropriate */
    instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
    res = drreg_spill_aflags(drcontext, ilist, where, pt);
    instrlist_set_auto_predicate(ilist, pred);
    if (res != DRREG_SUCCESS)
        return res;
    pt->aflags.in_use = true;
    pt->aflags.native = false;
    pt->aflags.slot = AFLAGS_SLOT;
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_unreserve_aflags(void *drcontext, instrlist_t *ilist, instr_t *where)
{
    per_thread_t *pt = get_tls_data(drcontext);
    if (!pt->aflags.in_use)
        return DRREG_ERROR_INVALID_PARAMETER;
    pt->aflags.in_use = false;
    if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
        dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
        /* We have no way to lazily restore.  We do not bother at this point
         * to try and eliminate back-to-back spill/restore pairs.
         */
        /* XXX i#2585: drreg should predicate spills and restores as appropriate */
        instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
        if (pt->aflags.xchg != DR_REG_NULL)
            drreg_move_aflags_from_reg(drcontext, ilist, where, pt, true);
        else if (!pt->aflags.native) {
            drreg_restore_aflags(drcontext, ilist, where, pt, true /*release*/);
            pt->aflags.native = true;
        }
        instrlist_set_auto_predicate(ilist, pred);
        pt->slot_use[AFLAGS_SLOT] = DR_REG_NULL;
    }
    LOG(drcontext, DR_LOG_ALL, 3, "%s @%d." PFX "\n", __FUNCTION__, pt->live_idx,
        get_where_app_pc(where));
    /* We lazily restore in drreg_event_bb_insert_late(), in case
     * someone else wants the aflags locally.
     */
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_aflags_liveness(void *drcontext, instr_t *inst, OUT uint *value)
{
    per_thread_t *pt = get_tls_data(drcontext);
    if (value == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;
    if (drmgr_current_bb_phase(drcontext) != DRMGR_PHASE_INSERTION) {
        drreg_status_t res = drreg_forward_analysis(drcontext, inst);
        if (res != DRREG_SUCCESS)
            return res;
        ASSERT(pt->live_idx == 0, "non-drmgr-insert always uses 0 index");
    }
    *value = (uint)(ptr_uint_t)drvector_get_entry(&pt->aflags.live, pt->live_idx);
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_are_aflags_dead(void *drcontext, instr_t *inst, bool *dead)
{
    uint flags;
    drreg_status_t res = drreg_aflags_liveness(drcontext, inst, &flags);
    if (res != DRREG_SUCCESS)
        return res;
    if (dead == NULL)
        return DRREG_ERROR_INVALID_PARAMETER;
    *dead = !TESTANY(EFLAGS_READ_ARITH, flags);
    return DRREG_SUCCESS;
}

drreg_status_t
drreg_restore_app_aflags(void *drcontext, instrlist_t *ilist, instr_t *where)
{
    per_thread_t *pt = get_tls_data(drcontext);
    drreg_status_t res = DRREG_SUCCESS;
    if (!pt->aflags.native) {
        dr_pred_type_t pred = instrlist_get_auto_predicate(ilist);
        LOG(drcontext, DR_LOG_ALL, 3,
            "%s @%d." PFX ": restoring app aflags as requested\n", __FUNCTION__,
            pt->live_idx, get_where_app_pc(where));
        /* XXX i#2585: drreg should predicate spills and restores as appropriate */
        instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
        res = drreg_restore_aflags(drcontext, ilist, where, pt, !pt->aflags.in_use);
        instrlist_set_auto_predicate(ilist, pred);
        if (!pt->aflags.in_use)
            pt->aflags.native = true;
    }
    return res;
}

/***************************************************************************
 * RESTORE STATE
 */

static bool
is_our_spill_or_restore(void *drcontext, instr_t *instr, instr_t *next_instr,
                        bool *spill OUT, reg_id_t *reg_spilled OUT, uint *slot_out OUT,
                        uint *offs_out OUT, bool *is_indirectly_spilled OUT)
{
    bool tls;
    uint slot, offs;
    reg_id_t reg;
    bool is_spilled;  /* Flag denoting spill or restore*/
    bool is_indirect; /* Flag denoting direct or indirect access */

    is_indirect = false;
    if (is_indirectly_spilled != NULL)
        *is_indirectly_spilled = is_indirect;

    if (!instr_is_reg_spill_or_restore(drcontext, instr, &tls, &is_spilled, &reg, &offs))
        return false;
    /* Checks whether this from our direct raw TLS for gpr registers. */
    if (tls && offs >= tls_slot_offs &&
        offs < (tls_slot_offs + ops.num_spill_slots * sizeof(reg_t))) {
        slot = (offs - tls_slot_offs) / sizeof(reg_t);
    }
#ifdef SIMD_SUPPORTED
    else if (tls && offs == tls_simd_offs &&
             !(is_spilled) /* Cant be a spill bc loading block */) {
        /* In order to detect indirect spills, the loading of the pointer
         * to the indrect block must be done exactly prior. We assume that
         * nobody else can interfere with our indirect load sequence for
         * simd registers.
         */
        ASSERT(next_instr != NULL, "next_instr cannot be NULL");
        /* FIXME i#3844: Might need to change this assert when
         * supporting other register spillage.
         */
        ASSERT(instr_get_opcode(next_instr) == OP_movdqa ||
                   instr_get_opcode(next_instr) == OP_vmovdqa,
               "next instruction needs to be a mov");
        is_indirect = true;
        opnd_t dst = instr_get_dst(next_instr, 0);
        opnd_t src = instr_get_src(next_instr, 0);

        if (opnd_is_reg(dst) && reg_is_vector_simd(opnd_get_reg(dst)) &&
            opnd_is_base_disp(src)) {
            reg = opnd_get_reg(dst);
            is_spilled = false;
            int disp = opnd_get_disp(src);
            slot = disp / SIMD_REG_SIZE;
        } else if (opnd_is_reg(src) && reg_is_vector_simd(opnd_get_reg(src)) &&
                   opnd_is_base_disp(dst)) {
            reg = opnd_get_reg(src);
            is_spilled = true;
            int disp = opnd_get_disp(dst);
            /* Each slot here is of size SIMD_REG_SIZE. We perform a division to get the
             * slot based on the displacement.
             */
            slot = disp / SIMD_REG_SIZE;
        } else {
            ASSERT(false, "use of block must involve a load/store");
            return false;
        }
    }
#endif
    else {
        /* We assume a DR spill slot, in TLS or thread-private mcontext */
        if (tls) {
            /* We assume the DR slots are either low-to-high or high-to-low. */
            uint DR_min_offs =
                opnd_get_disp(dr_reg_spill_slot_opnd(drcontext, SPILL_SLOT_1));
            uint DR_max_offs = opnd_get_disp(
                dr_reg_spill_slot_opnd(drcontext, dr_max_opnd_accessible_spill_slot()));
            uint max_DR_slot = (uint)dr_max_opnd_accessible_spill_slot();
            if (DR_min_offs > DR_max_offs) {
                if (offs > DR_min_offs) {
                    slot = (offs - DR_min_offs) / sizeof(reg_t);
                } else if (offs < DR_max_offs) {
                    /* Fix hidden slot regardless of low-to-high or vice versa. */
                    slot = max_DR_slot + 1;
                } else {
                    slot = (DR_min_offs - offs) / sizeof(reg_t);
                }
            } else {
                if (offs > DR_max_offs) {
                    slot = (offs - DR_max_offs) / sizeof(reg_t);
                } else if (offs < DR_min_offs) {
                    /* Fix hidden slot regardless of low-to-high or vice versa. */
                    slot = max_DR_slot + 1;
                } else {
                    slot = (offs - DR_min_offs) / sizeof(reg_t);
                }
            }
            if (slot > max_DR_slot) {
                /* This is not a drreg spill, but some TLS access by
                 * tool instrumentation (i#2035).
                 */
                return false;
            }
#ifdef X86
            if (slot > max_DR_slot - 1) {
                /* FIXME i#2933: We rule out the 3rd DR TLS slot b/c it's used by
                 * DR for purposes where there's no restore paired with a spill.
                 * Another tool component could also use the other slots that way,
                 * though: we need a more foolproof solution.  For now we have a hole
                 * and tools should allocate enough dedicated drreg TLS slots to
                 * ensure robustness.
                 */
                return false;
            }
#endif
        } else {
            /* We assume mcontext spill offs is 0-based. */
            slot = offs / sizeof(reg_t);
        }
        slot += ops.num_spill_slots;
    }
    if (spill != NULL)
        *spill = is_spilled;
    if (reg_spilled != NULL)
        *reg_spilled = reg;
    if (slot_out != NULL)
        *slot_out = slot;
    if (offs_out != NULL)
        *offs_out = offs;
    if (is_indirectly_spilled != NULL)
        *is_indirectly_spilled = is_indirect;
    return true;
}

drreg_status_t
drreg_is_instr_spill_or_restore(void *drcontext, instr_t *instr, bool *spill OUT,
                                bool *restore OUT, reg_id_t *reg_spilled OUT)
{
    bool is_spill;
    if (!is_our_spill_or_restore(drcontext, instr, instr_get_next(instr), &is_spill,
                                 reg_spilled, NULL, NULL, NULL)) {
        if (spill != NULL)
            *spill = false;
        if (restore != NULL)
            *restore = false;
        return DRREG_SUCCESS;
    }
    if (spill != NULL)
        *spill = is_spill;
    if (restore != NULL)
        *restore = !is_spill;
    return DRREG_SUCCESS;
}

static bool
drreg_event_restore_state(void *drcontext, bool restore_memory,
                          dr_restore_state_info_t *info)
{
    /* To achieve a clean and simple reserve-and-unreserve interface w/o specifying
     * up front how many cross-app-instr scratch regs (and then limited to whole-bb
     * regs with stored per-bb info, like Dr. Memory does), we have to pay with a
     * complex state xl8 scheme.  We need to decode the in-cache fragment and walk
     * it, recognizing our own spills and restores.  We distinguish a tool value
     * spill to a temp slot (from drreg_event_bb_insert_late()) by watching for
     * a spill of an already-spilled reg to a different slot.
     */
    uint spilled_to[DR_NUM_GPR_REGS];
    uint spilled_to_aflags = MAX_SPILLS;
#ifdef SIMD_SUPPORTED
    uint spilled_simd_to[DR_NUM_SIMD_VECTOR_REGS];
    reg_id_t simd_slot_use[MAX_SIMD_SPILLS];
    byte simd_buf[SIMD_REG_SIZE];
#endif
    reg_id_t reg;
    instr_t inst;
    instr_t next_inst; /* used to analyse the load to an indirect block. */
    byte *prev_pc, *pc = info->fragment_info.cache_start_pc;
    uint offs;
    bool is_spill;
    bool is_indirect_spill;
#ifdef X86
    bool prev_xax_spill = false;
    bool aflags_in_xax = false;
#endif
    uint slot;
    if (pc == NULL)
        return true; /* fault not in cache */
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++)
        spilled_to[GPR_IDX(reg)] = MAX_SPILLS;
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++)
        spilled_simd_to[SIMD_IDX(reg)] = MAX_SIMD_SPILLS;
    for (slot = 0; slot < ops.num_spill_simd_slots; ++slot)
        simd_slot_use[slot] = DR_REG_NULL;
#endif

    LOG(drcontext, DR_LOG_ALL, 3,
        "%s: processing fault @" PFX ": decoding from " PFX "\n", __FUNCTION__,
        info->raw_mcontext->pc, pc);
    instr_init(drcontext, &inst);
    instr_init(drcontext, &next_inst);
    while (pc < info->raw_mcontext->pc) {
        instr_reset(drcontext, &inst);
        instr_reset(drcontext, &next_inst);
        prev_pc = pc;
        pc = decode(drcontext, pc, &inst);
        if (pc == NULL) {
            LOG(drcontext, DR_LOG_ALL, 3, "%s @" PFX " \n", __FUNCTION__, prev_pc,
                "PC decoding returned NULL during state restoration");
            instr_free(drcontext, &inst);
            instr_free(drcontext, &next_inst);
            return true;
        }
        decode(drcontext, pc, &next_inst);
        if (is_our_spill_or_restore(drcontext, &inst, &next_inst, &is_spill, &reg, &slot,
                                    &offs, &is_indirect_spill)) {
            LOG(drcontext, DR_LOG_ALL, 3,
                "%s @" PFX " found %s to %s offs=0x%x => slot %d\n", __FUNCTION__,
                prev_pc, is_spill ? "is_spill" : "restore", get_register_name(reg), offs,
                slot);
            if (is_spill) {
                if (slot == AFLAGS_SLOT) {
                    spilled_to_aflags = slot;
#ifdef SIMD_SUPPORTED
                } else if (is_indirect_spill) {
                    if (reg_is_vector_simd(reg)) {
                        if (spilled_simd_to[SIMD_IDX(reg)] < ops.num_spill_simd_slots &&
                            /* allow redundant spill */
                            spilled_simd_to[SIMD_IDX(reg)] != slot) {
                            /* This reg is already spilled: we assume that this new
                             * spill is to a tmp slot for preserving the tool's value.
                             */
                            LOG(drcontext, DR_LOG_ALL, 3,
                                "%s @" PFX ": ignoring tool is_spill\n", __FUNCTION__,
                                pc);
                        } else {
                            spilled_simd_to[SIMD_IDX(reg)] = slot;
                            simd_slot_use[slot] = reg;
                        }
                    }
#endif
                } else if (spilled_to[GPR_IDX(reg)] < MAX_SPILLS &&
                           /* allow redundant is_spill */
                           spilled_to[GPR_IDX(reg)] != slot) {
                    /* This reg is already spilled: we assume that this new spill
                     * is to a tmp slot for preserving the tool's value.
                     */
                    LOG(drcontext, DR_LOG_ALL, 3, "%s @" PFX ": ignoring tool is_spill\n",
                        __FUNCTION__, pc);
                } else {
                    spilled_to[GPR_IDX(reg)] = slot;
                }
            } else {
                /* Not a spill, but a restore. */
                if (slot == AFLAGS_SLOT && spilled_to_aflags == slot) {
                    spilled_to_aflags = MAX_SPILLS;
#ifdef SIMD_SUPPORTED
                } else if (is_indirect_spill) {
                    if (spilled_simd_to[SIMD_IDX(reg)] == slot) {
                        spilled_simd_to[SIMD_IDX(reg)] = MAX_SIMD_SPILLS;
                        simd_slot_use[slot] = DR_REG_NULL;
                    }
#endif
                } else if (spilled_to[GPR_IDX(reg)] == slot)
                    spilled_to[GPR_IDX(reg)] = MAX_SPILLS;
                else {
                    LOG(drcontext, DR_LOG_ALL, 3, "%s @" PFX ": ignoring restore\n",
                        __FUNCTION__, pc);
                }
            }
#ifdef X86
            if (reg == DR_REG_XAX) {
                prev_xax_spill = true;
                if (aflags_in_xax)
                    aflags_in_xax = false;
            }
#endif
        }
#ifdef X86
        else if (prev_xax_spill && instr_get_opcode(&inst) == OP_lahf && is_spill)
            aflags_in_xax = true;
        else if (aflags_in_xax && instr_get_opcode(&inst) == OP_sahf)
            aflags_in_xax = false;
#endif
    }
    instr_free(drcontext, &inst);
    instr_free(drcontext, &next_inst);

    if (spilled_to_aflags < MAX_SPILLS IF_X86(|| aflags_in_xax)) {
        reg_t newval = info->mcontext->xflags;
        reg_t val;
#ifdef X86
        uint sahf;
        if (aflags_in_xax)
            val = info->mcontext->xax;
        else
#endif
            val = get_directly_spilled_value(drcontext, spilled_to_aflags);
#ifdef AARCHXX
        newval &= ~(EFLAGS_ARITH);
        newval |= val;
#elif defined(X86)
        sahf = (val & 0xff00) >> 8;
        newval &= ~(EFLAGS_ARITH);
        newval |= sahf;
        if (TEST(1, val)) /* seto */
            newval |= EFLAGS_OF;
#endif
        LOG(drcontext, DR_LOG_ALL, 3, "%s: restoring aflags from " PFX " to " PFX "\n",
            __FUNCTION__, info->mcontext->xflags, newval);
        info->mcontext->xflags = newval;
    }
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
        if (spilled_to[GPR_IDX(reg)] < MAX_SPILLS) {
            reg_t val = get_directly_spilled_value(drcontext, spilled_to[GPR_IDX(reg)]);
            LOG(drcontext, DR_LOG_ALL, 3,
                "%s: restoring %s from slot %d from " PFX " to " PFX "\n", __FUNCTION__,
                get_register_name(reg), spilled_to[GPR_IDX(reg)],
                reg_get_value(reg, info->mcontext), val);
            reg_set_value(reg, info->mcontext, val);
        }
    }
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++) {
        uint slot = spilled_simd_to[SIMD_IDX(reg)];
        if (slot < ops.num_spill_simd_slots) {
            reg_id_t actualreg = simd_slot_use[slot];
            ASSERT(actualreg != DR_REG_NULL, "internal error, register should be valid");
            if (reg_is_strictly_xmm(actualreg)) {
                get_indirectly_spilled_value(drcontext, reg, actualreg, simd_buf,
                                             XMM_REG_SIZE);
            } else if (reg_is_strictly_ymm(reg)) {
                /* The callers should catch this when checking the spill class. */
                ASSERT(false, "internal error: ymm registers not supported yet.");
            } else if (reg_is_strictly_zmm(reg)) {
                /* The callers should catch this when checking the spill class. */
                ASSERT(false, "inernal error: zmm registers not supported yet.");
            } else
                ASSERT(false, "internal error: not an applicable register.");
            reg_set_value_ex(reg, info->mcontext, simd_buf);
        }
    }
#endif
    return true;
}

/***************************************************************************
 * INIT AND EXIT
 */

static int drreg_init_count;

static per_thread_t init_pt;

static per_thread_t *
get_tls_data(void *drcontext)
{
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    /* Support use during init (i#2910). */
    if (pt == NULL)
        return &init_pt;
    return pt;
}

static void
tls_data_init(void *drcontext, per_thread_t *pt)
{
    reg_id_t reg;
    memset(pt, 0, sizeof(*pt));
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
        drvector_init(&pt->reg[GPR_IDX(reg)].live, 20, false /*!synch*/, NULL);
        pt->reg[GPR_IDX(reg)].native = true;
    }
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++) {
        drvector_init(&pt->simd_reg[SIMD_IDX(reg)].live, DR_NUM_SIMD_VECTOR_REGS,
                      false /*!synch*/, NULL);
        pt->simd_reg[SIMD_IDX(reg)].native = true;
    }
    /* We align the block on a 64-byte boundary. */
    if (ops.num_spill_simd_slots > 0) {
        if (drcontext == GLOBAL_DCONTEXT) {
            pt->simd_spill_start =
                dr_global_alloc((SIMD_REG_SIZE * ops.num_spill_simd_slots) + 63);
        } else {
            pt->simd_spill_start = dr_thread_alloc(
                drcontext, (SIMD_REG_SIZE * ops.num_spill_simd_slots) + 63);
        }
        pt->simd_spills = (byte *)ALIGN_FORWARD(pt->simd_spill_start, 64);
    }

#endif
    pt->aflags.native = true;
    drvector_init(&pt->aflags.live, 20, false /*!synch*/, NULL);
}

static void
tls_data_free(void *drcontext, per_thread_t *pt)
{
    reg_id_t reg;
    for (reg = DR_REG_START_GPR; reg <= DR_REG_STOP_GPR; reg++) {
        drvector_delete(&pt->reg[GPR_IDX(reg)].live);
    }
#ifdef SIMD_SUPPORTED
    for (reg = DR_REG_APPLICABLE_START_SIMD; reg <= DR_REG_APPLICABLE_STOP_SIMD; reg++) {
        drvector_delete(&pt->simd_reg[SIMD_IDX(reg)].live);
    }
    if (ops.num_spill_simd_slots > 0) {
        ASSERT(pt->simd_spill_start != NULL, "SIMD slot storage cannot be NULL");
        if (drcontext == GLOBAL_DCONTEXT) {
            dr_global_free(pt->simd_spill_start,
                           (SIMD_REG_SIZE * ops.num_spill_simd_slots) + 63);
        } else {
            dr_thread_free(drcontext, pt->simd_spill_start,
                           (SIMD_REG_SIZE * ops.num_spill_simd_slots) + 63);
        }
    }
#endif
    drvector_delete(&pt->aflags.live);
}

static void
drreg_thread_init(void *drcontext)
{
    per_thread_t *pt = (per_thread_t *)dr_thread_alloc(drcontext, sizeof(*pt));
    drmgr_set_tls_field(drcontext, tls_idx, (void *)pt);
    tls_data_init(drcontext, pt);
    pt->tls_seg_base = dr_get_dr_segment_base(tls_seg);
    /* Place the pointer to the SIMD block inside a hidden slot.
     * XXX: We could get API to access raw TLS slots like this.
     */
    void **addr = (void **)(pt->tls_seg_base + tls_simd_offs);
    *addr = pt->simd_spills;
}

static void
drreg_thread_exit(void *drcontext)
{
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    tls_data_free(drcontext, pt);
    dr_thread_free(drcontext, pt, sizeof(*pt));
}

static uint
get_updated_num_slots(bool do_not_sum_slots, uint cur_slots, uint new_slots)
{
    if (do_not_sum_slots) {
        if (new_slots > cur_slots)
            return new_slots;
        else
            return cur_slots;
    } else
        return cur_slots + new_slots;
}

drreg_status_t
drreg_init(drreg_options_t *ops_in)
{
    uint prior_slots = ops.num_spill_slots;
    drmgr_priority_t high_priority = { sizeof(high_priority),
                                       DRMGR_PRIORITY_NAME_DRREG_HIGH, NULL, NULL,
                                       DRMGR_PRIORITY_INSERT_DRREG_HIGH };
    drmgr_priority_t low_priority = { sizeof(low_priority), DRMGR_PRIORITY_NAME_DRREG_LOW,
                                      NULL, NULL, DRMGR_PRIORITY_INSERT_DRREG_LOW };
    drmgr_priority_t fault_priority = { sizeof(fault_priority),
                                        DRMGR_PRIORITY_NAME_DRREG_FAULT, NULL, NULL,
                                        DRMGR_PRIORITY_FAULT_DRREG };

    int count = dr_atomic_add32_return_sum(&drreg_init_count, 1);
    if (count == 1) {
        drmgr_init();

        if (!drmgr_register_thread_init_event(drreg_thread_init) ||
            !drmgr_register_thread_exit_event(drreg_thread_exit))
            return DRREG_ERROR;
        tls_idx = drmgr_register_tls_field();
        if (tls_idx == -1)
            return DRREG_ERROR;

        if (!drmgr_register_bb_instrumentation_event(NULL, drreg_event_bb_insert_early,
                                                     &high_priority) ||
            !drmgr_register_bb_instrumentation_event(
                drreg_event_bb_analysis, drreg_event_bb_insert_late, &low_priority) ||
            !drmgr_register_restore_state_ex_event_ex(drreg_event_restore_state,
                                                      &fault_priority))
            return DRREG_ERROR;
#ifdef X86
        /* We get an extra slot for aflags xax, rather than just documenting that
         * clients should add 2 instead of just 1, as there are many existing clients.
         */
        ops.num_spill_slots = 1;
#endif
        /* Support use during init when there is no TLS (i#2910). */
        tls_data_init(GLOBAL_DCONTEXT, &init_pt);
    }

    if (ops_in->struct_size < offsetof(drreg_options_t, error_callback))
        return DRREG_ERROR_INVALID_PARAMETER;

    /* Instead of allowing only one drreg_init() and all other components to be
     * passed in scratch regs by a master, which is not always an easy-to-use
     * model, we instead consider all callers' requests, combining the option
     * fields.  We don't shift init to drreg_thread_init() or sthg b/c we really
     * want init-time error codes returning from drreg_init().
     */

    /* Sum the spill slots, honoring a new or prior do_not_sum_slots by taking
     * the max instead of summing.
     */
    if (ops_in->struct_size > offsetof(drreg_options_t, do_not_sum_slots)) {
        ops.num_spill_slots = get_updated_num_slots(
            ops_in->do_not_sum_slots, ops.num_spill_slots, ops_in->num_spill_slots);
        if (ops_in->struct_size > offsetof(drreg_options_t, num_spill_simd_slots))
            ops.num_spill_simd_slots =
                get_updated_num_slots(ops_in->do_not_sum_slots, ops.num_spill_simd_slots,
                                      ops_in->num_spill_simd_slots);
        ops.do_not_sum_slots = ops_in->do_not_sum_slots;
    } else {
        ops.num_spill_slots = get_updated_num_slots(
            ops.do_not_sum_slots, ops.num_spill_slots, ops_in->num_spill_slots);
        if (ops_in->struct_size > offsetof(drreg_options_t, num_spill_simd_slots))
            ops.num_spill_simd_slots =
                get_updated_num_slots(ops.do_not_sum_slots, ops.num_spill_simd_slots,
                                      ops_in->num_spill_simd_slots);
        ops.do_not_sum_slots = false;
    }

    /* If anyone wants to be conservative, then be conservative. */
    ops.conservative = ops.conservative || ops_in->conservative;

    /* The first callback wins. */
    if (ops_in->struct_size > offsetof(drreg_options_t, error_callback) &&
        ops.error_callback == NULL)
        ops.error_callback = ops_in->error_callback;

    if (prior_slots > 0) {
        /* +1 for the pointer to the indirect spill block, see below. */
        if (!dr_raw_tls_cfree(tls_simd_offs, prior_slots + 1))
            return DRREG_ERROR;
    }

    /* 0 spill slots is supported and just fills in tls_seg for us. */
    /* We are allocating an additional slot for the pointer to the indirect
     * spill block.
     */
    if (!dr_raw_tls_calloc(&tls_seg, &tls_simd_offs, ops.num_spill_slots + 1, 0))
        return DRREG_ERROR_OUT_OF_SLOTS;

    /* Increment offset so that we now directly point to GPR slots, skipping the
     * pointer to the indirect SIMD block. We are treating this extra slot differently
     * from the aflags slot, because its offset is distinctly used for spilling and
     * restoring indirectly vs. directly.
     */
    tls_slot_offs = tls_simd_offs + sizeof(void *);

    return DRREG_SUCCESS;
}

drreg_status_t
drreg_exit(void)
{
    int count = dr_atomic_add32_return_sum(&drreg_init_count, -1);
    if (count != 0)
        return DRREG_SUCCESS;

    tls_data_free(GLOBAL_DCONTEXT, &init_pt);

    if (!drmgr_unregister_thread_init_event(drreg_thread_init) ||
        !drmgr_unregister_thread_exit_event(drreg_thread_exit))
        return DRREG_ERROR;

    drmgr_unregister_tls_field(tls_idx);
    if (!drmgr_unregister_bb_insertion_event(drreg_event_bb_insert_early) ||
        !drmgr_unregister_bb_instrumentation_event(drreg_event_bb_analysis) ||
        !drmgr_unregister_restore_state_ex_event(drreg_event_restore_state))
        return DRREG_ERROR;

    drmgr_exit();

    /* +1 for the pointer to the indirect spill block, see above. */
    if (!dr_raw_tls_cfree(tls_simd_offs, ops.num_spill_slots + 1))
        return DRREG_ERROR;

    /* Support re-attach */
    memset(&ops, 0, sizeof(ops));

    return DRREG_SUCCESS;
}
