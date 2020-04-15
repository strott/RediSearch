
#define RS_GC_C_

#include <math.h>
#include <assert.h>
#include <sys/param.h>
#include "inverted_index.h"
#include "redis_index.h"
#include "gc.h"
#include "redismodule.h"
#include "rmutil/util.h"
#include "default_gc.h"
#include "tests/time_sample.h"
#include "numeric_index.h"
#include "tag_index.h"
#include "config.h"
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>

// convert a frequency to timespec
struct timespec hzToTimeSpec(float hz) {
  struct timespec ret;
  ret.tv_sec = (time_t)floor(1.0 / hz);
  ret.tv_nsec = (long)floor(1000000000.0 / hz) % 1000000000L;
  return ret;
}

typedef struct NumericFieldGCCtx {
  NumericRangeTree *rt;
  uint32_t revisionId;
  NumericRangeTreeIterator *gcIterator;
} NumericFieldGCCtx;

#define NUMERIC_GC_INITIAL_SIZE 4

#define SPEC_STATUS_OK 1
#define SPEC_STATUS_INVALID 2

/* Internal definition of the garbage collector context (each index has one) */
struct GarbageCollectorCtx {
  IndexSpec *sp;
  // current frequency
  float hz;

  // statistics for reporting
  GCStats stats;

  NumericFieldGCCtx **numericGCCtx;

  bool noLockMode;
};

/* Create a new garbage collector, with a string for the index name, and initial frequency */
GarbageCollectorCtx *NewGarbageCollector(IndexSpec *sp, float initialHZ, uint64_t specUniqueId,
                                         GCCallbacks *callbacks) {
  GarbageCollectorCtx *gcCtx = rm_malloc(sizeof(*gcCtx));

  *gcCtx = (GarbageCollectorCtx){
      .sp = sp,
      .hz = initialHZ,
      .stats = {0},
      .noLockMode = false,
      .numericGCCtx = array_new(NumericFieldGCCtx *, NUMERIC_GC_INITIAL_SIZE),
  };

  callbacks->onDelete = GC_OnDelete;
  callbacks->onTerm = GC_OnTerm;
  callbacks->periodicCallback = GC_PeriodicCallback;
  callbacks->renderStats = GC_RenderStats;
  callbacks->getInterval = GC_GetInterval;

  return gcCtx;
}

void gc_updateStats(GarbageCollectorCtx *gc, size_t recordsRemoved, size_t bytesCollected) {
  gc->sp->stats.numRecords -= recordsRemoved;
  gc->sp->stats.invertedSize -= bytesCollected;
  gc->stats.totalCollected += bytesCollected;
}

size_t gc_RandomTerm(RedisModuleCtx *ctx, GarbageCollectorCtx *gc, int *status) {
  IndexSpec *sp = gc->sp;
  RedisSearchCtx sctx = SEARCH_CTX_STATIC(ctx, gc->sp);
  size_t totalRemoved = 0;
  size_t totalCollected = 0;
  // Select a weighted random term
  TimeSample ts;
  char *term = IndexSpec_GetRandomTerm(gc->sp, 20);
  // if the index is empty we won't get anything here
  if (!term) {
    goto end;
  }
  RedisModule_Log(ctx, "debug", "Garbage collecting for term '%s'", term);
  // Open the term's index
  InvertedIndex *idx = IDX_LoadTerm(sp, term, strlen(term), REDISMODULE_WRITE);
  if (idx) {
    int blockNum = 0;
    while (1) {
      IndexRepairParams params = {.limit = RSGlobalConfig.gcScanSize};
      TimeSampler_Start(&ts);
      // repair 100 blocks at once
      blockNum = InvertedIndex_Repair(idx, &sp->docs, blockNum, &params);
      TimeSampler_End(&ts);
      RedisModule_Log(ctx, "debug", "Repair took %lldns", TimeSampler_DurationNS(&ts));
      /// update the statistics with the the number of records deleted
      totalRemoved += params.docsCollected;
      gc_updateStats(gc, params.docsCollected, params.bytesCollected);
      totalCollected += params.bytesCollected;
      // blockNum 0 means error or we've finished
      if (!blockNum) break;

      // After each iteration we yield execution
      if (!IDX_YieldWrite(gc->sp)) {
        *status = SPEC_STATUS_INVALID;
        break;
      }
    }
  }
  if (totalRemoved) {
    RedisModule_Log(ctx, "debug", "Garbage collected %zd bytes in %zd records for term '%s'",
                    totalCollected, totalRemoved, term);
  }
  rm_free(term);
  RedisModule_Log(ctx, "debug", "New HZ: %f\n", gc->hz);
end:
  return totalRemoved;
}

static NumericRangeNode *NextGcNode(NumericFieldGCCtx *numericGcCtx) {
  bool runFromStart = false;
  NumericRangeNode *node = NULL;
  do {
    while ((node = NumericRangeTreeIterator_Next(numericGcCtx->gcIterator))) {
      if (node->range) {
        return node;
      }
    }
    assert(!runFromStart);
    NumericRangeTreeIterator_Free(numericGcCtx->gcIterator);
    numericGcCtx->gcIterator = NumericRangeTreeIterator_New(numericGcCtx->rt);
    runFromStart = true;
  } while (true);

  // will never reach here
  return NULL;
}

static NumericFieldGCCtx *gc_NewNumericGcCtx(NumericRangeTree *rt) {
  NumericFieldGCCtx *ctx = rm_malloc(sizeof(NumericFieldGCCtx));
  ctx->rt = rt;
  ctx->revisionId = rt->revisionId;
  ctx->gcIterator = NumericRangeTreeIterator_New(rt);
  return ctx;
}

static void gc_FreeNumericGcCtx(NumericFieldGCCtx *ctx) {
  NumericRangeTreeIterator_Free(ctx->gcIterator);
  rm_free(ctx);
}

static void gc_FreeNumericGcCtxArray(GarbageCollectorCtx *gc) {
  for (int i = 0; i < array_len(gc->numericGCCtx); ++i) {
    gc_FreeNumericGcCtx(gc->numericGCCtx[i]);
  }
  array_trimm_len(gc->numericGCCtx, 0);
}

static TagIndex *getRandomTagFieldByType(IndexSpec *spec) {
  FieldSpec **tagFields = NULL;
  tagFields = getFieldsByType(spec, INDEXFLD_T_TAG);
  if (array_len(tagFields) == 0) {
    array_free(tagFields);
    return NULL;
  }

  // choose random tag field
  int randomIndex = rand() % array_len(tagFields);
  FieldSpec *fs = tagFields[randomIndex];
  array_free(tagFields);
  TagIndex *ret = IDX_LoadTags(spec, fs, REDISMODULE_WRITE);
  return ret;
}

size_t gc_TagIndex(RedisModuleCtx *ctx, GarbageCollectorCtx *gc, int *status) {
  size_t totalRemoved = 0;
  char *randomKey = NULL;
  IndexSpec *spec = gc->sp;
  if (!IDX_IsAlive(spec)) {
    goto end;
  }

  TagIndex *indexTag = getRandomTagFieldByType(spec);
  if (!indexTag) {
    goto end;
  }

  InvertedIndex *iv;
  tm_len_t len;

  if (!TrieMap_RandomKey(indexTag->values, &randomKey, &len, (void **)&iv)) {
    goto end;
  }

  int blockNum = 0;
  do {
    // repair 100 blocks at once
    IndexRepairParams params = {.limit = RSGlobalConfig.gcScanSize, .arg = NULL};
    blockNum = InvertedIndex_Repair(iv, &spec->docs, blockNum, &params);
    /// update the statistics with the the number of records deleted
    totalRemoved += params.docsCollected;
    gc_updateStats(gc, params.docsCollected, params.bytesCollected);
    // blockNum 0 means error or we've finished
    if (!blockNum) break;

    // After each iteration we yield execution
    // First we close the relevant keys we're touching
    if (!IDX_YieldWrite(spec)) {
      *status = SPEC_STATUS_INVALID;
      break;
    }

    iv = TrieMap_Find(indexTag->values, randomKey, len);
    if (iv == TRIEMAP_NOTFOUND) {
      break;
    }

  } while (true);

end:
  if (randomKey) {
    rm_free(randomKey);
  }
  return totalRemoved;
}

size_t gc_NumericIndex(RedisModuleCtx *ctx, GarbageCollectorCtx *gc, int *status) {
  size_t totalRemoved = 0;
  FieldSpec **numericFields = NULL;
  IndexSpec *spec = gc->sp;
  // find all the numeric fields
  if (!IDX_IsAlive(spec)) {
    goto end;
  }
  numericFields = getFieldsByType(spec, INDEXFLD_T_NUMERIC);

  if (array_len(numericFields) == 0) {
    goto end;
  }

  if (array_len(numericFields) != array_len(gc->numericGCCtx)) {
    // add all numeric fields to our gc
    assert(array_len(numericFields) >
           array_len(gc->numericGCCtx));  // it is not possible to remove fields
    gc_FreeNumericGcCtxArray(gc);
    for (int i = 0; i < array_len(numericFields); ++i) {
      NumericRangeTree *rt = IDX_LoadRange(gc->sp, numericFields[i], REDISMODULE_WRITE);
      // if we could not open the numeric field we probably have a
      // corruption in our data, better to know it now.
      assert(rt);
      gc->numericGCCtx = array_append(gc->numericGCCtx, gc_NewNumericGcCtx(rt));
    }
  }

  // choose random numeric gc ctx
  int randomIndex = rand() % array_len(gc->numericGCCtx);
  NumericFieldGCCtx *numericGcCtx = gc->numericGCCtx[randomIndex];
  NumericRangeTree *rt = IDX_LoadRange(spec, numericFields[randomIndex], REDISMODULE_WRITE);
  if (numericGcCtx->rt != rt || numericGcCtx->revisionId != numericGcCtx->rt->revisionId) {
    // memory or revision changed, recreating our numeric gc ctx
    assert(numericGcCtx->rt != rt || numericGcCtx->revisionId < numericGcCtx->rt->revisionId);
    gc->numericGCCtx[randomIndex] = gc_NewNumericGcCtx(rt);
    gc_FreeNumericGcCtx(numericGcCtx);
    numericGcCtx = gc->numericGCCtx[randomIndex];
  }

  NumericRangeNode *nextNode = NextGcNode(numericGcCtx);

  int blockNum = 0;
  do {
    IndexRepairParams params = {.limit = RSGlobalConfig.gcScanSize, .arg = nextNode->range};
    // repair 100 blocks at once
    blockNum = InvertedIndex_Repair(nextNode->range->entries, &spec->docs, blockNum, &params);
    /// update the statistics with the the number of records deleted
    numericGcCtx->rt->numEntries -= params.docsCollected;
    totalRemoved += params.docsCollected;
    gc_updateStats(gc, params.docsCollected, params.bytesCollected);
    // blockNum 0 means error or we've finished
    if (!blockNum) break;
    if (!IDX_YieldWrite(spec)) {
      *status = SPEC_STATUS_INVALID;
      break;
    }
    if (numericGcCtx->revisionId != numericGcCtx->rt->revisionId) {
      break;
    }
  } while (true);

end:
  if (numericFields) {
    array_free(numericFields);
  }
  return totalRemoved;
}

/* The GC periodic callback, called in a separate thread. It selects a random term (using weighted
 * random) */
int GC_PeriodicCallback(RedisModuleCtx *ctx, void *privdata) {
  GarbageCollectorCtx *gc = privdata;

  int status = SPEC_STATUS_OK;
  RedisModule_AutoMemory(ctx);
  RedisModule_ThreadSafeContextLock(ctx);

  assert(gc);

  size_t totalRemoved = 0;

  totalRemoved += gc_RandomTerm(ctx, gc, &status);

  totalRemoved += gc_NumericIndex(ctx, gc, &status);

  totalRemoved += gc_TagIndex(ctx, gc, &status);

  gc->stats.numCycles++;
  gc->stats.effectiveCycles += totalRemoved > 0 ? 1 : 0;

  // if we didn't remove anything - reduce the frequency a bit.
  // if we did  - increase the frequency a bit
  // the timer is NULL if we've been cancelled
  if (totalRemoved > 0) {
    gc->hz = MIN(gc->hz * 1.2, GC_MAX_HZ);
  } else {
    gc->hz = MAX(gc->hz * 0.99, GC_MIN_HZ);
  }

  RedisModule_ThreadSafeContextUnlock(ctx);

  return status == SPEC_STATUS_OK;
}

/* Termination callback for the GC. Called after we stop, and frees up all the resources. */
void GC_OnTerm(void *privdata) {
  GarbageCollectorCtx *gc = privdata;
  RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
  RedisModule_ThreadSafeContextLock(ctx);
  for (int i = 0; i < array_len(gc->numericGCCtx); ++i) {
    gc_FreeNumericGcCtx(gc->numericGCCtx[i]);
  }
  array_free(gc->numericGCCtx);
  RedisModule_ThreadSafeContextUnlock(ctx);
  RedisModule_FreeThreadSafeContext(ctx);
  rm_free(gc);
}

// called externally when the user deletes a document to hint at increasing the HZ
void GC_OnDelete(void *ctx) {
  GarbageCollectorCtx *gc = ctx;
  if (!gc) return;
  gc->hz = MIN(gc->hz * 1.5, GC_MAX_HZ);
}

struct timespec GC_GetInterval(void *ctx) {
  GarbageCollectorCtx *gc = ctx;
  return hzToTimeSpec(gc->hz);
}

void GC_RenderStats(RedisModuleCtx *ctx, void *gcCtx) {
#define REPLY_KVNUM(n, k, v)                   \
  RedisModule_ReplyWithSimpleString(ctx, k);   \
  RedisModule_ReplyWithDouble(ctx, (double)v); \
  n += 2

  GarbageCollectorCtx *gc = gcCtx;

  int n = 0;
  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
  if (gc) {
    REPLY_KVNUM(n, "current_hz", gc->hz);
    REPLY_KVNUM(n, "bytes_collected", gc->stats.totalCollected);
    REPLY_KVNUM(n, "effectiv_cycles_rate",
                (double)gc->stats.effectiveCycles /
                    (double)(gc->stats.numCycles ? gc->stats.numCycles : 1));
  }
  RedisModule_ReplySetArrayLength(ctx, n);
}