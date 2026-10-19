#pragma once
#include "vgn_protocol.h"
#include <span>
#include <string_view>

namespace peripheral::vgn {
// F2 Master+ CID 118 / MID 35 only. These are read formats, not write ranges
// or a promise that another mouse sharing the receiver PID uses this layout.
struct DpiStage { int x = 0, y = 0; };
struct ButtonMapping { int actionClass = 0, parameter = 0; };
struct Profile {
    int pollingRateHz = 0;
    int activeStage = 0; // Zero based; stage count is stages.size().
    std::vector<DpiStage> stages;
    int lodCode = 0;
    int debounceMs = 0;
    int sleepSeconds = 0;
    bool motionSync = false, angleSnapping = false, rippleControl = false;
    int performanceCode = 0;
    std::array<ButtonMapping, 6> buttons{};
};
struct ReadBlock {
    std::uint16_t address;
    std::uint8_t length;
    bool operator==(const ReadBlock&) const = default;
};
// Read only the settings, performance fields and six physical button records
// observed on the user's receiver; never scan arbitrary device memory.
inline constexpr std::array<ReadBlock, 10> kProfileReads{{
    {0,10}, {10,10}, {20,10}, {30,10}, {40,4},
    {0xa8,10}, {0xb2,5}, {0x60,10}, {0x6a,10}, {0x74,4}}};
// Flash addresses and the write frame observed in the vendor WebHID bundle.
// These are candidates: see docs/PRIVATE_WRITE_CANDIDATES_2026-10-19.md. They
// grant no permission, and no caller may enable a write before a read-back and
// physical effect are recorded for the exact model.
constexpr std::uint8_t kFlashWriteCommand = 7;
constexpr std::uint16_t kFlashReportRate = 0;
constexpr std::uint16_t kFlashStageCount = 2;
constexpr std::uint16_t kFlashActiveStage = 4;
constexpr std::uint16_t kFlashDpiBase = 12;
constexpr std::size_t kFlashDpiStages = 8;

Report identityQuery();
bool matchesF2Identity(std::span<const std::uint8_t> response);
std::optional<Report> profileQuery(ReadBlock block);
// All framing, status, checksum, address and length must match this query.
bool matchesProfileReply(std::span<const std::uint8_t> response, ReadBlock block);
std::optional<DpiStage> decodeDpi(std::span<const std::uint8_t> record);
std::optional<int> decodePollingRate(int raw);

class ProfileCapture {
  public:
    bool accept(ReadBlock block, std::span<const std::uint8_t> response);
    // No default DPI/rate, clamping, or data from a previous capture on failure.
    std::optional<Profile> decode() const;
  private:
    std::array<std::uint8_t, 0xb7> bytes_{};
    std::array<bool, kProfileReads.size()> received_{};
};
// Strict keys for storing read-back values. This grants no write permission.
bool validProfileField(std::string_view key, int value);

// The two record formats that a whitelisted flash write may carry.
// A scalar is the value plus its complement, summing to 0x55.
std::array<std::uint8_t, 2> encodeScalarRecord(int value);
// One 4-byte DPI record. Values that no enabled stage format represents
// exactly are rejected instead of rounded to a nearby DPI.
std::optional<std::array<std::uint8_t, 4>> encodeDpiRecord(int x, int y);
// Inverse of decodePollingRate for the write code.
std::optional<int> encodePollingRate(int hertz);
// Builds one write report for an address/length pair this build may write.
// Any other address, length, or payload size returns nullopt.
std::optional<Report> flashWriteQuery(std::uint16_t address, std::span<const std::uint8_t> data);
} // namespace peripheral::vgn
