#ifndef MEDIA_STREAM_BOUNDED_QUEUE_H
#define MEDIA_STREAM_BOUNDED_QUEUE_H

// 有界队列模板：两种背压策略
//  - BlockOnFull：MP4 写线程语义，队满时阻塞等待（本地封装正确性优先，不丢帧）
//  - DropNewestVideo：RTMP 发送语义，队满时丢弃新入队元素（直播链路不背压采集/编码）

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>

namespace media_stream {

enum class OverflowPolicy {
    kBlockOnFull,     // 队满阻塞（MP4）
    kDropNewest,      // 队满丢弃新元素（RTMP 视频帧）
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity, OverflowPolicy policy)
        : capacity_(capacity), policy_(policy), closed_(false) {}

    // 入队；返回 false 表示队列已关闭或元素被丢弃
    bool Push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (policy_ == OverflowPolicy::kBlockOnFull) {
            // 阻塞策略：等待有空间或关闭
            notFull_.wait(lock, [this] { return queue_.size() < capacity_ || closed_; });
            if (closed_) {
                return false;
            }
        } else {
            // 丢弃策略：队满直接丢弃新元素
            if (queue_.size() >= capacity_) {
                droppedCount_++;
                return false;
            }
        }
        queue_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    // 带超时的入队（TimeBlockOnFull 语义）：最多等待 timeoutMs 毫秒获取空间，超时返回 false。
    // 用于在「持锁调用入队」的场景下避免因消费者（写线程）短暂卡顿而无限阻塞持锁线程，
    // 造成锁序反转死锁。返回 false 表示超时/关闭/未入队，调用方应丢弃该元素。
    bool TryPush(T item, int64_t timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (queue_.size() >= capacity_) {
            notFull_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return queue_.size() < capacity_ || closed_; });
        }
        if (closed_) {
            return false;
        }
        if (queue_.size() >= capacity_) {
            droppedCount_++;
            return false; // 超时仍未腾出空间
        }
        queue_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    // 出队（阻塞）；返回 false 表示队列已关闭且为空
    bool Pop(T &out) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) {
            return false; // 已关闭且无剩余元素
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }

    // 带超时的出队：取到元素返回 true；超时或「已关闭且为空」返回 false。
    // 供 MP4 写线程在「停录后继续排空音频尾巴」期间使用 —— 那里既不能无限阻塞（音频真断流
    // 时文件将永远不收尾），也不能立刻返回（在途样本还没到）。
    bool PopTimed(T &out, int64_t timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }

    // 关闭队列：唤醒所有等待者；Pop 在排空后返回 false
    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        notFull_.notify_all();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    uint64_t DroppedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return droppedCount_;
    }

    void ResetDroppedCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        droppedCount_ = 0;
    }

private:
    const size_t capacity_;
    const OverflowPolicy policy_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> queue_;
    bool closed_;
    uint64_t droppedCount_ = 0;
};

} // namespace media_stream

#endif // MEDIA_STREAM_BOUNDED_QUEUE_H
