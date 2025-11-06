#include <gtest/gtest.h>
#include "urpc/Wire.h"

#include <gtest/gtest.h>
#include <span>
#include <vector>
#include <cstring>
#include "urpc/Wire.h"
#include "urpc/Codec.h"

using namespace urpc;

TEST(Wire, EncodeDecodeHeaderAndPayload)
{
    UrpcHdr hdr{};
    hdr.type = static_cast<uint8_t>(MsgType::REQUEST);
    hdr.stream = 1;
    hdr.method = fnv1a64_fast("hello");
    hdr.timeout_ms = 5000;

    const std::string meta_str = R"({"user":"alice"})";
    const std::string body_str = R"({"msg":"hello"})";

    const std::span<const std::byte> meta{
        reinterpret_cast<const std::byte*>(meta_str.data()), meta_str.size()
    };
    const std::span<const std::byte> body{
        reinterpret_cast<const std::byte*>(body_str.data()), body_str.size()
    };

    const size_t total = required_size(meta.size(), body.size());
    std::vector<uint8_t> buf(total);

    auto enc = encode_to(buf.data(), buf.size(), hdr, meta, body);
    ASSERT_TRUE(enc.has_value()) << "encode error: " << (int)enc.error().code << " - " << enc.error().message;
    EXPECT_EQ(enc->size(), total);

    UrpcHdr out{};
    std::memcpy(&out, enc->data(), sizeof(UrpcHdr));

    EXPECT_EQ(out.type, static_cast<uint8_t>(MsgType::REQUEST));
    EXPECT_EQ(out.stream, 1u);
    EXPECT_EQ(out.method, fnv1a64_fast("hello"));
    EXPECT_EQ(out.timeout_ms, 5000u);
    EXPECT_EQ(out.meta_len, meta.size());
    EXPECT_EQ(out.body_len, body.size());

    const auto* base = enc->data();
    const auto* meta_ptr = reinterpret_cast<const std::byte*>(base + sizeof(UrpcHdr));
    const auto* body_ptr = meta_ptr + out.meta_len;

    ASSERT_LE(sizeof(UrpcHdr) + out.meta_len + out.body_len, enc->size());

    EXPECT_TRUE(std::equal(meta_ptr, meta_ptr + out.meta_len, meta.begin(), meta.end()));
    EXPECT_TRUE(std::equal(body_ptr, body_ptr + out.body_len, body.begin(), body.end()));
}

TEST(Wire, EncodeFailsWhenBufferTooSmall)
{
    UrpcHdr hdr{};
    std::span<const std::byte> empty{};
    const size_t need = required_size(0, 0);
    std::vector<uint8_t> small(need - 1);

    auto enc = encode_to(small.data(), small.size(), hdr, empty, empty);
    EXPECT_FALSE(enc.has_value());
}