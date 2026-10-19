#include "DeviceProfiles.h"
#include "core/n9_protocol.h"
#include "core/vgn_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <cmath>
#include <utility>

namespace peripheral::app {
namespace {
constexpr qint64 maxFileBytes = 1024 * 1024;
constexpr qsizetype maxProfiles = 512;

bool integerValue(const QJsonValue& source, int& result) {
    if (!source.isDouble()) {
        return false;
    }
    const double number = source.toDouble();
    if (!std::isfinite(number) || number < -32768 || number > 65535 || std::floor(number) != number) {
        return false;
    }
    result = static_cast<int>(number);
    return true;
}
} // namespace

DeviceProfiles::DeviceProfiles(QString filePath) : filePath_(std::move(filePath)) {}

bool DeviceProfiles::validSetting(const QString& key, int value) {
    if (key == QStringLiteral("brightness") || key == QStringLiteral("volume") ||
        key == QStringLiteral("battery")) {
        return value >= 0 && value <= 100;
    }
    if (key == QStringLiteral("mute") || key == QStringLiteral("charging")) {
        return value == 0 || value == 1;
    }
    if (key == QStringLiteral("vcp_12") || key == QStringLiteral("vcp_60") ||
        key == QStringLiteral("vcp_dc") || key == QStringLiteral("vcp_14")) {
        return value >= 0 && value <= 65535;
    }
    return n9::validProfileField(key.toStdString(), value) || vgn::validProfileField(key.toStdString(), value);
}

bool DeviceProfiles::validMetadata(const QString& id, const QString& name, const QString& category) {
    return !id.isEmpty() && id.size() <= 4096 && !name.isEmpty() && name.size() <= 1024 &&
           !category.isEmpty() && category.size() <= 64;
}

bool DeviceProfiles::validDirection(const QString& direction) {
    return direction.isEmpty() || direction == QStringLiteral("input") ||
           direction == QStringLiteral("output");
}

bool DeviceProfiles::load() {
    error_.clear();
    const auto reject = [this](QString message) {
        // 解析失败不改变内存中的好数据，也禁止后续扫描自动覆盖损坏的原始文件。
        loadRejected_ = true;
        error_ = std::move(message);
        return false;
    };
    QFile file(filePath_);
    if (!file.exists()) {
        profiles_.clear();
        loadRejected_ = false;
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return reject(QStringLiteral("无法读取设备档案：%1").arg(file.errorString()));
    }
    if (file.size() > maxFileBytes) {
        return reject(QStringLiteral("设备档案超过 1 MiB 限制。"));
    }
    const QByteArray bytes = file.read(maxFileBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maxFileBytes) {
        return reject(QStringLiteral("设备档案读取失败或超过大小限制。"));
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject(QStringLiteral("设备档案 JSON 无效。"));
    }
    const auto root = document.object();
    const auto schema = root.value(QStringLiteral("schema"));
    if (!schema.isDouble() || schema.toDouble() != 1 || !root.value(QStringLiteral("profiles")).isArray()) {
        return reject(QStringLiteral("设备档案版本或结构不受支持。"));
    }
    const auto entries = root.value(QStringLiteral("profiles")).toArray();
    if (entries.size() > maxProfiles) {
        return reject(QStringLiteral("设备档案超过 512 个设备限制。"));
    }
    QMap<QString, SavedDeviceProfile> loaded;
    for (const auto& entry : entries) {
        if (!entry.isObject()) {
            return reject(QStringLiteral("设备档案条目无效。"));
        }
        const auto object = entry.toObject();
        SavedDeviceProfile profile;
        profile.id = object.value(QStringLiteral("id")).toString();
        profile.name = object.value(QStringLiteral("name")).toString();
        profile.category = object.value(QStringLiteral("category")).toString();
        const auto direction = object.value(QStringLiteral("direction"));
        if (!direction.isUndefined() && (!direction.isString() || !validDirection(direction.toString()))) {
            return reject(QStringLiteral("设备档案的音频方向无效。"));
        }
        profile.direction = direction.toString();
        profile.lastSeen =
            QDateTime::fromString(object.value(QStringLiteral("lastSeen")).toString(), Qt::ISODateWithMs);
        if (!validMetadata(profile.id, profile.name, profile.category) || !profile.lastSeen.isValid() ||
            loaded.contains(profile.id) || !object.value(QStringLiteral("settings")).isObject() ||
            !object.value(QStringLiteral("confirmedSettings")).isArray()) {
            return reject(QStringLiteral("设备档案字段缺失、重复或无效。"));
        }
        const auto settings = object.value(QStringLiteral("settings")).toObject();
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            int value = 0;
            if (!integerValue(it.value(), value) || !validSetting(it.key(), value)) {
                return reject(QStringLiteral("设备档案包含不支持的设置或数值。"));
            }
            profile.settings.insert(it.key(), value);
        }
        const auto confirmed = object.value(QStringLiteral("confirmedSettings")).toArray();
        for (const auto& keyValue : confirmed) {
            if (!keyValue.isString() || !profile.settings.contains(keyValue.toString()) ||
                profile.confirmedSettings.contains(keyValue.toString())) {
                return reject(QStringLiteral("设备档案的设置确认标记无效。"));
            }
            profile.confirmedSettings.insert(keyValue.toString());
        }
        loaded.insert(profile.id, std::move(profile));
    }
    profiles_ = std::move(loaded);
    loadRejected_ = false;
    return true;
}

bool DeviceProfiles::save() const {
    if (loadRejected_) {
        error_ = QStringLiteral("设备档案曾读取失败，已保留原文件；修复或移走该文件后重新加载才能保存。");
        return false;
    }
    error_.clear();
    QJsonArray entries;
    for (const auto& profile : profiles_) {
        QJsonObject settings;
        for (auto it = profile.settings.begin(); it != profile.settings.end(); ++it) {
            settings.insert(it.key(), it.value());
        }
        // 固定顺序便于阅读、比较档案变化。
        QStringList confirmedKeys(profile.confirmedSettings.begin(), profile.confirmedSettings.end());
        confirmedKeys.sort();
        QJsonArray confirmed;
        for (const auto& key : confirmedKeys) {
            confirmed.append(key);
        }
        entries.append(
            QJsonObject{{QStringLiteral("id"), profile.id},
                        {QStringLiteral("name"), profile.name},
                        {QStringLiteral("category"), profile.category},
                        {QStringLiteral("direction"), profile.direction},
                        {QStringLiteral("lastSeen"), profile.lastSeen.toUTC().toString(Qt::ISODateWithMs)},
                        {QStringLiteral("settings"), settings},
                        {QStringLiteral("confirmedSettings"), confirmed}});
    }
    const auto bytes =
        QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("profiles"), entries}})
            .toJson(QJsonDocument::Indented);
    if (bytes.size() > maxFileBytes) {
        error_ = QStringLiteral("设备档案超过 1 MiB 限制，未保存。");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath())) {
        error_ = QStringLiteral("无法创建设备档案目录。");
        return false;
    }
    QSaveFile file(filePath_);
    // 不退回直接写原文件，保证失败时旧档案仍然完整。
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error_ = QStringLiteral("保存设备档案失败：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool DeviceProfiles::remove(const QString& id) {
    const auto it = profiles_.constFind(id);
    if (it == profiles_.cend()) {
        error_ = QStringLiteral("设备档案不存在，未删除。");
        return false;
    }
    if (it->connected) {
        error_ = QStringLiteral("请先断开设备，再删除档案。");
        return false;
    }
    const auto previous = *it;
    profiles_.remove(id);
    if (!save()) {
        profiles_.insert(id, previous);
        return false;
    }
    return true;
}

void DeviceProfiles::beginScan() {
    for (auto& profile : profiles_) {
        profile.connected = false;
    }
}

void DeviceProfiles::keepConnected(const QString& id) {
    const auto profile = profiles_.find(id);
    if (profile != profiles_.end()) {
        profile->connected = true;
    }
}

void DeviceProfiles::observe(const QString& id, const QString& name, const QString& category,
                             const QMap<QString, int>& initialValues, const QString& direction) {
    if (!validMetadata(id, name, category)) {
        return;
    }
    if (!profiles_.contains(id) && profiles_.size() >= maxProfiles) {
        error_ = QStringLiteral("设备档案已达到 512 个设备限制。");
        return;
    }
    auto& profile = profiles_[id];
    profile.id = id;
    profile.name = name;
    profile.category = category;
    if (!direction.isEmpty() && validDirection(direction)) {
        profile.direction = direction;
    }
    profile.lastSeen = QDateTime::currentDateTimeUtc();
    profile.connected = true;
    // 重连时仅补充未知字段，不能用设备重置后的默认值覆盖上次保存的目标值。
    for (auto it = initialValues.begin(); it != initialValues.end(); ++it) {
        if (!profile.settings.contains(it.key()) && validSetting(it.key(), it.value())) {
            profile.settings.insert(it.key(), it.value());
            profile.confirmedSettings.insert(it.key());
        }
    }
}

void DeviceProfiles::finishScan() {}

void DeviceProfiles::remember(const QString& id, const QString& key, int value, bool confirmed) {
    auto it = profiles_.find(id);
    if (it == profiles_.end() || !it->connected || !validSetting(key, value)) {
        return;
    }
    it->settings.insert(key, value);
    if (confirmed) {
        it->confirmedSettings.insert(key);
    } else {
        it->confirmedSettings.remove(key);
    }
}

void DeviceProfiles::confirm(const QString& id, const QString& key, int actual) {
    auto it = profiles_.find(id);
    if (it == profiles_.end() || !it->connected || !validSetting(key, actual) ||
        !it->settings.contains(key)) {
        return;
    }
    if (it->settings.value(key) == actual) {
        it->confirmedSettings.insert(key);
    } else {
        it->confirmedSettings.remove(key);
    }
}

QList<SavedDeviceProfile> DeviceProfiles::profiles() const {
    return profiles_.values();
}

std::optional<SavedDeviceProfile> DeviceProfiles::find(const QString& id) const {
    const auto it = profiles_.constFind(id);
    if (it == profiles_.cend()) {
        return std::nullopt;
    }
    return *it;
}

QString DeviceProfiles::errorString() const {
    return error_;
}

} // namespace peripheral::app
