#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "AncsProtocol.h"

namespace ancs {

// Fixed-capacity, newest-first list of ANCS notifications keyed by UID.
// Not thread-safe: callers serialize access (IosAncsService holds g_ancsMutex).
template <size_t N>
class NotificationList {
 public:
  static_assert(N > 0, "NotificationList needs capacity");

  size_t size() const { return count_; }
  static constexpr size_t capacity() { return N; }
  const AncsNotification& at(size_t index) const { return items_[index]; }

  // Remove `uid` if present. Returns true when an entry was removed.
  bool remove(uint32_t uid) {
    for (size_t i = 0; i < count_; ++i) {
      if (items_[i].valid && items_[i].uid == uid) {
        for (size_t j = i + 1; j < count_; ++j) {
          items_[j - 1] = items_[j];
        }
        items_[count_ - 1].clear();
        --count_;
        return true;
      }
    }
    return false;
  }

  // Insert `item` at the front, replacing any entry with the same UID and
  // evicting the oldest entry when full.
  void upsertFront(const AncsNotification& item) {
    remove(item.uid);
    const size_t shiftCount = std::min<size_t>(count_, N - 1);
    for (size_t i = shiftCount; i > 0; --i) {
      items_[i] = items_[i - 1];
    }
    items_[0] = item;
    count_ = shiftCount + 1;
    for (size_t i = count_; i < N; ++i) {
      items_[i].clear();
    }
  }

  void clear() {
    for (auto& it : items_) {
      it.clear();
    }
    count_ = 0;
  }

 private:
  std::array<AncsNotification, N> items_{};
  size_t count_ = 0;
};

}  // namespace ancs
