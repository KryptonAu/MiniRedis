#pragma once

#include <chrono>
#include <thread>

namespace miniredis::test {

// Callers must stop and join their workers even when this times out. CTest's
// process timeout also bounds joins if a regression breaks shutdown itself.
template <class Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

}  // namespace miniredis::test
