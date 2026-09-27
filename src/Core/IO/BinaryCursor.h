#pragma once
#include "../Result/Result.h"
#include <bit>
#include <concepts>
#include <cstddef>
#include <span>

namespace ii::core {
template<class T> concept BinaryInteger=std::integral<T> && (!std::same_as<T,bool>);
class ByteReader final {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size()-position_; }
    [[nodiscard]] Result<std::span<const std::byte>> take(std::size_t size) {
        if (size>remaining()) return fail(ErrorCode::out_of_range,"Binary read exceeds buffer");
        auto result=bytes_.subspan(position_,size); position_+=size; return result;
    }
    template<BinaryInteger T> [[nodiscard]] Result<T> read(std::endian order=std::endian::little) {
        if (order!=std::endian::little && order!=std::endian::big) return fail(ErrorCode::invalid_argument,"Invalid byte order");
        auto data=take(sizeof(T)); if (!data) return std::unexpected(data.error());
        using U=std::make_unsigned_t<T>;
        U value=0;
        for (std::size_t i=0;i<sizeof(T);++i) {
            const auto shift=(order==std::endian::little ? i : sizeof(T)-1-i)*8;
            value=static_cast<U>(value | static_cast<U>(static_cast<U>(std::to_integer<unsigned>((*data)[i]))<<shift));
        }
        return std::bit_cast<T>(value);
    }
private:
    std::span<const std::byte> bytes_;
    std::size_t position_=0;
};
class ByteWriter final {
public:
    explicit ByteWriter(std::span<std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size()-position_; }
    template<BinaryInteger T> [[nodiscard]] Result<void> write(T input, std::endian order=std::endian::little) {
        if (order!=std::endian::little && order!=std::endian::big) return fail(ErrorCode::invalid_argument,"Invalid byte order");
        if (sizeof(T)>remaining()) return fail(ErrorCode::out_of_range,"Binary write exceeds buffer");
        const auto value=std::bit_cast<std::make_unsigned_t<T>>(input);
        for (std::size_t i=0;i<sizeof(T);++i) {
            const auto shift=(order==std::endian::little ? i : sizeof(T)-1-i)*8;
            bytes_[position_+i]=static_cast<std::byte>((value>>shift)&0xffu);
        }
        position_+=sizeof(T); return {};
    }
private:
    std::span<std::byte> bytes_;
    std::size_t position_=0;
};
} // namespace ii::core
