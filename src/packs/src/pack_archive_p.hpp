#pragma once

#include "appellate/packs/pack_archive.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace appellate::packs::detail {

enum class SecureScratchEvent {
    TempPathCaptured,
    ControllerOpened,
    ControllerRebound,
    AccessAclProbe,
    DefaultAclProbe,
    NameCandidate,
    NameCollision,
    MemberFeasibility,
    DirectoryCreate,
    DirectoryRetainOpen,
    DirectoryRetained,
    DirectoryNormalize,
    DirectoryRebound,
    DirectorySync,
    FileCreate,
    FileRetainRebind,
    FileRetained,
    FileNormalize,
    FileRebound,
    FileWrite,
    FileSync,
    BeforeReader,
    AfterReader,
    InventoryRebind,
    DirectoryEnumerateOpen,
    DirectoryEnumerateRead,
    DirectoryEnumerateRebind,
    CleanupInspect,
    CleanupRemove,
    CleanupSync,
};

inline constexpr std::size_t secure_scratch_nonce_hex_characters = 32;

enum class SecureScratchCleanupOutcome {
    NotAttempted,
    Removed,
    Preserved,
};

enum class SecureScratchFailureCode {
    EnvironmentInfeasible,
    OperationalFailure,
    InvalidConfiguration,
};

struct SecureScratchFailure final {
    SecureScratchFailureCode code{};
    QString message;
};

enum class SecureScratchInjectedAction {
    Continue,
    FailBefore,
    FailAfter,
};

struct SecureScratchObservation final {
    SecureScratchEvent event{};
    QString absolute_path;
    QByteArray component;
    std::size_t attempt{};
    unsigned int mode_before{};
    unsigned int mode_after{};

    SecureScratchObservation() = default;
    explicit SecureScratchObservation(SecureScratchEvent observed_event, QString observed_path = {},
                                      QByteArray observed_component = {},
                                      std::size_t observed_attempt = 0,
                                      unsigned int observed_mode_before = 0,
                                      unsigned int observed_mode_after = 0)
        : event(observed_event), absolute_path(std::move(observed_path)),
          component(std::move(observed_component)), attempt(observed_attempt),
          mode_before(observed_mode_before), mode_after(observed_mode_after) {}
};

struct SecureScratchHooks final {
    std::function<QString()> temp_path_provider;
    std::function<QByteArray(std::size_t attempt)> name_source;
    std::function<SecureScratchInjectedAction(const SecureScratchObservation&)> inject;
    std::function<void(const SecureScratchObservation&)> observe;
    struct SecureScratchReport* report{};
};

struct SecureScratchReport final {
    QString captured_temp_path;
    QString workspace_path;
    SecureScratchCleanupOutcome cleanup{SecureScratchCleanupOutcome::NotAttempted};
    std::optional<SecureScratchFailureCode> failure_code;
    QStringList remaining_ledger_paths;
    QList<QByteArray> unexpected_raw_paths;
    bool residue_identity_ambiguous{};
};

class SecureScratchContext final {
  public:
    SecureScratchContext(const SecureScratchContext&) = delete;
    SecureScratchContext& operator=(const SecureScratchContext&) = delete;
    SecureScratchContext(SecureScratchContext&&) noexcept;
    SecureScratchContext& operator=(SecureScratchContext&&) noexcept;
    ~SecureScratchContext();

    [[nodiscard]] bool isValid() const;

  private:
    struct Impl;
    explicit SecureScratchContext(std::unique_ptr<Impl> state);
    std::unique_ptr<Impl> impl_;

    friend std::expected<SecureScratchContext, SecureScratchFailure>
    acquireSecureScratchContext(const SecureScratchHooks& hooks);
    friend class SecureScratchWorkspace;
};

[[nodiscard]] bool isValidSecureScratchName(QByteArrayView name);

[[nodiscard]] std::expected<SecureScratchContext, SecureScratchFailure>
acquireSecureScratchContext(const SecureScratchHooks& hooks = {});

// Private test/catalog seam. This is deliberately absent from the installed include tree.
[[nodiscard]] std::expected<LoadedPack, Error>
importArchiveThroughSecureScratch(const QString& archive_path, PackArchiveLimits limits,
                                  PackValidationScope scope, const SecureScratchHooks& hooks);

} // namespace appellate::packs::detail
