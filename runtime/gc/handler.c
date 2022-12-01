/* Copyright (C) 2019 Matthew Fluet.
 * Copyright (C) 1999-2007 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 */

/* GC_startSignalHandler does not do an enter()/leave(), even though
 * it is exported.  The basis library uses it via _import, not _prim,
 * and so does not treat it as a runtime call -- so the invariant in
 * enter would fail miserably.  It is OK because GC_startHandler must
 * be called from within a critical section.
 *
 * Don't make it inline, because it is also called in basis/Thread.c,
 * and when compiling with COMPILE_FAST, they may appear out of order.
 */
void GC_startSignalHandler (GC_state s) {
  /* Switch to the signal handler thread. */
  if (DEBUG_SIGNALS) {
    fprintf (stderr, "GC_startSignalHandler [%d]\n",
             Proc_processorNumber (s));
  }
  assert (s->atomicState == 1);
  assert (s->signalsInfo.signalIsPending);
  s->signalsInfo.signalIsPending = FALSE;
  s->signalsInfo.amInSignalHandler = TRUE;
  assert (s->savedThread == BOGUS_OBJPTR);
  s->savedThread = s->currentThread;
  /* Set s->atomicState to 2 when switching to the signal handler
   * thread; leaving the runtime will decrement s->atomicState to 1,
   * the signal handler will then run atomically and will finish by
   * switching to the thread to continue with, which will decrement
   * s->atomicState to 0.
   */
  s->atomicState = 2;
}

void GC_finishSignalHandler (GC_state s) {
  if (DEBUG_SIGNALS)
    fprintf (stderr, "GC_finishSignalHandler () [%d]\n",
             Proc_processorNumber (s));
  assert (s->atomicState == 1);
  s->signalsInfo.amInSignalHandler = FALSE;
}

void switchToSignalHandlerThreadIfNonAtomicAndSignalPending (GC_state s) {
  if (s->atomicState == 1
      and s->signalsInfo.signalIsPending) {
    // printf("switchToSignalHandlerThread triggered...\n");
    GC_startSignalHandler (s);

    // printf("[%d] switchToThread\n  from %p\n    to %p (signal handler thread)\n",
    //   s->procNumber,
    //   (void*)getThreadCurrent(s),
    //   (void*)threadObjptrToStruct(s, s->signalHandlerThread));

    // SAM_NOTE: synchronizes with loop in switchToThread...
    atomicStoreS32(&(getThreadCurrent(s)->currentProcNum), -1);
    s->currentThread = BOGUS_OBJPTR;

    switchToThread (s, s->signalHandlerThread);
  }
}

static inline void relaySignalTo(GC_state s, int id, int signum) {
  if (id == Proc_processorNumber(s))
    return;

  // first, try to prevent them from terminating
  uint32_t *statusp = &(s->procStates[id].terminationStatus);
  uint32_t status = atomicLoadU32(statusp);
  bool success = FALSE;
  while (status > 0 && !GC_CheckForTerminationRequest(s)) {
    success = __sync_bool_compare_and_swap(statusp, status, status+1);
    if (success)
      break;
    status = atomicLoadU32(statusp);
  }

  if (success) {
    assert(atomicLoadU32(statusp) >= 2);
    pthread_kill(s->procStates[id].self, signum);
    assert(atomicLoadU32(statusp) >= 2);
    __sync_fetch_and_sub(statusp, 1);
  }
}

/* GC_handler sets s->limit = 0 so that the next limit check will
 * fail.  Signals need to be blocked during the handler (i.e. it
 * should run atomically) because sigaddset does both a read and a
 * write of s->signalsInfo.signalsPending.  The signals are blocked
 * by Posix_Signal_handle (see Posix/Signal/Signal.c).
 */
void GC_handler (int signum) {
  GC_state s = MLton_gcState ();
  if (NULL == s) {
    // possible because of race between handler and teardown??
    return;
  }

  if (DEBUG_SIGNALS)
    fprintf (stderr, "GC_handler signum = %d [%d]\n", signum,
             Proc_processorNumber (s));

#if ASSERT
  // Signal disposition is per-process; use primary to maintain handled set.

  // This isn't safe in general because of termination protocol, so we can
  // only safely check it if we are proc 0...
  if (Proc_processorNumber(s) == 0) {
    assert (sigismember (&s->procStates[0].signalsInfo.signalsHandled, signum));
  }
#endif

  if (s->atomicState == 0)
    s->limit = 0;
  s->signalsInfo.signalIsPending = TRUE;
  sigaddset (&s->signalsInfo.signalsPending, signum);

  int me = Proc_processorNumber(s);

  if (signum == SIGALRM) {
    if (me != 0) {
      relaySignalTo(s, 0, SIGALRM);
    }
    else {
      for (uint32_t p = 1;
           p < s->numberOfProcs && !GC_CheckForTerminationRequest(s);
           p++)
      {
        relaySignalTo(s, p, SIGUSR1);
      }
    }
  }

  /* some old code below: this implements a relay tree, where each processor
   * relays to its children. relayCount controls how many children each
   * processor has, and we build a full n-ary tree with processor 0 at the
   * root. This would need to be called if a processor receives SIGUSR1, and
   * requires that the SIGALRM is always immediately relayed to the root.
   * The root would call this code when it receives SIGALRM.
   */
  // int relayCount = 16;
  // uint32_t start = relayCount * me + 1;
  // uint32_t stop = relayCount * me + relayCount;
  // for (uint32_t p = start;
  //       p <= stop && p < s->numberOfProcs && !GC_CheckForTerminationRequest(s);
  //       p++)
  // {
  //   relaySignalTo(s, p, SIGUSR1);
  // }


  // if (signum == SIGUSR1) {
  //   printf("[%d] received relay\n", Proc_processorNumber(s));
  // }

  if (DEBUG_SIGNALS)
    fprintf (stderr, "GC_handler done [%d]\n",
             Proc_processorNumber (s));
}

pointer GC_handlerEnterHeapOfThread(GC_state s, objptr threadp) {
  enter(s);

  GC_thread target = threadObjptrToStruct(s, threadp);
  assert(getThreadCurrent(s)->currentDepth == 0);
  assert(target->currentProcNum == -1);

  HM_HierarchicalHeap abandonedHH = getThreadCurrent(s)->hierarchicalHeap;

  // copy thread details to current thread
  getThreadCurrent(s)->currentDepth = target->currentDepth;
  getThreadCurrent(s)->currentChunk = target->currentChunk;
  getThreadCurrent(s)->hierarchicalHeap = target->hierarchicalHeap;
  getThreadCurrent(s)->bytesAllocatedSinceLastCollection =
    target->bytesAllocatedSinceLastCollection;
  getThreadCurrent(s)->bytesSurvivedLastCollection =
    target->bytesSurvivedLastCollection;
  getThreadCurrent(s)->minLocalCollectionDepth = target->minLocalCollectionDepth;

#ifdef DETECT_ENTANGLEMENT
  getThreadCurrent(s)->decheckState = target->decheckState;
  uint32_t* fromSyncDepths = &(target->decheckSyncDepths[0]);
  uint32_t* toSyncDepths = &(getThreadCurrent(s)->decheckSyncDepths[0]);
  memcpy(toSyncDepths, fromSyncDepths, DECHECK_DEPTHS_LEN * sizeof(uint32_t));
#endif

  // clear out some stuff for my sanity
  target->currentChunk = NULL;
  target->hierarchicalHeap = NULL;
  target->bytesAllocatedSinceLastCollection = 0;
  target->bytesSurvivedLastCollection = 0;
#ifdef DETECT_ENTANGLEMENT
  target->decheckState = DECHECK_BOGUS_TID;
#endif

  s->frontier = HM_HH_getFrontier(getThreadCurrent(s));
  s->limitPlusSlop = HM_HH_getLimit(getThreadCurrent(s));
  s->limit = s->limitPlusSlop - GC_HEAP_LIMIT_SLOP;

  s->savedThreadDuringSignalHandler = threadp;

  HM_ensureHierarchicalHeapAssurances(
    s,
    FALSE,
    getThreadCurrent(s)->bytesNeeded,
    FALSE);

  assert(invariantForMutatorFrontier(s));
  assert(invariantForMutatorStack(s));
  leave(s);
  return (void*)abandonedHH;
}


void GC_handlerLeaveHeapOfThread(
  GC_state s,
  objptr threadp,
  pointer abandonedHH)
{
  enter(s);

  GC_thread target = threadObjptrToStruct(s, threadp);
  assert(target->currentProcNum == -1);
  assert(s->savedThreadDuringSignalHandler == threadp);

  target->currentDepth = getThreadCurrent(s)->currentDepth;
  target->currentChunk = getThreadCurrent(s)->currentChunk;
  target->hierarchicalHeap = getThreadCurrent(s)->hierarchicalHeap;
  target->bytesAllocatedSinceLastCollection =
    getThreadCurrent(s)->bytesAllocatedSinceLastCollection;
  target->bytesSurvivedLastCollection =
    getThreadCurrent(s)->bytesSurvivedLastCollection;
  target->minLocalCollectionDepth = getThreadCurrent(s)->minLocalCollectionDepth;

#ifdef DETECT_ENTANGLEMENT
  target->decheckState = getThreadCurrent(s)->decheckState;
  uint32_t* fromSyncDepths = &(getThreadCurrent(s)->decheckSyncDepths[0]);
  uint32_t* toSyncDepths = &(target->decheckSyncDepths[0]);
  memcpy(toSyncDepths, fromSyncDepths, DECHECK_DEPTHS_LEN * sizeof(uint32_t));
#endif

  HM_HierarchicalHeap originalHH = (HM_HierarchicalHeap)abandonedHH;
  assert(HM_HH_getDepth(originalHH) == 0);

  getThreadCurrent(s)->currentDepth = 0;
  getThreadCurrent(s)->hierarchicalHeap = originalHH;
  getThreadCurrent(s)->bytesAllocatedSinceLastCollection = 0;
  getThreadCurrent(s)->bytesSurvivedLastCollection = 0;
#ifdef DETECT_ENTANGLEMENT
  getThreadCurrent(s)->decheckState = DECHECK_BOGUS_TID;
#endif

  // Find a chunk to get back to
  HM_chunk chunk = HM_getChunkListLastChunk(HM_HH_getChunkList(originalHH));
  assert(NULL != chunk);
  getThreadCurrent(s)->currentChunk = chunk;

  if (!chunk->mightContainMultipleObjects) {
    if (!HM_HH_extend(s, getThreadCurrent(s), GC_HEAP_LIMIT_SLOP)) {
      DIE("Ran out of space for Hierarchical Heap!");
    }
  }

  assert(NULL != getThreadCurrent(s)->currentChunk);
  assert(getThreadCurrent(s)->currentChunk->mightContainMultipleObjects);

  s->frontier = HM_HH_getFrontier(getThreadCurrent(s));
  s->limitPlusSlop = HM_HH_getLimit(getThreadCurrent(s));
  s->limit = s->limitPlusSlop - GC_HEAP_LIMIT_SLOP;

  s->savedThreadDuringSignalHandler = BOGUS_OBJPTR;

  HM_ensureHierarchicalHeapAssurances(
    s,
    FALSE,
    getThreadCurrent(s)->bytesNeeded,
    FALSE);

  assert(invariantForMutatorFrontier(s));
  assert(invariantForMutatorStack(s));
  leave(s);
}
