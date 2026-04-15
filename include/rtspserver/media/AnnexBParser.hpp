/**
 * @file AnnexBParser.hpp
 * @brief Utilities for slicing and dicing Annex-B formatted byte streams.
 *
 * Header-only: all implementations are inlined here — no separate .cpp needed.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rtspserver::media {

namespace types {
    /// Zero-copy NAL views into an existing buffer.
    /// IMPORTANT: the source buffer must outlive all spans.
    using NalUnitViews = std::vector<std::span<const uint8_t>>;

    /// Owning copies of each NAL unit payload, start code exclued
    using NalUnitList = std::vector<std::vector<uint8_t>>;
} // namespace types

/**
 * @brief Non-virtual polymorphic interface for NAL-unit parsers.
 *
 * Concrete parsers inherit as:
 * @code
 *   class MyParser : public INalParser<MyParser> { ... };
 * @endcode
 *
 * Required private hooks (accessible via friend or public):
 *   - types::NalUnitViews splitViewImpl(std::span<const uint8_t>) const
 *   - types::NalUnitList  splitImpl    (std::span<const uint8_t>) const
 */
template <typename Derived>
class INalParser {
public:
    /// Split a buffer into zero-copy NAL-unit views with stripped start code
    types::NalUnitViews splitView(std::span<const uint8_t> buf) const
    {
        return static_cast<const Derived*>(this)->splitViewImpl(buf);
    }

    /// Split a buffer into owning copies of each NAL unit.
    types::NalUnitList split(std::span<const uint8_t> buf) const
    {
        return static_cast<const Derived*>(this)->splitImpl(buf);
    }

protected:
    ~INalParser() = default;
};

/**
 * @brief Parser for Annex-B (ISO 14496-10) byte streams.
 *
 * Handles 3-byte (0x000001) and 4-byte (0x00000001) start codes, including
 * mixed lengths within the same stream.
 *
 * @note Emulation-prevention bytes are left intact; removal is the
 *       responsibility of the downstream decoder.
 * @note Stateless utility class. Use through INalParser<AnnexBParser> for
 *       generic code, or call the static helpers directly.
 */
class AnnexBParser : public INalParser<AnnexBParser> {
    friend class INalParser<AnnexBParser>;

public:
    AnnexBParser() = default;

    /// Raw-pointer overload — zero overhead for C-API callers.
    static types::NalUnitViews splitView(const uint8_t* data, size_t size) noexcept;

    /// Span overload (delegates to the raw-pointer overload).
    static types::NalUnitViews splitView(std::span<const uint8_t> buf) noexcept
    {
        return splitView(buf.data(), buf.size());
    }

    /// Owning split: deep copy of each NAL unit payload.
    static types::NalUnitList split(std::span<const uint8_t> buf) noexcept;

    /// Returns the start-code length (3 or 4) at @p offset, or 0 if absent.
    static size_t startCodeLength(const uint8_t* data, size_t size,
        size_t offset) noexcept;

private:
    // CRTP implementation hooks — delegate to the static helpers.
    types::NalUnitViews splitViewImpl(std::span<const uint8_t> buf) const
    {
        return AnnexBParser::splitView(buf);
    }
    types::NalUnitList splitImpl(std::span<const uint8_t> buf) const
    {
        return AnnexBParser::split(buf);
    }
};

inline size_t AnnexBParser::startCodeLength(const uint8_t* data, size_t size,
    size_t offset) noexcept
{
    if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1)
        return 4;
    if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1)
        return 3;
    return 0;
}

inline types::NalUnitViews
AnnexBParser::splitView(const uint8_t* data, size_t size) noexcept
{
    types::NalUnitViews nals;

    size_t sc = startCodeLength(data, size, 0);
    size_t i = sc;
    size_t nal_start = sc ? sc : size; // sentinel: no leading start code yet

    while (i < size) {
        size_t sc_len = startCodeLength(data, size, i);
        if (sc_len > 0) {
            if (nal_start < i)
                nals.emplace_back(data + nal_start, i - nal_start);
            i += sc_len;
            nal_start = i;
        } else {
            ++i;
        }
    }
    if (nal_start < size)
        nals.emplace_back(data + nal_start, size - nal_start);

    return nals;
}

inline types::NalUnitList
AnnexBParser::split(std::span<const uint8_t> buf) noexcept
{
    const uint8_t* data = buf.data();
    const size_t size = buf.size();
    types::NalUnitList nals;

    size_t sc = startCodeLength(data, size, 0);
    size_t i = sc;
    size_t nal_start = sc ? sc : size; // sentinel

    while (i < size) {
        size_t sc_len = startCodeLength(data, size, i);
        if (sc_len > 0) {
            if (nal_start < i)
                nals.emplace_back(data + nal_start, data + i);
            i += sc_len;
            nal_start = i;
        } else {
            ++i;
        }
    }
    if (nal_start < size)
        nals.emplace_back(data + nal_start, data + size);

    return nals;
}

} // namespace rtspserver::media