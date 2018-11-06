/* =============================================================================
 *
 * tmalloc.c
 *
 * Memory allocator with extra metadata for TM.
 *
 * =============================================================================
 */

// External headers
#include <stdlib.h>

// Internal headers
#include <tmalloc.h>

/* =============================================================================
 * tmalloc_alloc
 * -- Returns NULL if failed
 * =============================================================================
 */
tmalloc_t *
tmalloc_alloc(long initCapacity)
{
  tmalloc_t *tmallocPtr;
  long capacity = ((initCapacity > 1) ? initCapacity : 1);

  tmallocPtr = (tmalloc_t *)malloc(sizeof(tmalloc_t));

  if (tmallocPtr != NULL)
  {
    tmallocPtr->size = 0;
    tmallocPtr->capacity = capacity;
    tmallocPtr->elements = (void **)malloc(capacity * sizeof(void *));
    if (tmallocPtr->elements == NULL)
    {
      return NULL;
    }
  }

  return tmallocPtr;
}