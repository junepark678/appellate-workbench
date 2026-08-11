#pragma once

#include "appellate/sync/object_provider.hpp"

#include <QString>

#include <cstdint>
#include <expected>

namespace appellate::sync {

class LocalFolderProvider final : public ObjectProvider {
  public:
    static constexpr std::uint64_t default_maximum_ciphertext_bytes =
        4ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t maximum_page_size = 1'000;

    [[nodiscard]] static auto
    open(const QString& root_directory,
         std::uint64_t maximum_ciphertext_bytes = default_maximum_ciphertext_bytes)
        -> std::expected<LocalFolderProvider, ProviderError>;

    [[nodiscard]] auto list(QStringView after_remote_object_id, std::size_t limit) const
        -> std::expected<ProviderListPage, ProviderError> override;
    [[nodiscard]] auto stat(QStringView remote_object_id) const
        -> std::expected<ProviderObjectMetadata, ProviderError> override;
    [[nodiscard]] auto createIfAbsent(QStringView remote_object_id, QIODevice& ciphertext,
                                      std::uint64_t ciphertext_bytes)
        -> std::expected<ProviderCreateResult, ProviderError> override;
    [[nodiscard]] auto download(QStringView remote_object_id, QIODevice& destination) const
        -> std::expected<ProviderObjectMetadata, ProviderError> override;

    [[nodiscard]] const QString& rootDirectory() const noexcept;

  private:
    explicit LocalFolderProvider(QString root_directory, std::uint64_t maximum_ciphertext_bytes);

    [[nodiscard]] QString objectPath(QStringView remote_object_id) const;
    [[nodiscard]] QString prefixDirectory(QStringView remote_object_id) const;

    QString root_directory_;
    QString objects_directory_;
    std::uint64_t maximum_ciphertext_bytes_{};
};

} // namespace appellate::sync
