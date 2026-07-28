#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

namespace mdv {

// A bounded FIFO that keeps only the newest value for each logical key.
// SameQueueKey(value, value) and QueueItemBytes(value) are resolved by ADL,
// allowing every service to define its own key and memory estimate.
template <typename T, std::size_t MaximumItems, std::size_t MaximumBytes>
class BoundedLatestQueue {
    static_assert(MaximumItems > 0U);
    static_assert(MaximumBytes > 0U);

public:
    bool push_back(T value)
    {
        const std::size_t valueBytes = QueueItemBytes(value);
        if (valueBytes > MaximumBytes) {
            ++droppedCount_;
            return false;
        }

        const auto duplicate = std::find_if(
            entries_.begin(), entries_.end(),
            [&](const Entry& entry) {
                return SameQueueKey(entry.value, value);
            });
        if (duplicate != entries_.end()) {
            currentBytes_ -= duplicate->bytes;
            entries_.erase(duplicate);
        }

        while (!entries_.empty() &&
               (entries_.size() >= MaximumItems ||
                currentBytes_ + valueBytes > MaximumBytes)) {
            currentBytes_ -= entries_.front().bytes;
            entries_.pop_front();
            ++droppedCount_;
        }

        entries_.push_back(Entry{std::move(value), valueBytes});
        currentBytes_ += valueBytes;
        return true;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return entries_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return entries_.size();
    }

    [[nodiscard]] std::size_t bytes() const noexcept
    {
        return currentBytes_;
    }

    [[nodiscard]] std::size_t dropped_count() const noexcept
    {
        return droppedCount_;
    }

    T& front()
    {
        return entries_.front().value;
    }

    const T& front() const
    {
        return entries_.front().value;
    }

    void pop_front()
    {
        currentBytes_ -= entries_.front().bytes;
        entries_.pop_front();
    }

    void clear() noexcept
    {
        entries_.clear();
        currentBytes_ = 0U;
    }

private:
    struct Entry {
        T value;
        std::size_t bytes = 0U;
    };

    std::deque<Entry> entries_;
    std::size_t currentBytes_ = 0U;
    std::size_t droppedCount_ = 0U;
};

} // namespace mdv
