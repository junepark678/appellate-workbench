#pragma once

#include "oral_argument_launch_provider.hpp"
#include "workflow_launch_provider.hpp"

#include "appellate/storage/session_store.hpp"

#include <QDateTime>
#include <QString>

#include <expected>
#include <functional>
#include <memory>

namespace appellate::ui {

struct LocalSessionPaths final {
    QString database_path;
    QString asset_root;

    friend bool operator==(const LocalSessionPaths&, const LocalSessionPaths&) = default;
};

// Canonical, versioned digest of every field in a persisted workflow snapshot. Oral-argument
// sessions pin this digest and cannot mutate the workflow that produced it.
[[nodiscard]] QString workflowLegalStateDigest(const storage::SessionSnapshot& snapshot);

class LocalSessionProvider final : public OralArgumentLaunchProvider,
                                   public WorkflowLaunchProvider {
  public:
    using UtcClock = std::function<QDateTime()>;

    [[nodiscard]] static auto fromStandardPaths(UtcClock clock = {})
        -> std::expected<std::shared_ptr<LocalSessionProvider>, QString>;
    [[nodiscard]] static auto create(LocalSessionPaths paths, UtcClock clock = {})
        -> std::expected<std::shared_ptr<LocalSessionProvider>, QString>;

    [[nodiscard]] auto openWorkflow(const packs::ResolvedPack& resolved_pack,
                                    const model::CaseId& case_id)
        -> std::expected<std::unique_ptr<app::WorkflowSessionController>,
                         app::WorkflowSessionError> override;

    [[nodiscard]] auto open(const packs::ResolvedPack& resolved_pack, const model::CaseId& case_id,
                            const packs::RuntimeArgumentConfigId& argument_configuration_id)
        -> std::expected<std::unique_ptr<app::OralArgumentSessionController>,
                         app::OralArgumentSessionError> override;

    [[nodiscard]] const LocalSessionPaths& paths() const noexcept;

  private:
    explicit LocalSessionProvider(LocalSessionPaths paths, UtcClock clock,
                                  std::unique_ptr<storage::SessionStore> owner_store);

    [[nodiscard]] auto nowUtc() const -> std::expected<QString, QString>;

    LocalSessionPaths paths_;
    UtcClock clock_;
    std::unique_ptr<storage::SessionStore> owner_store_;
};

} // namespace appellate::ui
