#include <tm.h>
#include <tmlist.h>
#include <stdlib.h>

/* =============================================================================
 * MakeList
 *
 * Allocate the primary list as a large chunk so we can guarantee ascending &
 * adjacent addresses through the list. This improves D$ and DTLB behavior.
 * =============================================================================
 */
__INLINE__ AVPair *MakeList(long sz, Thread *Self)
{
  AVPair *ap = (AVPair *)malloc((sizeof(*ap) * sz) + TL2_CACHE_LINE_SIZE);
  assert(ap);
  memset(ap, 0, sizeof(*ap) * sz);
  AVPair *List = ap;
  AVPair *Tail = NULL;
  long i;
  for (i = 0; i < sz; i++)
  {
    AVPair *e = ap++;
    e->Next = ap;
    e->Prev = Tail;
    e->Owner = Self;
    e->Ordinal = i;
    Tail = e;
  }
  Tail->Next = NULL;

  return List;
}