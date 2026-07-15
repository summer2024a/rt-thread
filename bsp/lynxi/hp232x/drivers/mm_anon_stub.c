/*
 * HP232X MM minimal: stubs for private-mapping APIs removed with mm_anon.c.
 * hp232x uses identity-mapped hp232x_mmu; COW/private vareas are never used.
 */
#include <rtthread.h>
#include "mm_private.h"

rt_err_t rt_aspace_anon_ref_dec(rt_mem_obj_t aobj)
{
    (void)aobj;
    return RT_EOK;
}

int rt_varea_fix_private_locked(rt_varea_t ex_varea, void *pa,
                                struct rt_aspace_fault_msg *msg,
                                rt_bool_t dont_copy)
{
    (void)ex_varea;
    (void)pa;
    (void)msg;
    (void)dont_copy;
    return MM_FAULT_FIXABLE_FALSE;
}
