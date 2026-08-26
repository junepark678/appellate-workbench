#pragma once

#include <QString>

#include <cstddef>
#include <expected>
#include <span>

namespace appellate::sync {

enum class SecretMemoryErrorCode {
    CryptoInitializationFailed,
    AllocationFailed,
};

struct SecretMemoryError final {
    SecretMemoryErrorCode code{};
    QString message;

    friend bool operator==(const SecretMemoryError&, const SecretMemoryError&) = default;
};

// A guarded, best-effort locked allocation for short-lived secret material. The allocation is
// never implicitly shared or copied and is wiped by sodium_free().
class SecretBytes final {
  public:
    SecretBytes() = default;
    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;
    SecretBytes(SecretBytes&& other) noexcept;
    SecretBytes& operator=(SecretBytes&& other) noexcept;
    ~SecretBytes();

    [[nodiscard]] static auto allocate(std::size_t size)
        -> std::expected<SecretBytes, SecretMemoryError>;
    [[nodiscard]] static auto copyOf(std::span<const unsigned char> bytes)
        -> std::expected<SecretBytes, SecretMemoryError>;

    [[nodiscard]] auto bytes() const noexcept -> std::span<const unsigned char>;
    [[nodiscard]] auto mutableBytes() noexcept -> std::span<unsigned char>;
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto empty() const noexcept -> bool;

    void clear() noexcept;

  private:
    SecretBytes(unsigned char* data, std::size_t size) noexcept;

    unsigned char* data_{};
    std::size_t size_{};
};

} // namespace appellate::sync
