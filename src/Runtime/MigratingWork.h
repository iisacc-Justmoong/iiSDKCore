#pragma once
#include "Runtime.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ii::runtime {
enum class MigrationState { queued,running,paused,completed,failed,cancelled,transferred };
enum class MigrationError { none,invalid,busy,full,stopped,too_large,callback_failed,timed_out,wrong_executor };
struct MigrationStatus final { MigrationState state=MigrationState::queued;std::uint64_t steps=0;MigrationError error=MigrationError::none; };
struct MigrationStep final { std::vector<std::byte> checkpoint;bool complete=false; };
struct MigrationProgram final {
    std::string kind;
    std::uint64_t version=0;
    std::function<MigrationStep(std::span<const std::byte>)> advance;
};

// Move-only within a process. Encoded bytes can be copied, so a transport
// must enforce authorization, ownership fencing and replay protection.
class WorkCheckpoint final {
public:
    WorkCheckpoint()=default;
    WorkCheckpoint(WorkCheckpoint&&) noexcept;
    WorkCheckpoint& operator=(WorkCheckpoint&&) noexcept;
    WorkCheckpoint(const WorkCheckpoint&)=delete;
    WorkCheckpoint& operator=(const WorkCheckpoint&)=delete;
    explicit operator bool() const noexcept { return valid_; }
    [[nodiscard]] std::string_view kind() const noexcept { return valid_?std::string_view(kind_):std::string_view{}; }
    [[nodiscard]] std::uint64_t version() const noexcept { return valid_?version_:0; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return valid_?steps_:0; }
    void reset() noexcept;
    [[nodiscard]] std::vector<std::byte> encode() const;
    [[nodiscard]] static std::expected<WorkCheckpoint,MigrationError> decode(std::span<const std::byte>,std::size_t maximum_bytes);
private:
    friend class MigratingWork;
    std::string kind_;
    std::uint64_t version_=0,steps_=0;
    std::vector<std::byte> data_;
    bool valid_=false;
};

// Each target Runtime is borrowed and must outlive its accepted/active work.
// Steps run without the control lock; a handoff occurs only after a step returns.
class MigratingWork final {
public:
    ~MigratingWork();
    MigratingWork(MigratingWork&&) noexcept;
    MigratingWork& operator=(MigratingWork&&) noexcept;
    MigratingWork(const MigratingWork&)=delete;
    MigratingWork& operator=(const MigratingWork&)=delete;
    [[nodiscard]] static std::expected<MigratingWork,MigrationError> start(
        Runtime&,MigrationProgram,std::vector<std::byte> initial,std::size_t maximum_bytes=1024u*1024u);
    // A failed admission leaves the checkpoint valid and unchanged.
    [[nodiscard]] static std::expected<MigratingWork,MigrationError> resume(
        Runtime&,MigrationProgram,WorkCheckpoint&,std::size_t maximum_bytes=1024u*1024u);
    [[nodiscard]] std::expected<void,MigrationError> migrate(Runtime&,MigrationProgram);
    [[nodiscard]] std::expected<void,MigrationError> pause();
    [[nodiscard]] std::expected<void,MigrationError> resume(Runtime&,MigrationProgram);
    void cancel() noexcept;
    [[nodiscard]] MigrationStatus status() const;
    [[nodiscard]] std::expected<MigrationStatus,MigrationError> wait(std::chrono::nanoseconds timeout) const;
    [[nodiscard]] std::expected<std::vector<std::byte>,MigrationError> result() const;
    [[nodiscard]] std::expected<WorkCheckpoint,MigrationError> take_checkpoint();
private:
    struct State;
    explicit MigratingWork(std::shared_ptr<State> state):state_(std::move(state)){}
    std::shared_ptr<State> state_;
};
} // namespace ii::runtime
