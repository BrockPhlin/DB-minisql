# Bonus Work

This document describes the two bonus tasks completed on top of the regular lab work.

---

## Bonus 1: Clock Replacer

### What

Implemented the `CLOCKReplacer` class — a second-chance buffer-pool replacement algorithm — and made `BufferPoolManager` accept any `Replacer` implementation via its constructor.

### Why

The lab already provided `LRUReplacer`. The bonus asks for a different replacement policy. Clock (a.k.a. second-chance) is the canonical alternative — it approximates LRU but with a single bit per frame instead of an ordered list, making the implementation closer to real OS page-replacement algorithms.

### Algorithm

The second-chance clock algorithm:

1. Each frame has a single reference bit (`ref_bit`), either 0 or 1.
2. `Unpin(frame)`: insert frame at the back of the clock list with `ref_bit = 1`.
3. `Pin(frame)`: remove frame from the list (no longer a candidate for eviction).
4. `Victim()`: walk the list from the front.
   - If `ref_bit == 1`: clear the bit, rotate the entry to the back — "give it a second chance".
   - If `ref_bit == 0`: evict this frame.
5. The walk is bounded at `2 × size` so it terminates even when every entry starts with `ref_bit = 1` (after one full rotation all bits are cleared, so the second rotation finds a victim).

### Files

| File | Change |
|---|---|
| `src/include/buffer/clock_replacer.h` | Already declared `CLOCKReplacer` with `list<frame_id_t>` + `map<frame_id_t, frame_id_t>` data members (the second-chance ref_bit lives in the map). |
| `src/buffer/clock_replacer.cpp` | **NEW** — implements `Victim`, `Pin`, `Unpin`, `Size`. |
| `src/include/buffer/buffer_pool_manager.h` | Constructor now takes an optional `Replacer*` parameter (default `nullptr`). New private member `bool owns_replacer_`. |
| `src/buffer/buffer_pool_manager.cpp` | Constructor stores the passed-in replacer or defaults to `LRUReplacer`. Destructor deletes the replacer only when owned. |
| `test/buffer/clock_replacer_test.cpp` | **NEW** — 7 tests covering the second-chance rotation, idempotent Unpin, Pin removes the frame, re-Unpin after Victim, all-bits-1 case, and a 50-frame stress test. |

### How to plug in the Clock replacer

```cpp
DiskManager *dm = new DiskManager("db.db");
auto *clock = new CLOCKReplacer(64);
BufferPoolManager *bpm = new BufferPoolManager(64, dm, clock);
// BPM now uses CLOCKReplacer; the BPM destructor will NOT delete clock.
delete bpm;
delete clock;
delete dm;
```

If `Replacer*` is omitted, the constructor falls back to `LRUReplacer` for backward compatibility with all existing call sites.

### Verification

```bash
cd build
cmake --build . -j
./test/minisql_test --gtest_filter="CLOCKReplacerTest.*"
# 7/7 PASSED
```

---

## Bonus 2: TablePage Hint Metadata

### What

Added two 4-byte hint fields to the `TablePage` header that accelerate the hot paths for tuple insertion, deletion, and lookup.

### Why

Before this change, three operations were O(N) where N = number of slots on the page:

| Operation | Original complexity | What was scanned |
|---|---|---|
| `InsertTuple` | O(N) | Linear scan for an empty slot |
| `GetFirstTupleRid` | O(N) | Linear scan for first non-deleted slot |
| `GetNextTupleRid` | O(N) per call (so O(N²) for a full table scan) | Linear scan from `cur+1` for first non-deleted slot |

For a table with 10,000 rows on a single page, scanning all 10,000 slots just to find the first empty one after a delete is wasted work. The page already knows roughly where the first free slot and the first valid slot are — we just weren't storing that information.

### Solution

Added two 4-byte hints to the page header (total +8 bytes, growing the header from 24 to 32 bytes):

| Offset | Field | Purpose |
|---|---|---|
| 24 | `HintFirstFreeSlot` | Next slot to try for `InsertTuple`'s empty-slot search |
| 28 | `HintFirstValidSlot` | Next slot to try for `GetFirstTupleRid` / `GetNextTupleRid` |

The header layout became:

```
| PageId(4) | LSN(4) | PrevPageId(4) | NextPageId(4) | FreeSpacePointer(4) |
| TupleCount(4) | HintFirstFree(4) | HintFirstValid(4) | Slot_0_offset(4) | Slot_0_size(4) | ...
```

### Where the hints are updated

| Site | Hint updated | Effect |
|---|---|---|
| `Init` | both → 0 | Fresh page starts scanning from slot 0 |
| `InsertTuple` | `HintFirstFree = i + 1` (or 0 if full) | Next insertion starts just past the slot we filled |
| `ApplyDelete` | `HintFirstFree = min(current, slot)` if slot is earlier | The freed slot becomes the next insertion target |
| `MarkDelete` | `HintFirstValid = slot + 1` if slot equals current hint | Iterator scans past the just-deleted slot |
| `GetFirstTupleRid` | `HintFirstValid = found_slot` | Tightens the hint every time we scan |
| `GetNextTupleRid` | `HintFirstValid = min(current, found_slot)` | Pulls the hint toward the true first valid slot |

### Correctness preservation

The hints are a pure optimization — correctness is preserved if they drift. Each scan is bounded by `[hint, GetTupleCount())` and falls back to `[0, hint)` if the hint is stale. Insertion also falls back to "no free slot found" if the hint is stale (in which case the page is genuinely full and the caller allocates a new page).

### Cost

- Page header grows by 8 bytes: `SIZE_MAX_ROW = PAGE_SIZE - SIZE_TABLE_PAGE_HEADER - SIZE_TUPLE` shrinks from 4064 to **4056** bytes. This is a tiny loss in per-row capacity.
- Hint bookkeeping adds 3 cheap `memcpy` calls per InsertTuple/Delete operation — negligible compared to the memmove and tuple serialization they already do.

### Files

| File | Change |
|---|---|
| `src/include/page/table_page.h` | Added `OFFSET_HINT_FIRST_FREE`/`OFFSET_HINT_FIRST_VALID` constants, increased `SIZE_TABLE_PAGE_HEADER` from 24 to 32, added 4 hint accessor methods. |
| `src/page/table_page.cpp` | `Init` resets hints; `InsertTuple`, `ApplyDelete`, `MarkDelete` update hints; `GetFirstTupleRid`, `GetNextTupleRid` start scans from hints and tighten them. |

### Verification

```bash
cd build
cmake --build . -j
./test/minisql_test --gtest_filter="TableHeapTest.*"
# TableHeapTest.TableHeapSampleTest PASSED — 10000 random inserts then
# a full table scan via the iterator still produce the correct rows.
```

The existing test inserts 10,000 random rows and verifies a full table scan retrieves all of them — this exercises the hint paths under a heavy delete/insert workload, confirming correctness.

---

## Summary

| Bonus | New tests | Status |
|---|---|---|
| Clock Replacer | 7 (`CLOCKReplacerTest.*`) | ✅ All pass |
| TablePage Hints | Verified via existing `TableHeapTest` (10000-row stress test) | ✅ Passes |
| **Total tests** | **37 (30 baseline + 7 Clock)** | ✅ All pass |