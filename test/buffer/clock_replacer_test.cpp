#include "buffer/clock_replacer.h"

#include "gtest/gtest.h"

/**
 * Basic CLOCK Replacer test.
 *
 * Unpin adds frames to the clock list with ref_bit = 1.
 * Victim walks the list, clearing ref_bit = 1 entries (giving them a
 * "second chance" by rotating them to the back) and selecting the first
 * frame whose ref_bit == 0.
 */
TEST(CLOCKReplacerTest, SampleTest) {
  CLOCKReplacer clock_replacer(7);

  // Unpin 1..6 (skipping 0 to match LRU test pattern).
  clock_replacer.Unpin(1);
  clock_replacer.Unpin(2);
  clock_replacer.Unpin(3);
  clock_replacer.Unpin(4);
  clock_replacer.Unpin(5);
  clock_replacer.Unpin(6);
  // Re-unpinning 1 should be a no-op (already tracked).
  clock_replacer.Unpin(1);
  EXPECT_EQ(6, clock_replacer.Size());

  // Victim walks: 1 (bit=1 → clear+rotate), 2 (bit=1 → clear+rotate),
  //               3 (bit=1 → clear+rotate), 4 (bit=1 → clear+rotate),
  //               5 (bit=1 → clear+rotate), 6 (bit=1 → clear+rotate),
  //               1 (bit=0) → victim.
  int value;
  clock_replacer.Victim(&value);
  EXPECT_EQ(1, value);

  // Second Victim: 2 (bit=0 now) → victim.
  clock_replacer.Victim(&value);
  EXPECT_EQ(2, value);

  // Third Victim: 3 → victim.
  clock_replacer.Victim(&value);
  EXPECT_EQ(3, value);
  EXPECT_EQ(3, clock_replacer.Size());

  // Pinning 3 (already evicted) is a no-op; pinning 4 removes 4 from list.
  clock_replacer.Pin(3);
  clock_replacer.Pin(4);
  EXPECT_EQ(2, clock_replacer.Size());

  // Unpin 4 with ref_bit = 1 — it'll be revisited before being victimized.
  clock_replacer.Unpin(4);

  // Continue Victim: list is [4(ref=1), 5(ref=0)] after first pop gives 4 a
  // second chance → 5 is selected.
  clock_replacer.Victim(&value);
  EXPECT_EQ(5, value);
  clock_replacer.Victim(&value);
  EXPECT_EQ(6, value);
  // Last survivor: 4 (now ref_bit = 0 after one rotation).
  clock_replacer.Victim(&value);
  EXPECT_EQ(4, value);
  EXPECT_EQ(0, clock_replacer.Size());
}

/**
 * Empty replacer returns false on Victim.
 */
TEST(CLOCKReplacerTest, EmptyReplacer) {
  CLOCKReplacer r(10);
  EXPECT_EQ(0, r.Size());
  int v;
  EXPECT_FALSE(r.Victim(&v));
}

/**
 * Pin removes a frame; Size decreases accordingly.
 */
TEST(CLOCKReplacerTest, PinRemovesFrame) {
  CLOCKReplacer r(5);
  r.Unpin(0);
  r.Unpin(1);
  r.Unpin(2);
  EXPECT_EQ(3, r.Size());
  r.Pin(1);
  EXPECT_EQ(2, r.Size());
  r.Pin(0);
  r.Pin(2);
  EXPECT_EQ(0, r.Size());
}

/**
 * Unpin on a frame already tracked is a no-op.
 */
TEST(CLOCKReplacerTest, UnpinIdempotent) {
  CLOCKReplacer r(3);
  r.Unpin(0);
  r.Unpin(0);  // already tracked
  r.Unpin(0);  // still tracked
  EXPECT_EQ(1, r.Size());
}

/**
 * A frame that was victimized can be re-unpinned.
 */
TEST(CLOCKReplacerTest, ReviveAfterVictim) {
  CLOCKReplacer r(2);
  r.Unpin(0);
  r.Unpin(1);
  int v;
  ASSERT_TRUE(r.Victim(&v));
  EXPECT_EQ(0, v);
  EXPECT_EQ(1, r.Size());
  // Re-unpin the victimized frame.
  r.Unpin(0);
  EXPECT_EQ(2, r.Size());
}

/**
 * Verify the second-chance round-trip: with all entries at ref_bit = 1,
 * one Victim() call clears all bits and selects the first.
 */
TEST(CLOCKReplacerTest, AllRefBitsOneSecondChance) {
  CLOCKReplacer r(4);
  r.Unpin(0);
  r.Unpin(1);
  r.Unpin(2);
  r.Unpin(3);

  // First victim walks all 4 entries, giving each a second chance,
  // then picks the first (0).
  int v;
  ASSERT_TRUE(r.Victim(&v));
  EXPECT_EQ(0, v);
  EXPECT_EQ(3, r.Size());

  // Now ref_bits for {1,2,3} are all 0. Next victim is 1.
  ASSERT_TRUE(r.Victim(&v));
  EXPECT_EQ(1, v);
}

/**
 * Stress test: many Unpin + Victim cycles remain consistent.
 */
TEST(CLOCKReplacerTest, StressTest) {
  CLOCKReplacer r(50);
  // Unpin 50 frames
  for (int i = 0; i < 50; i++) {
    r.Unpin(i);
  }
  EXPECT_EQ(50, r.Size());

  // Victimize all of them
  std::set<int> evicted;
  int v;
  for (int i = 0; i < 50; i++) {
    ASSERT_TRUE(r.Victim(&v));
    evicted.insert(v);
  }
  EXPECT_EQ(50, evicted.size());  // all unique
  EXPECT_EQ(0, r.Size());

  // No more victims
  EXPECT_FALSE(r.Victim(&v));
}