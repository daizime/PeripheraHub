#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace peripheral {

// 只保留一个已派发请求和一个最新待写值，慢设备不会积压整段拖动轨迹。
// 由调用者提供时间，便于脱离 Qt、线程和真实硬件验证节流行为。
class LiveValueControl {
  public:
    using Milliseconds = std::int64_t;
    struct Request {
        std::wstring target;
        int percent;
        std::uint64_t token;
        std::uint64_t generation;
    };

    explicit LiveValueControl(Milliseconds interval) : interval_(std::max<Milliseconds>(1, interval)) {}

    bool selectTarget(std::wstring target) {
        if (target == target_)
            return false;
        target_ = std::move(target);
        pending_.reset();
        urgent_ = false;
        lastDispatch_.reset();
        ++generation_;
        // 切换目标会丢弃待写值；已派发请求仍绑定原目标，完成后才允许下一次派发。
        return true;
    }
    bool request(const std::wstring& target, int percent) {
        if (target.empty() || target != target_)
            return false;
        pending_ = std::clamp(percent, 0, 100);
        return true;
    }
    void flush() { urgent_ = true; }
    std::optional<Milliseconds> delay(Milliseconds now) const {
        if (inFlight_ || !pending_)
            return std::nullopt;
        if (urgent_ || !lastDispatch_)
            return 0;
        return std::max<Milliseconds>(0, interval_ - (now - *lastDispatch_));
    }
    std::optional<Request> take(Milliseconds now) {
        const auto wait = delay(now);
        if (!wait || *wait != 0)
            return std::nullopt;
        inFlight_ = Request{target_, *pending_, ++token_, generation_};
        pending_.reset();
        urgent_ = false;
        lastDispatch_ = now;
        return inFlight_;
    }
    bool complete(std::uint64_t token) {
        if (!inFlight_ || inFlight_->token != token)
            return false;
        inFlight_.reset();
        return true;
    }
    bool active() const { return inFlight_.has_value() || pending_.has_value(); }
    bool hasPending() const { return pending_.has_value(); }
    const std::wstring& target() const { return target_; }
    std::uint64_t generation() const { return generation_; }

  private:
    Milliseconds interval_;
    std::wstring target_;
    std::optional<int> pending_;
    std::optional<Request> inFlight_;
    std::optional<Milliseconds> lastDispatch_;
    std::uint64_t token_ = 0;
    std::uint64_t generation_ = 0;
    bool urgent_ = false;
};

} // namespace peripheral
