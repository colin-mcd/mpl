/* Copyright (C) 1999-2007 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

void switchToThread (GC_state s, objptr op) {
  if (DEBUG_THREADS) {
    GC_thread thread;
    GC_stack stack;

    thread = (GC_thread)(objptrToPointer (op, s->heap->start)
                         + offsetofThread (s));
    stack = (GC_stack)(objptrToPointer (thread->stack, s->heap->start));

    fprintf (stderr, "switchToThread ("FMTOBJPTR")  used = %"PRIuMAX
             "  reserved = %"PRIuMAX"\n",
             op, (uintmax_t)stack->used, (uintmax_t)stack->reserved);
  }

  s->currentThread = op;
  setGCStateCurrentThreadAndStack (s);
}

void GC_switchToThread (GC_state s, pointer p, size_t ensureBytesFree) {
  pointer currentP = objptrToPointer(getThreadCurrentObjptr(s), s->heap->start);
  LOG (LM_THREAD, LL_DEBUG,
       "current = "FMTPTR", p = "FMTPTR", ensureBytesFree = %zu)",
       ((uintptr_t)(currentP)),
       ((uintptr_t)(p)),
       ensureBytesFree);

  /* RAM_NOTE: Switch to other branch when I can */
  if (TRUE) {
    /* This branch is slower than the else branch, especially
     * when debugging is turned on, because it does an invariant
     * check on every thread switch.
     * So, we'll stick with the else branch for now.
     */
    //ENTER1 (s, p);
    /* SPOONHOWER_NOTE: copied from enter() */
    /* used needs to be set because the mutator has changed s->stackTop. */
    getStackCurrent(s)->used = sizeofGCStateCurrentStackUsed (s);
    getThreadCurrent(s)->exnStack = s->exnStack;
    beginAtomic (s);

    if (!HM_inGlobalHeap(s)) {
      /* save HH info for from-thread */
      /* copied from HM_enterGlobalHeap() */
      HM_exitLocalHeap(s); // remembers s->frontier in HH

      spinlock_lock(&(s->lock), Proc_processorNumber(s));
      s->frontier = s->globalFrontier;
      s->limitPlusSlop = s->globalLimitPlusSlop;
      s->limit = s->limitPlusSlop - GC_HEAP_LIMIT_SLOP;
      spinlock_unlock(&(s->lock));
    }

    /*
     * at this point, the processor is in the global heap, but the thread is
     * not.
     */

    getThreadCurrent(s)->bytesNeeded = ensureBytesFree;
// #if ASSERT
//     // mark the current thread as no longer being executed
//     getThreadCurrent(s)->currentProcNum = -1;
// #endif
    switchToThread (s, pointerToObjptr(p, s->heap->start));

// #if ASSERT
//     int priorOwner = __sync_val_compare_and_swap(&(getThreadCurrent(s)->currentProcNum), -1, s->procNumber);
//     if (priorOwner != -1) {
//       DIE("Proc %d failed to claim thread "FMTPTR" which is owned by proc %d",
//         s->procNumber,
//         (uintptr_t)getThreadCurrentObjptr(s),
//         priorOwner);
//     }
// #endif

    if (!HM_inGlobalHeap(s)) {
      /* I need to switch to the HH for the to-thread */
      /* copied from HM_exitGlobalHeap() */
      spinlock_lock(&(s->lock), Proc_processorNumber(s));
      s->globalFrontier = s->frontier;
      s->globalLimitPlusSlop = s->limitPlusSlop;
      HM_enterLocalHeap (s);
      spinlock_unlock(&(s->lock));
    }

    s->atomicState--;
    switchToSignalHandlerThreadIfNonAtomicAndSignalPending (s);
    if (HM_inGlobalHeap(s)) {
      ensureStackInvariantInGlobal(s);
      ensureBytesFreeInGlobal(s, getThreadCurrent(s)->bytesNeeded);
    } else {
      HM_ensureHierarchicalHeapAssurances (s, FALSE, 0, FALSE);
    }

    endAtomic (s);
    assert (strongInvariantForMutatorFrontier(s));
    assert (invariantForMutatorStack(s));
    //LEAVE0 (s);
  } else {
    /* RAM_NOTE: Why? It looks exactly the same... */
    assert (false and "unsafe in a multiprocessor context");
    /* BEGIN: enter(s); */
    getStackCurrent(s)->used = sizeofGCStateCurrentStackUsed (s);
    getThreadCurrent(s)->exnStack = s->exnStack;
    beginAtomic (s);
    /* END: enter(s); */
    getThreadCurrent(s)->bytesNeeded = ensureBytesFree;
    switchToThread (s, pointerToObjptr(p, s->heap->start));
    s->atomicState--;
    switchToSignalHandlerThreadIfNonAtomicAndSignalPending (s);
    /* BEGIN: ensureInvariantForMutator */
    if (not (invariantForMutatorFrontier(s))
        or not (invariantForMutatorStack(s))) {
      /* This GC will grow the stack, if necessary. */
      performGC (s, 0, getThreadCurrent(s)->bytesNeeded, FALSE, TRUE);
    }
    /* END: ensureInvariantForMutator */
    /* BEGIN: leave(s); */
    endAtomic (s);
    /* END: leave(s); */
    assert (invariantForMutatorFrontier(s));
    assert (invariantForMutatorStack(s));
  }
}
