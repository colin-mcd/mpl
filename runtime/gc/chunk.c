/* Copyright (C) 2015 Ram Raghunathan.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

/**
 * @file chunk.c
 *
 * @author Ram Raghunathan
 *
 * This file implements the management interface defined in chunk.h
 */

#include "chunk.h"

/******************************/
/* Static Function Prototypes */
/******************************/
/**
 * This function appends 'chunkList' to 'destinationChunkList'.
 *
 * @note
 * 'chunkList's level head is demoted to a regular chunk and its level is
 * switched to 'destinationChunkList's
 *
 * @param destinationChunkList The head of the chunk list to append to. Must be
 * the full level chunk list.
 * @param chunkList the chunk list to append. Must be a full level chunk list,
 * <em>not</em> in a level list
 */
static void appendChunkList(void* destinationChunkList, void* chunkList);

#if ASSERT
/**
 * This function asserts the chunk invariants
 *
 * @attention
 * If an assertion fails, this function aborts the program, as per the assert()
 * macro.
 *
 * @param chunk The chunk to assert invariants for.
 * @param levelHeadChunk The head chunk of the level 'chunk' belongs to
 */
static void HM_assertChunkInvariants(const void* chunk,
                                     const void* levelHeadChunk);

/**
 * This function asserts the chunk list invariants
 *
 * @attention
 * If an assertion fails, this function aborts the program, as per the assert()
 * macro.
 *
 * @param chunkList The chunk list to assert invariants for.
 */
static void HM_assertChunkListInvariants(const void* chunkList);
#endif /* ASSERT */

/**
 * This function retrieves the ChunkInfo object from a chunk
 *
 * @param chunk The chunk to retrieve the object from
 *
 * @return the ChunkInfo struct pointer
 */
static struct HM_ChunkInfo* getChunkInfo(void* chunk);

/**
 * Same as getChunkInfo() except for const-correctness. See getChunkInfo() for
 * more details
 */
static const struct HM_ChunkInfo* getChunkInfoConst(const void* chunk);

/**
 * Gets the level's head chunk for a given chunk.
 *
 * @param chunk The chunk to get the level head chunk for
 *
 * @return the head chunk of the level 'chunk' belongs to
 */
static void* getLevelHeadChunk(void* chunk);

/**
 * Same as getLevelHeadChunk() except for const-correctness. See
 * getLevelHeadChunk() for more details.
 */
static const void* getLevelHeadChunkConst(const void* chunk);

/************************/
/* Function Definitions */
/************************/
#if (defined (MLTON_GC_INTERNAL_FUNCS))
void* HM_allocateChunk(void* levelHeadChunk,
                       void** chunkEnd,
                       size_t allocableSize) {
  size_t totalSize = allocableSize + sizeof(struct HM_ChunkInfo);
  void* chunk = ChunkPool_allocate(&totalSize);

  if (NULL == chunk) {
    return NULL;
  }

  struct HM_ChunkInfo* chunkInfo = getChunkInfo(chunk);
  chunkInfo->frontier = HM_getChunkStart(chunk);
  chunkInfo->level = CHUNK_INVALID_LEVEL;
  chunkInfo->split.normal.levelHead = levelHeadChunk;
  chunkInfo->split.levelHead.containingHH = NULL;

  /* insert into list */
  chunkInfo->nextChunk = NULL;
  if (NULL == getChunkInfo(levelHeadChunk)->nextChunk) {
    /* empty list */
    getChunkInfo(levelHeadChunk)->nextChunk = chunk;
    getChunkInfo(levelHeadChunk)->split.levelHead.lastChunk = chunk;
  } else {
    void* lastChunk = getChunkInfo(levelHeadChunk)->split.levelHead.lastChunk;
    getChunkInfo(lastChunk)->nextChunk = chunk;
    getChunkInfo(levelHeadChunk)->split.levelHead.lastChunk = chunk;
  }

  /* populate chunkEnd */
  *chunkEnd = ((void*)(((char*)(chunk)) + totalSize));

  return chunk;
}

void* HM_allocateLevelHeadChunk(void** levelList,
                                void** chunkEnd,
                                size_t allocableSize,
                                Word32 level,
                                struct HM_HierarchicalHeap* hh) {
  size_t totalSize = allocableSize + sizeof(struct HM_ChunkInfo);
  void* chunk = ChunkPool_allocate(&totalSize);

  if (NULL == chunk) {
    return NULL;
  }

  /* setup chunk info */
  struct HM_ChunkInfo* chunkInfo = getChunkInfo(chunk);
  chunkInfo->frontier = HM_getChunkStart(chunk);
  chunkInfo->nextChunk = NULL;
  chunkInfo->level = level;
  chunkInfo->split.levelHead.nextHead = NULL;
  chunkInfo->split.levelHead.lastChunk = chunk;
  chunkInfo->split.levelHead.containingHH = hh;

  /* insert into level list */
  HM_mergeLevelList(levelList, chunk);

  /* populate chunkEnd */
  *chunkEnd = ((void*)(((char*)(chunk)) + totalSize));

  return chunk;
}

void HM_foreachHHObjptrInLevelList(GC_state s,
                                   void** destinationLevelList,
                                   HHObjptrFunction f,
                                   struct HM_HierarchicalHeap* hh,
                                   size_t minLevel) {
  struct HHObjptrFunctionArgs fArgs = {
    .destinationLevelList = destinationLevelList,
    .hh = hh,
    .minLevel = minLevel,
    .maxLevel = 0
  };

  for (void* levelList = *destinationLevelList;
       NULL != levelList;
       levelList = getChunkInfo(levelList)->split.levelHead.nextHead) {
    for (void* chunk = levelList;
         NULL != chunk;
         chunk = getChunkInfo(chunk)->nextChunk) {
#pragma message "Optimize by saving frontier?"
      for (pointer p = HM_getChunkStart(chunk);
           p != getChunkInfo(chunk)->frontier;) {
        LOCAL_USED_FOR_ASSERT void* savedLevelList = *destinationLevelList;

        p = advanceToObjectData(s, p);
        fArgs.maxLevel = getChunkInfo(levelList)->level;
        p = HM_HHC_foreachHHObjptrInObject(s,
                                           p,
                                           FALSE,
                                           f,
                                           &fArgs);

        assert(savedLevelList == *destinationLevelList);
      }
    }
  }
}

void HM_freeChunks(void** levelList, Word32 minLevel) {
  for (void* chunkList = *levelList;
       (NULL != chunkList) && (getChunkInfo(chunkList)->level >= minLevel);
       chunkList = *levelList) {
    assert(CHUNK_INVALID_LEVEL != getChunkInfo(chunkList)->level);

    /* unlink chunkList */
    *levelList = getChunkInfo(chunkList)->split.levelHead.nextHead;

    /* free everything in chunkList */
    for(void* chunk = chunkList;
        NULL != chunk;
        chunk = chunkList) {
      /* unlink chunk */
      chunkList = getChunkInfo(chunkList)->nextChunk;

      /* free the chunk */
      ChunkPool_free(chunk);
    }
  }
}

void* HM_getChunkFrontier(void* chunk) {
  return getChunkInfo(chunk)->frontier;
}

void* HM_getChunkLimit(void* chunk) {
  size_t size = ChunkPool_size(chunk);
  assert(0 != size);

  return ((void*)(((char*)(chunk)) + size));
}

void* HM_getChunkStart(void* chunk) {
  return ((void*)(((char*)(chunk)) + sizeof(struct HM_ChunkInfo)));
}

Word32 HM_getChunkListLevel(void* chunkList) {
  assert(CHUNK_INVALID_LEVEL != getChunkInfo(chunkList)->level);
  return ((Word32)(getChunkInfo(chunkList)->level));
}

void* HM_getChunkListLastChunk(void* chunkList) {
  if (NULL == chunkList) {
    return NULL;
  }

  assert(CHUNK_INVALID_LEVEL != getChunkInfo(chunkList)->level);
  return getChunkInfo(chunkList)->split.levelHead.lastChunk;
}

struct HM_HierarchicalHeap* HM_getContainingHierarchicalHeap(objptr object) {
  void* chunk = ChunkPool_find(objptrToPointer(object, 0));
  void* levelHeadChunk;
  for(levelHeadChunk = chunk;
      CHUNK_INVALID_LEVEL == getChunkInfo(levelHeadChunk)->level;
      levelHeadChunk = getChunkInfo(levelHeadChunk)->split.normal.levelHead) {
  }

  return getChunkInfo(levelHeadChunk)->split.levelHead.containingHH;
}

size_t HM_getHighestLevel(const void* levelList) {
  if (NULL == levelList) {
    return CHUNK_INVALID_LEVEL;
  }

  assert(CHUNK_INVALID_LEVEL != getChunkInfoConst(levelList)->level);
  return (getChunkInfoConst(levelList)->level);
}

Word32 HM_getObjptrLevel(GC_state s, objptr object) {
  void* chunk = ChunkPool_find(objptrToPointer(object, s->heap->start));
  void* levelHeadChunk;
  for(levelHeadChunk = chunk;
      CHUNK_INVALID_LEVEL == getChunkInfo(levelHeadChunk)->level;
      levelHeadChunk = getChunkInfo(levelHeadChunk)->split.normal.levelHead) {
  }

  return getChunkInfo(levelHeadChunk)->level;
}

void HM_mergeLevelList(void** destinationLevelList, void* levelList) {
  void* newLevelList = NULL;

  /* construct newLevelList */
  {
    void** previousChunkList = &newLevelList;
    void* cursor1 = *destinationLevelList;
    void* cursor2 = levelList;
    while ((NULL != cursor1) && (NULL != cursor2)) {
      size_t level1 = getChunkInfo(cursor1)->level;
      size_t level2 = getChunkInfo(cursor2)->level;
      assert(CHUNK_INVALID_LEVEL != level1);
      assert(CHUNK_INVALID_LEVEL != level2);

      if (level1 > level2) {
        /* append the first list */
        *previousChunkList = cursor1;

        /* advance cursor1 */
        cursor1 = getChunkInfo(cursor1)->split.levelHead.nextHead;
      } else if (level1 < level2) {
        /* append the second list */
        *previousChunkList = cursor2;

        /* advance cursor2 */
        cursor2 = getChunkInfo(cursor2)->split.levelHead.nextHead;
      } else {
        /* level1 == level2 */
        /* advance cursor 2 early since appendChunkList will unlink it */
        void* savedCursor2 = cursor2;
        cursor2 = getChunkInfo(cursor2)->split.levelHead.nextHead;

        /* merge second list into first before inserting */
        appendChunkList(cursor1, savedCursor2);

        /* append the first list */
        *previousChunkList = cursor1;

        /* advance cursor1 */
        cursor1 = getChunkInfo(cursor1)->split.levelHead.nextHead;
      }

      /* advance previousChunkList */
      previousChunkList =
          &(getChunkInfo(*previousChunkList)->split.levelHead.nextHead);
    }

    if (NULL != cursor1) {
      assert(NULL == cursor2);

      /* append the remainder of cursor1 */
      *previousChunkList = cursor1;
    } else if (NULL != cursor2) {
      assert(NULL == cursor1);

      /* append the remainder of cursor2 */
      *previousChunkList = cursor2;
    }
  }

  /* update destinationChunkList */
  *destinationLevelList = newLevelList;
}

void HM_promoteChunks(void** levelList, size_t level) {
  /* find the level  1 list */
  void** cursor;
  for (cursor = levelList;

#if ASSERT
       (NULL != *cursor) &&
#endif
                (getChunkInfo(*cursor)->level > level);

       cursor = &(getChunkInfo(*cursor)->split.levelHead.nextHead)) {
    assert(CHUNK_INVALID_LEVEL != getChunkInfo(*cursor)->level);
  }
  assert(CHUNK_INVALID_LEVEL != getChunkInfo(*cursor)->level);

  assert(NULL != *cursor);
  if (getChunkInfo(*cursor)->level < level) {
    /* no chunks to promote */
    return;
  }

  void* chunkList = *cursor;
  /* unlink level list */
  *cursor = getChunkInfo(chunkList)->split.levelHead.nextHead;

  if ((NULL != *cursor) && (level - 1 == getChunkInfo(*cursor)->level)) {
    /* need to merge into cursor */
    appendChunkList(*cursor, chunkList);
  } else {
    /* need to reassign levelList to level - 1 */
    assert((NULL == *cursor) || (level - 1 > getChunkInfo(*cursor)->level));
    getChunkInfo(chunkList)->level = level - 1;

    /* insert chunkList where *cursor is */
    getChunkInfo(chunkList)->split.levelHead.nextHead = *cursor;
    *cursor = chunkList;
  }
}

#if ASSERT
void HM_assertLevelListInvariants(const void* levelList) {
  size_t lastLevel = ~((size_t)(0LL));
  for (const void* chunkList = levelList;
       NULL != chunkList;
       chunkList = getChunkInfoConst(chunkList)->split.levelHead.nextHead) {
    size_t level = getChunkInfoConst(chunkList)->level;
    assert(CHUNK_INVALID_LEVEL != level);
    assert(level < lastLevel);
    lastLevel = level;

    HM_assertChunkListInvariants(chunkList);
  }
}
#else
void HM_assertLevelListInvariants(const void* levelList) {
  ((void)(levelList));
}
#endif /* ASSERT */

void HM_updateChunkValues(void* chunk, void* frontier) {
  assert(ChunkPool_find(((char*)(frontier)) - 1) == chunk);
  getChunkInfo(chunk)->frontier = frontier;
}

void HM_updateLevelListPointers(void* levelList,
                                struct HM_HierarchicalHeap* hh) {
  for (void* cursor = levelList;
       NULL != cursor;
       cursor = getChunkInfo(cursor)->split.levelHead.nextHead) {
    getChunkInfo(cursor)->split.levelHead.containingHH = hh;
  }
}
#endif /* MLTON_GC_INTERNAL_FUNCS */

void appendChunkList(void* destinationChunkList, void* chunkList) {
  assert (NULL != destinationChunkList);
  assert(CHUNK_INVALID_LEVEL != getChunkInfo(destinationChunkList)->level);
  assert(CHUNK_INVALID_LEVEL != getChunkInfo(chunkList)->level);

  if (NULL == chunkList) {
    /* nothing to append */
    return;
  }

  /* append list */
  void* lastDestinationChunk = HM_getChunkListLastChunk(destinationChunkList);
  assert(NULL == getChunkInfo(lastDestinationChunk)->nextChunk);
  getChunkInfo(lastDestinationChunk)->nextChunk = chunkList;

  /* update pointers */
  void* lastChunk = HM_getChunkListLastChunk(chunkList);
  getChunkInfo(destinationChunkList)->split.levelHead.lastChunk = lastChunk;

  /* demote chunkList's level head chunk */
  getChunkInfo(chunkList)->level = CHUNK_INVALID_LEVEL;
  getChunkInfo(chunkList)->split.normal.levelHead = destinationChunkList;
}

#if ASSERT
void HM_assertChunkInvariants(const void* chunk, const void* levelHeadChunk) {
  const struct HM_ChunkInfo* chunkInfo = getChunkInfoConst(chunk);

  assert(ChunkPool_find(((char*)(chunkInfo->frontier)) - 1) == chunk);

  if (chunk == levelHeadChunk) {
    /* this is the level head chunk */
    assert(CHUNK_INVALID_LEVEL != chunkInfo->level);
    assert(NULL != chunkInfo->split.levelHead.containingHH);
  } else {
    /* this is a normal chunk */
    assert(CHUNK_INVALID_LEVEL == chunkInfo->level);
  }

  assert(levelHeadChunk == getLevelHeadChunkConst(chunk));
}

void HM_assertChunkListInvariants(const void* chunkList) {
  for (const void* chunk = chunkList;
       NULL != chunk;
       chunk = getChunkInfoConst(chunk)->nextChunk) {
    HM_assertChunkInvariants(chunk, chunkList);
  }
}
#endif /* ASSERT */

struct HM_ChunkInfo* getChunkInfo(void* chunk) {
  return ((struct HM_ChunkInfo*)(chunk));
}

const struct HM_ChunkInfo* getChunkInfoConst(const void* chunk) {
  return ((const struct HM_ChunkInfo*)(chunk));
}

void* getLevelHeadChunk(void* chunk) {
  void* cursor;
  for (cursor = chunk;
#if ASSERT
       (NULL != cursor) &&
#endif
                CHUNK_INVALID_LEVEL == getChunkInfo(cursor)->level;
       cursor = getChunkInfo(cursor)->split.normal.levelHead) {
  }
  assert(NULL != cursor);

  return cursor;
}

const void* getLevelHeadChunkConst(const void* chunk) {
  const void* cursor;
  for (cursor = chunk;
#if ASSERT
       (NULL != cursor) &&
#endif
                CHUNK_INVALID_LEVEL == getChunkInfoConst(cursor)->level;
       cursor = getChunkInfoConst(cursor)->split.normal.levelHead) {
  }
  assert(NULL != cursor);

  return cursor;
}
