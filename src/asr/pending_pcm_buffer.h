#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

constexpr size_t kDefaultPendingPcmMaxBytes = 120u * 32000u;

class PendingPcmBuffer {
public:
    explicit PendingPcmBuffer(size_t maxBytes = kDefaultPendingPcmMaxBytes)
        : maxBytes_(maxBytes) {}

    bool Append(const BYTE* data, size_t bytes) {
        if (!data || bytes == 0) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (maxBytes_ > 0 && data_.size() + bytes > maxBytes_) {
            overflowed_ = true;
            return false;
        }
        data_.insert(data_.end(), data, data + bytes);
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.clear();
        overflowed_ = false;
    }

    void SwapTo(std::vector<BYTE>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(data_);
    }

    bool DrainTo(std::vector<BYTE>& target, size_t targetBytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (data_.empty() || target.size() >= targetBytes) return false;
        const size_t take = (std::min)(targetBytes - target.size(), data_.size());
        target.insert(target.end(), data_.begin(), data_.begin() + static_cast<ptrdiff_t>(take));
        data_.erase(data_.begin(), data_.begin() + static_cast<ptrdiff_t>(take));
        return take > 0;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.size();
    }

    bool Overflowed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return overflowed_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<BYTE> data_;
    size_t maxBytes_ = kDefaultPendingPcmMaxBytes;
    bool overflowed_ = false;
};
