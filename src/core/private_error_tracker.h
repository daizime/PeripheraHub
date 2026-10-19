#pragma once
#include <map>
#include <set>
#include <string>

namespace peripheral::core {

// 私有读数错误的边沿记录器：同一路径只在"新失败 / 原因变化 / 恢复"时报告一次，
// 避免后台轮询把同一条错误刷进日志。它不持有设备句柄，也不访问系统接口。
class PrivateErrorTracker {
  public:
    enum class Change { None, Failed, Recovered };

    // error 为空表示本次读取成功。
    Change update(const std::wstring& path, const std::string& error) {
        const auto it = errors_.find(path);
        const std::string previous = it == errors_.end() ? std::string{} : it->second;
        if (previous == error)
            return Change::None;
        if (error.empty()) {
            if (it != errors_.end())
                errors_.erase(it);
            return Change::Recovered;
        }
        errors_[path] = error;
        return Change::Failed;
    }

    // 设备消失后丢弃其记录，重新接入时从干净状态开始计数。
    void retain(const std::set<std::wstring>& present) {
        for (auto it = errors_.begin(); it != errors_.end();)
            if (present.contains(it->first))
                ++it;
            else
                it = errors_.erase(it);
    }

    bool failing(const std::wstring& path) const { return errors_.contains(path); }
    std::size_t size() const { return errors_.size(); }

  private:
    std::map<std::wstring, std::string> errors_;
};

} // namespace peripheral::core
