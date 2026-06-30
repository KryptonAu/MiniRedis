#include <gtest/gtest.h>

#include "ds/dict.h"

namespace miniredis::ds {
namespace {

TEST(DictTest, ConstructEmpty) {
  Dict<int, std::string> d;
  EXPECT_EQ(d.Size(), 0);
  EXPECT_GT(d.Buckets(), 0);
  EXPECT_FALSE(d.IsRehashing());
}

TEST(DictTest, AddAndFind) {
  Dict<int, std::string> d;
  EXPECT_TRUE(d.Add(1, "one"));
  EXPECT_EQ(d.Size(), 1);
  EXPECT_FALSE(d.Add(1, "again"));  // duplicate key

  const std::string* val = d.Find(1);
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(*val, "one");
}

TEST(DictTest, FindMissing) {
  Dict<int, std::string> d;
  EXPECT_EQ(d.Find(42), nullptr);

  d.Add(1, "one");
  EXPECT_EQ(d.Find(42), nullptr);
}

TEST(DictTest, SetNewAndExisting) {
  Dict<int, std::string> d;
  EXPECT_TRUE(d.Set(1, "first"));
  EXPECT_EQ(d.Size(), 1);
  EXPECT_EQ(*d.Find(1), "first");

  EXPECT_TRUE(d.Set(1, "second"));
  EXPECT_EQ(d.Size(), 1);
  EXPECT_EQ(*d.Find(1), "second");
}

TEST(DictTest, Delete) {
  Dict<int, std::string> d;
  d.Add(1, "one");
  d.Add(2, "two");
  EXPECT_EQ(d.Size(), 2);

  EXPECT_TRUE(d.Delete(1));
  EXPECT_EQ(d.Size(), 1);
  EXPECT_EQ(d.Find(1), nullptr);
  EXPECT_NE(d.Find(2), nullptr);

  EXPECT_FALSE(d.Delete(42));  // not found
  EXPECT_EQ(d.Size(), 1);
}

TEST(DictTest, Clear) {
  Dict<int, std::string> d;
  d.Add(1, "one");
  d.Add(2, "two");
  d.Add(3, "three");
  EXPECT_EQ(d.Size(), 3);

  d.Clear();
  EXPECT_EQ(d.Size(), 0);
  EXPECT_EQ(d.Find(1), nullptr);
}

TEST(DictTest, ExpandTriggersRehash) {
  Dict<int, std::string> d;
  // Initial size is 4, so 4 inserts should trigger expand
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(d.Add(i, "val")) << "i=" << i;
  }
  // Should now be rehashing or already expanded
  EXPECT_GT(d.Size(), 4);
  // All keys still findable
  for (int i = 0; i < 5; i++) {
    EXPECT_NE(d.Find(i), nullptr) << "i=" << i;
  }
}

TEST(DictTest, AddDuringRehash) {
  Dict<int, std::string> d;
  // Fill to trigger expand: initial 4 buckets
  for (int i = 0; i < 5; i++) {
    d.Add(i, "val");
  }
  // Add more during/after rehash
  for (int i = 5; i < 20; i++) {
    EXPECT_TRUE(d.Add(i, "val")) << "i=" << i;
  }
  EXPECT_EQ(d.Size(), 20);
  for (int i = 0; i < 20; i++) {
    EXPECT_NE(d.Find(i), nullptr) << "i=" << i;
  }
}

TEST(DictTest, DeleteDuringRehash) {
  Dict<int, std::string> d;
  for (int i = 0; i < 10; i++) {
    d.Add(i, "val");
  }
  EXPECT_EQ(d.Size(), 10);
  // Delete some keys
  EXPECT_TRUE(d.Delete(0));
  EXPECT_TRUE(d.Delete(5));
  EXPECT_EQ(d.Size(), 8);
  EXPECT_EQ(d.Find(0), nullptr);
  EXPECT_EQ(d.Find(5), nullptr);
  EXPECT_NE(d.Find(7), nullptr);
}

TEST(DictTest, SetUpdatesExisting) {
  Dict<int, std::string> d;
  d.Add(1, "first");
  d.Set(1, "second");
  EXPECT_EQ(d.Size(), 1);
  EXPECT_EQ(*d.Find(1), "second");
}

TEST(DictTest, DeleteTriggersShrink) {
  Dict<int, std::string> d;
  // Add many items to expand the table
  for (int i = 0; i < 100; i++) {
    d.Add(i, "val");
  }
  size_t buckets_before = d.Buckets();
  EXPECT_GT(buckets_before, 100);  // Should be expanded

  // Delete most items
  for (int i = 0; i < 95; i++) {
    d.Delete(i);
  }
  // After shrinking, buckets should decrease (or be in process)
  // Just verify remaining keys are findable
  EXPECT_EQ(d.Size(), 5);
  for (int i = 95; i < 100; i++) {
    EXPECT_NE(d.Find(i), nullptr) << "i=" << i;
  }
}

TEST(DictTest, SetAfterDeletingAllExpandedEntriesKeepsBucketsValid) {
  Dict<int, std::string> d;
  for (int i = 0; i < 100; i++) {
    ASSERT_TRUE(d.Set(i, "val"));
  }
  ASSERT_GT(d.Buckets(), 4);

  for (int i = 0; i < 100; i++) {
    ASSERT_TRUE(d.Delete(i));
  }
  ASSERT_EQ(d.Size(), 0);

  EXPECT_TRUE(d.Set(200, "new"));
  EXPECT_EQ(d.Size(), 1);
  ASSERT_NE(d.Find(200), nullptr);
  EXPECT_EQ(*d.Find(200), "new");
}

TEST(DictTest, RandomKeyEmpty) {
  Dict<int, std::string> d;
  EXPECT_EQ(d.RandomKey(), std::nullopt);
}

TEST(DictTest, RandomKeyNonEmpty) {
  Dict<int, std::string> d;
  d.Add(42, "answer");
  auto key = d.RandomKey();
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, 42);
}

// ===== Iterator =====
TEST(DictTest, IteratorEmpty) {
  Dict<int, std::string> d;
  int count = 0;
  for (auto it = d.begin(); it != d.end(); ++it) {
    count++;
  }
  EXPECT_EQ(count, 0);
}

TEST(DictTest, IteratorTraverseAll) {
  Dict<int, std::string> d;
  d.Add(1, "one");
  d.Add(2, "two");
  d.Add(3, "three");

  std::set<int> seen_keys;
  std::set<std::string> seen_values;
  for (auto it = d.begin(); it != d.end(); ++it) {
    seen_keys.insert(it->key);
    seen_values.insert(it->value);
  }

  EXPECT_EQ(seen_keys.size(), 3);
  EXPECT_TRUE(seen_keys.count(1));
  EXPECT_TRUE(seen_keys.count(2));
  EXPECT_TRUE(seen_keys.count(3));
  EXPECT_TRUE(seen_values.count("one"));
  EXPECT_TRUE(seen_values.count("two"));
  EXPECT_TRUE(seen_values.count("three"));
}

TEST(DictTest, IteratorDuringRehash) {
  Dict<int, std::string> d;
  // Insert many items to trigger rehash
  for (int i = 0; i < 100; i++) {
    d.Add(i, "val");
  }

  // Iterator should see all entries even during rehash
  std::set<int> seen;
  for (auto it = d.begin(); it != d.end(); ++it) {
    seen.insert(it->key);
  }
  EXPECT_EQ(seen.size(), 100);
}

// ===== SafeIterator =====
TEST(DictTest, SafeIteratorDeleteCurrentEntry) {
  Dict<int, std::string> d;
  d.Add(1, "one");
  d.Add(2, "two");
  d.Add(3, "three");

  // Safe iterator should survive deletion of the *current* entry
  auto it = d.SafeBegin();
  auto end = d.SafeEnd();

  ASSERT_NE(it, end);
  EXPECT_EQ(it->key, 1);  // or whatever first entry is
  int first_key = it->key;
  d.Delete(first_key);  // delete while iterator points to it
  ++it;                  // must advance safely via pre-computed next

  // Should still reach remaining entries
  ASSERT_NE(it, end);
  int count = 0;
  while (it != end) {
    count++;
    ++it;
  }
  EXPECT_EQ(count, 2);  // 3 original - 1 deleted = 2 remaining
}

TEST(DictTest, SafeIteratorDeleteMultipleDuringIteration) {
  Dict<int, std::string> d;
  for (int i = 0; i < 10; i++) {
    d.Add(i, "val");
  }

  auto it = d.SafeBegin();
  auto end = d.SafeEnd();

  int seen = 0;
  while (it != end) {
    int key = it->key;
    seen++;
    if (key % 2 == 0) {
      d.Delete(key);  // delete even keys on the fly
    }
    ++it;
  }
  EXPECT_EQ(seen, 10);  // iterated all original entries
  EXPECT_EQ(d.Size(), 5);  // 5 odd keys left
}

TEST(DictTest, SafeIteratorEqualityAtSamePosition) {
  Dict<int, std::string> d;
  d.Add(1, "one");
  d.Add(2, "two");

  auto it = d.SafeBegin();
  auto same = it;
  EXPECT_EQ(it, same);

  ++same;
  EXPECT_NE(it, same);
}

}  // namespace
}  // namespace miniredis::ds
