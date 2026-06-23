#include "buffer/clock_replacer.h"

CLOCKReplacer::CLOCKReplacer(size_t num_pages) : capacity(num_pages) {}

CLOCKReplacer::~CLOCKReplacer() = default;

/**
 * Victim: walks the clock list and selects the next frame whose ref_bit == 0.
 * Any frame encountered with ref_bit == 1 is given a "second chance" — its
 * bit is cleared and it is rotated to the back of the list.
 *
 * Returns true and writes the chosen frame_id via *frame_id if a victim is
 * found; returns false when the clock list is empty.
 */
bool CLOCKReplacer::Victim(frame_id_t *frame_id) {
  if (clock_list.empty()) {
    return false;
  }

  // Standard second-chance clock:
  //   Walk the list. For each entry:
  //     - if ref_bit == 1, clear it and rotate to back (second chance)
  //     - if ref_bit == 0, evict it
  //   Bound the walk at 2 * size so we always terminate even when every
  //   entry has ref_bit == 1 (after one full rotation all bits are 0,
  //   so the second rotation will find a victim).
  size_t budget = clock_list.size() * 2;
  size_t examined = 0;
  while (examined < budget) {
    frame_id_t candidate = clock_list.front();
    clock_list.pop_front();

    auto it = clock_status.find(candidate);
    if (it != clock_status.end() && it->second == 1) {
      // ref_bit == 1 — give a second chance, clear bit, rotate to back
      clock_status[candidate] = 0;
      clock_list.push_back(candidate);
    } else {
      // ref_bit == 0 — victim found, evict
      clock_status.erase(candidate);
      *frame_id = candidate;
      return true;
    }
    ++examined;
  }
  return false;
}

/**
 * Pin: removes frame_id from the clock list so it can never be chosen
 * as a victim until it is unpinned again.
 */
void CLOCKReplacer::Pin(frame_id_t frame_id) {
  clock_status.erase(frame_id);
  clock_list.remove(frame_id);
}

/**
 * Unpin: makes frame_id eligible for eviction again. Sets ref_bit = 1
 * so it gets a second chance on the next Victim() call.
 * If frame_id is already tracked, this is a no-op.
 */
void CLOCKReplacer::Unpin(frame_id_t frame_id) {
  if (clock_status.find(frame_id) != clock_status.end()) {
    return;  // already tracked — don't add twice
  }
  clock_status[frame_id] = 1;  // start with ref_bit = 1
  clock_list.push_back(frame_id);
}

size_t CLOCKReplacer::Size() {
  return clock_list.size();
}