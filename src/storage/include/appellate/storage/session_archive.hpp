#pragma once

#include "appellate/storage/session_store.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <expected>
#include <vector>

namespace appellate::storage {

class AssetStore;

enum class SessionArchiveErrorCode {
    InvalidArgument,
    LimitExceeded,
    MalformedArchive,
    DigestMismatch,
    SessionConflict,
    IncompatibleRevisionPins,
    SessionStoreFailure,
    AssetStoreFailure,
};

struct SessionArchiveError final {
    SessionArchiveErrorCode code;
    QString message;

    friend bool operator==(const SessionArchiveError&, const SessionArchiveError&) = default;
};

struct SessionArchiveLimits final {
    static constexpr qsizetype default_maximum_archive_bytes = 512LL * 1024LL * 1024LL;
    static constexpr std::size_t default_maximum_sessions = 4096U;
    static constexpr std::size_t default_maximum_rows_per_session = 100'000U;
    static constexpr std::size_t default_maximum_total_rows = 100'000U;
    static constexpr std::size_t default_maximum_assets = 4096U;
    static constexpr qint64 default_maximum_total_asset_bytes = 512LL * 1024LL * 1024LL;

    qsizetype maximum_archive_bytes{default_maximum_archive_bytes};
    std::size_t maximum_sessions{default_maximum_sessions};
    std::size_t maximum_rows_per_session{default_maximum_rows_per_session};
    std::size_t maximum_total_rows{default_maximum_total_rows};
    std::size_t maximum_assets{default_maximum_assets};
    qint64 maximum_total_asset_bytes{default_maximum_total_asset_bytes};
};

struct SessionArchiveImportOptions final {
    SessionArchiveLimits limits{};

    // Storage deliberately does not depend on the pack catalog. Callers must provide the exact
    // revisions they proved available; every archived pin must match an entry exactly. The
    // fail-closed default rejects every real session because persisted sessions require pins.
    std::vector<RevisionPin> available_revision_pins{};
};

struct SessionArchiveSessionManifest final {
    QString session_id;
    QString engine_revision;
    SessionAuthorityContract authority_contract{SessionAuthorityContract::LegacyV1};
    qint64 sequence{};
    QString created_at_utc;
    std::vector<RevisionPin> pins;
    std::size_t command_count{};
    std::size_t event_count{};
    std::size_t docket_count{};
    std::size_t asset_reference_count{};
};

struct SessionArchiveManifest final {
    QString archive_sha256;
    std::vector<SessionArchiveSessionManifest> sessions;
    QStringList asset_digests;
    qint64 total_asset_bytes{};
};

struct SessionArchiveAssetContents final {
    QString digest;
    QByteArray bytes;
};

struct SessionArchiveReplayContents final {
    SessionArchiveManifest manifest;
    std::vector<SessionSnapshot> sessions;
    std::vector<SessionArchiveAssetContents> assets;
};

class SessionArchive final {
  public:
    SessionArchive() = delete;

    // An empty selection exports every session. Explicit selections reject duplicates and
    // missing session IDs. The returned bytes contain no paths or filesystem metadata.
    [[nodiscard]] static auto exportSessions(const SessionStore& session_store,
                                             const AssetStore& asset_store,
                                             const QStringList& selected_session_ids = {},
                                             const SessionArchiveLimits& limits = {})
        -> std::expected<QByteArray, SessionArchiveError>;

    // Performs the same bounded envelope and content validation as import without opening or
    // changing a database or CAS.
    [[nodiscard]] static auto inspect(QByteArrayView archive,
                                      const SessionArchiveLimits& limits = {})
        -> std::expected<SessionArchiveManifest, SessionArchiveError>;

    // Returns the already bounded and structurally validated commands, events, projections, and
    // exact asset bytes needed by a product-facing caller to replay every pinned engine before
    // import. The returned content is untrusted until that caller-specific replay succeeds.
    [[nodiscard]] static auto readForReplay(QByteArrayView archive,
                                            const SessionArchiveLimits& limits = {})
        -> std::expected<SessionArchiveReplayContents, SessionArchiveError>;

    // The complete archive is integrity-checked, decoded, and validated before either store is
    // changed. The unkeyed digest detects corruption; it does not authenticate an archive.
    // Storage validates its generic persistence contract, but does not replay engine semantics;
    // product-facing callers must validate the manifest's exact pack closure and replay each
    // session with its pinned engine before invoking this method.
    // Imported session IDs must be new. Database rows and referenced CAS objects are then
    // committed as one paired operation under the stores' existing locks.
    [[nodiscard]] static auto importSessions(QByteArrayView archive, SessionStore& session_store,
                                             AssetStore& asset_store,
                                             const SessionArchiveImportOptions& options = {})
        -> std::expected<void, SessionArchiveError>;

  private:
    [[nodiscard]] static auto
    verifyAssetPair(const SessionStore& session_store, const AssetStore& asset_store,
                    const AssetStoreLock& lock, const QStringList& referenced_digests)
        -> std::expected<void, SessionArchiveError>;
};

} // namespace appellate::storage
