#pragma once

#include "appellate/storage/session_archive.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace appellate::packs {
class ResolvedPack;
}

namespace appellate::ui {

enum class SessionArchiveProviderErrorCode {
    InvalidArgument,
    ArchiveFailure,
    IncompatibleRevisionPins,
    UnsupportedSession,
    ReplayFailure,
    TemporaryAssetStoreFailure,
};

struct SessionArchiveProviderError final {
    SessionArchiveProviderErrorCode code;
    QString message;
    std::optional<storage::SessionArchiveErrorCode> storage_code{};

    friend bool operator==(const SessionArchiveProviderError&,
                           const SessionArchiveProviderError&) = default;
};

struct ValidatedSessionArchive final {
    storage::SessionArchiveManifest manifest;
    std::vector<storage::RevisionPin> available_revision_pins;
};

// Product boundary for workflow/oral session archives. Storage validates the generic envelope;
// implementations must additionally bind every session to one exact resolved closure and replay
// its engine semantics before permitting the create-only import.
class SessionArchiveProvider {
  public:
    virtual ~SessionArchiveProvider() = default;

    [[nodiscard]] virtual auto exportAll() const
        -> std::expected<QByteArray, SessionArchiveProviderError> = 0;

    [[nodiscard]] virtual auto read(QByteArrayView archive) const
        -> std::expected<storage::SessionArchiveReplayContents, SessionArchiveProviderError> = 0;

    [[nodiscard]] virtual auto
    validate(const storage::SessionArchiveReplayContents& contents,
             std::span<const packs::ResolvedPack* const> available_closures) const
        -> std::expected<ValidatedSessionArchive, SessionArchiveProviderError> = 0;

    // Imports new session IDs only. The implementation repeats bounded reading and semantic
    // validation so a caller cannot bypass replay with a forged validation result.
    [[nodiscard]] virtual auto
    import(QByteArrayView archive, std::span<const packs::ResolvedPack* const> available_closures)
        -> std::expected<storage::SessionArchiveManifest, SessionArchiveProviderError> = 0;
};

} // namespace appellate::ui
