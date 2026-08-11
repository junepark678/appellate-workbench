#pragma once

#include <QString>
#include <QStringView>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

class QIODevice;

namespace appellate::sync {

struct ProviderObjectMetadata final {
    QString remote_object_id;
    std::uint64_t ciphertext_bytes{};

    friend bool operator==(const ProviderObjectMetadata&, const ProviderObjectMetadata&) = default;
};

struct ProviderListPage final {
    std::vector<ProviderObjectMetadata> objects;
    QString continuation_token;

    friend bool operator==(const ProviderListPage&, const ProviderListPage&) = default;
};

enum class ProviderCreateResult {
    Created,
    AlreadyPresent,
};

enum class ProviderErrorCode {
    InvalidArgument,
    CannotCreateNamespace,
    CannotReadNamespace,
    NotFound,
    InvalidObject,
    ObjectTooLarge,
    SourceReadFailed,
    DestinationWriteFailed,
    PublicationFailed,
};

struct ProviderError final {
    ProviderErrorCode code{};
    QString message;

    friend bool operator==(const ProviderError&, const ProviderError&) = default;
};

class ObjectProvider {
  public:
    virtual ~ObjectProvider() = default;

    [[nodiscard]] virtual auto list(QStringView after_remote_object_id, std::size_t limit) const
        -> std::expected<ProviderListPage, ProviderError> = 0;
    [[nodiscard]] virtual auto stat(QStringView remote_object_id) const
        -> std::expected<ProviderObjectMetadata, ProviderError> = 0;
    [[nodiscard]] virtual auto createIfAbsent(QStringView remote_object_id, QIODevice& ciphertext,
                                              std::uint64_t ciphertext_bytes)
        -> std::expected<ProviderCreateResult, ProviderError> = 0;
    [[nodiscard]] virtual auto download(QStringView remote_object_id, QIODevice& destination) const
        -> std::expected<ProviderObjectMetadata, ProviderError> = 0;
};

} // namespace appellate::sync
