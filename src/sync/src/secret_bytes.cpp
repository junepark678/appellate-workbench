#include "appellate/sync/secret_bytes.hpp"

#include <sodium.h>

#include <cstring>
#include <utility>

namespace appellate::sync {
namespace {

[[nodiscard]] auto fail(SecretMemoryErrorCode code, QString message)
    -> std::unexpected<SecretMemoryError> {
    return std::unexpected(SecretMemoryError{code, std::move(message)});
}

} // namespace

SecretBytes::SecretBytes(unsigned char* data, std::size_t size) noexcept
    : data_(data), size_(size) {}

SecretBytes::SecretBytes(SecretBytes&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0U)) {}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0U);
    }
    return *this;
}

SecretBytes::~SecretBytes() { clear(); }

std::expected<SecretBytes, SecretMemoryError> SecretBytes::allocate(std::size_t size) {
    if (sodium_init() < 0) {
        return fail(SecretMemoryErrorCode::CryptoInitializationFailed,
                    QStringLiteral("Cannot initialize protected secret memory"));
    }
    if (size == 0U) {
        return SecretBytes{};
    }
    auto* allocation = static_cast<unsigned char*>(sodium_malloc(size));
    if (allocation == nullptr) {
        return fail(SecretMemoryErrorCode::AllocationFailed,
                    QStringLiteral("Cannot allocate protected secret memory"));
    }
    std::memset(allocation, 0, size);
    return SecretBytes{allocation, size};
}

std::expected<SecretBytes, SecretMemoryError>
SecretBytes::copyOf(std::span<const unsigned char> bytes) {
    auto copy = allocate(bytes.size());
    if (!copy) {
        return std::unexpected(copy.error());
    }
    if (!bytes.empty()) {
        std::memcpy(copy->data_, bytes.data(), bytes.size());
    }
    return copy;
}

std::span<const unsigned char> SecretBytes::bytes() const noexcept { return {data_, size_}; }

std::span<unsigned char> SecretBytes::mutableBytes() noexcept { return {data_, size_}; }

std::size_t SecretBytes::size() const noexcept { return size_; }

bool SecretBytes::empty() const noexcept { return size_ == 0U; }

void SecretBytes::clear() noexcept {
    if (data_ != nullptr) {
        sodium_memzero(data_, size_);
        sodium_free(data_);
        data_ = nullptr;
    }
    size_ = 0U;
}

} // namespace appellate::sync
