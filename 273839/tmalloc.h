/* =============================================================================
 *
 * tmalloc.h
 *
 * Memory allocator with extra metadata for TM.
 *
 * =============================================================================
 */

typedef struct tmalloc
{
  long size;
  long capacity;
  void **elements;
} tmalloc_t;