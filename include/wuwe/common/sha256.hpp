#ifndef WUWE_COMMON_SHA256_HPP
#define WUWE_COMMON_SHA256_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace wuwe::common {

class sha256 {
public:
  using digest_type = std::array<std::uint8_t, 32>;

  sha256() = default;

  void update(std::span<const std::byte> input) noexcept {
    total_size_ += static_cast<std::uint64_t>(input.size());
    while (!input.empty()) {
      const auto available = block_.size() - block_size_;
      const auto count = input.size() < available ? input.size() : available;
      for (std::size_t i = 0; i < count; ++i) {
        block_[block_size_ + i] = std::to_integer<std::uint8_t>(input[i]);
      }
      block_size_ += count;
      input = input.subspan(count);
      if (block_size_ == block_.size()) {
        transform(block_);
        block_size_ = 0;
      }
    }
  }

  void update(std::span<const std::uint8_t> input) noexcept {
    update(std::as_bytes(input));
  }

  void update(std::string_view input) noexcept {
    update(std::as_bytes(std::span(input.data(), input.size())));
  }

  [[nodiscard]] digest_type digest() const noexcept {
    auto copy = *this;
    return copy.finalize();
  }

  [[nodiscard]] std::string hex_digest() const {
    return to_hex(digest());
  }

  [[nodiscard]] static std::string to_hex(const digest_type& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(value.size() * 2, '0');
    for (std::size_t i = 0; i < value.size(); ++i) {
      result[i * 2] = digits[value[i] >> 4U];
      result[i * 2 + 1] = digits[value[i] & 0x0fU];
    }
    return result;
  }

private:
  static constexpr std::array<std::uint32_t, 64> round_constants_ {
    0x428a2f98U,
    0x71374491U,
    0xb5c0fbcfU,
    0xe9b5dba5U,
    0x3956c25bU,
    0x59f111f1U,
    0x923f82a4U,
    0xab1c5ed5U,
    0xd807aa98U,
    0x12835b01U,
    0x243185beU,
    0x550c7dc3U,
    0x72be5d74U,
    0x80deb1feU,
    0x9bdc06a7U,
    0xc19bf174U,
    0xe49b69c1U,
    0xefbe4786U,
    0x0fc19dc6U,
    0x240ca1ccU,
    0x2de92c6fU,
    0x4a7484aaU,
    0x5cb0a9dcU,
    0x76f988daU,
    0x983e5152U,
    0xa831c66dU,
    0xb00327c8U,
    0xbf597fc7U,
    0xc6e00bf3U,
    0xd5a79147U,
    0x06ca6351U,
    0x14292967U,
    0x27b70a85U,
    0x2e1b2138U,
    0x4d2c6dfcU,
    0x53380d13U,
    0x650a7354U,
    0x766a0abbU,
    0x81c2c92eU,
    0x92722c85U,
    0xa2bfe8a1U,
    0xa81a664bU,
    0xc24b8b70U,
    0xc76c51a3U,
    0xd192e819U,
    0xd6990624U,
    0xf40e3585U,
    0x106aa070U,
    0x19a4c116U,
    0x1e376c08U,
    0x2748774cU,
    0x34b0bcb5U,
    0x391c0cb3U,
    0x4ed8aa4aU,
    0x5b9cca4fU,
    0x682e6ff3U,
    0x748f82eeU,
    0x78a5636fU,
    0x84c87814U,
    0x8cc70208U,
    0x90befffaU,
    0xa4506cebU,
    0xbef9a3f7U,
    0xc67178f2U,
  };

  static std::uint32_t load_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
  }

  static void store_be32(std::uint32_t value, std::uint8_t* output) noexcept {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
  }

  void transform(const std::array<std::uint8_t, 64>& block) noexcept {
    std::array<std::uint32_t, 64> words {};
    for (std::size_t i = 0; i < 16; ++i) {
      words[i] = load_be32(block.data() + i * 4);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
      const auto s0 =
        std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3U);
      const auto s1 =
        std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10U);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
      const auto sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto temporary1 = h + sigma1 + choose + round_constants_[i] + words[i];
      const auto sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  digest_type finalize() noexcept {
    const auto message_size = total_size_;
    block_[block_size_++] = 0x80U;
    if (block_size_ > 56) {
      while (block_size_ < block_.size()) {
        block_[block_size_++] = 0;
      }
      transform(block_);
      block_size_ = 0;
    }
    while (block_size_ < 56) {
      block_[block_size_++] = 0;
    }
    const auto bit_size = message_size * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
      block_[63 - i] = static_cast<std::uint8_t>(bit_size >> (i * 8U));
    }
    transform(block_);

    digest_type output {};
    for (std::size_t i = 0; i < state_.size(); ++i) {
      store_be32(state_[i], output.data() + i * 4);
    }
    return output;
  }

  std::array<std::uint32_t, 8> state_ {
    0x6a09e667U,
    0xbb67ae85U,
    0x3c6ef372U,
    0xa54ff53aU,
    0x510e527fU,
    0x9b05688cU,
    0x1f83d9abU,
    0x5be0cd19U,
  };
  std::array<std::uint8_t, 64> block_ {};
  std::size_t block_size_ { 0 };
  std::uint64_t total_size_ { 0 };
};

[[nodiscard]] inline std::string sha256_hex(std::span<const std::byte> input) {
  sha256 hash;
  hash.update(input);
  return hash.hex_digest();
}

[[nodiscard]] inline std::string sha256_hex(std::string_view input) {
  sha256 hash;
  hash.update(input);
  return hash.hex_digest();
}

} // namespace wuwe::common

#endif // WUWE_COMMON_SHA256_HPP
