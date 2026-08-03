#include "novacache/protocol/encoder.hpp"
#include "novacache/protocol/parser.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace novacache::protocol {
namespace {

RespValue parse_one(std::string_view frame) {
    Parser parser;
    EXPECT_EQ(parser.feed(frame), ParseStatus::incomplete);
    ParseResult result = parser.next();
    EXPECT_EQ(result.status, ParseStatus::complete) << result.message;
    EXPECT_TRUE(result.value.has_value());
    return *result.value;
}

TEST(ProtocolParserTest, ParsesEveryResp2Type) {
    EXPECT_EQ(parse_one("+OK\r\n"), RespValue::simple("OK"));
    EXPECT_EQ(parse_one("-ERR failure\r\n"), RespValue::error("ERR failure"));
    EXPECT_EQ(parse_one(":0\r\n"), RespValue::integer(0));
    EXPECT_EQ(parse_one(":-9223372036854775808\r\n"),
              RespValue::integer(std::numeric_limits<std::int64_t>::min()));
    EXPECT_EQ(parse_one("$0\r\n\r\n"), RespValue::bulk(std::string{}));
    EXPECT_EQ(parse_one("$-1\r\n"), RespValue::bulk(std::nullopt));
    EXPECT_EQ(parse_one("*0\r\n"), RespValue::array(std::vector<RespValue>{}));
    EXPECT_EQ(parse_one("*-1\r\n"), RespValue::array(std::nullopt));
}

TEST(ProtocolParserTest, PreservesBinaryBulkData) {
    const std::string payload{"a\0b\r\nc", 6};
    const std::string frame = "$6\r\n" + payload + "\r\n";

    EXPECT_EQ(parse_one(frame), RespValue::bulk(payload));
}

TEST(ProtocolParserTest, ParsesNestedArrays) {
    const RespValue expected = RespValue::array(std::vector<RespValue>{
        RespValue::integer(1),
        RespValue::array(
            std::vector<RespValue>{RespValue::simple("OK"), RespValue::bulk(std::nullopt)}),
    });

    EXPECT_EQ(parse_one("*2\r\n:1\r\n*2\r\n+OK\r\n$-1\r\n"), expected);
}

TEST(ProtocolParserTest, SupportsEveryFragmentBoundary) {
    const std::string frame = "*3\r\n+OK\r\n$5\r\nhello\r\n:-42\r\n";
    const RespValue expected = RespValue::array(std::vector<RespValue>{
        RespValue::simple("OK"), RespValue::bulk(std::string{"hello"}), RespValue::integer(-42)});

    for (std::size_t boundary = 0; boundary <= frame.size(); ++boundary) {
        Parser parser;
        EXPECT_EQ(parser.feed(std::string_view{frame}.substr(0, boundary)),
                  ParseStatus::incomplete);
        if (boundary == frame.size()) {
            const ParseResult result = parser.next();
            ASSERT_EQ(result.status, ParseStatus::complete);
            EXPECT_EQ(result.value, expected);
            continue;
        }
        EXPECT_EQ(parser.next().status, ParseStatus::incomplete) << "boundary " << boundary;
        EXPECT_EQ(parser.feed(std::string_view{frame}.substr(boundary)), ParseStatus::incomplete);
        ParseResult result = parser.next();
        ASSERT_EQ(result.status, ParseStatus::complete) << "boundary " << boundary;
        EXPECT_EQ(result.value, expected);
    }
}

TEST(ProtocolParserTest, SupportsByteAtATimeFragments) {
    Parser parser;
    const std::string frame = "$5\r\nhello\r\n";
    for (std::size_t index = 0; index < frame.size(); ++index) {
        EXPECT_EQ(parser.feed(std::string_view{frame}.substr(index, 1)), ParseStatus::incomplete);
        const ParseResult result = parser.next();
        EXPECT_EQ(result.status,
                  index + 1U == frame.size() ? ParseStatus::complete : ParseStatus::incomplete);
    }
}

TEST(ProtocolParserTest, ReturnsPipelinedFramesInOrder) {
    Parser parser;
    EXPECT_EQ(parser.feed("+one\r\n:2\r\n$3\r\nxyz\r\n"), ParseStatus::incomplete);

    EXPECT_EQ(parser.next().value, RespValue::simple("one"));
    EXPECT_EQ(parser.next().value, RespValue::integer(2));
    EXPECT_EQ(parser.next().value, RespValue::bulk(std::string{"xyz"}));
    EXPECT_EQ(parser.next().status, ParseStatus::incomplete);
    EXPECT_EQ(parser.buffered_bytes(), 0U);
}

TEST(ProtocolParserTest, RejectsInvalidLineEndings) {
    for (const std::string_view frame :
         {"+OK\n", "+OK\rX", "-ERR\n", ":1\n", "$1\nx\r\n", "*0\rX"}) {
        Parser parser;
        EXPECT_EQ(parser.feed(frame), ParseStatus::incomplete);
        EXPECT_EQ(parser.next().status, ParseStatus::malformed) << frame;
    }

    Parser parser;
    EXPECT_EQ(parser.feed("$1\r\nxX"), ParseStatus::incomplete);
    EXPECT_EQ(parser.next().status, ParseStatus::malformed);
}

TEST(ProtocolParserTest, WaitsForPotentiallyFragmentedCrlf) {
    Parser parser;
    EXPECT_EQ(parser.feed("+OK\r"), ParseStatus::incomplete);
    EXPECT_EQ(parser.next().status, ParseStatus::incomplete);
    EXPECT_EQ(parser.feed("\n"), ParseStatus::incomplete);
    EXPECT_EQ(parser.next().value, RespValue::simple("OK"));

    EXPECT_EQ(parser.feed("$1\r\nx\r"), ParseStatus::incomplete);
    EXPECT_EQ(parser.next().status, ParseStatus::incomplete);
    EXPECT_EQ(parser.feed("\n"), ParseStatus::incomplete);
    EXPECT_EQ(parser.next().value, RespValue::bulk(std::string{"x"}));
}

TEST(ProtocolParserTest, RejectsMalformedAndOverflowingNumbers) {
    for (const std::string_view frame :
         {":\r\n", ":+1\r\n", ":--1\r\n", ":1x\r\n", ":9223372036854775808\r\n",
          ":-9223372036854775809\r\n", "$abc\r\n", "*999999999999999999999\r\n"}) {
        Parser parser;
        EXPECT_EQ(parser.feed(frame), ParseStatus::incomplete);
        EXPECT_EQ(parser.next().status, ParseStatus::malformed) << frame;
    }
}

TEST(ProtocolParserTest, RejectsIllegalNegativeLengths) {
    for (const std::string_view frame : {"$-2\r\n", "*-2\r\n"}) {
        Parser parser;
        EXPECT_EQ(parser.feed(frame), ParseStatus::incomplete);
        EXPECT_EQ(parser.next().status, ParseStatus::malformed) << frame;
    }
}

TEST(ProtocolParserTest, EnforcesBulkArrayAndBufferLimits) {
    ParserLimits limits;
    limits.max_bulk_size = 3;
    limits.max_array_size = 2;
    limits.max_buffer_size = 8;

    Parser bulk_parser{limits};
    EXPECT_EQ(bulk_parser.feed("$4\r\n"), ParseStatus::incomplete);
    EXPECT_EQ(bulk_parser.next().status, ParseStatus::oversized);

    Parser array_parser{limits};
    EXPECT_EQ(array_parser.feed("*3\r\n"), ParseStatus::incomplete);
    EXPECT_EQ(array_parser.next().status, ParseStatus::oversized);

    Parser buffer_parser{limits};
    EXPECT_EQ(buffer_parser.feed("12345678"), ParseStatus::incomplete);
    EXPECT_EQ(buffer_parser.feed("9"), ParseStatus::oversized);
    EXPECT_EQ(buffer_parser.next().status, ParseStatus::oversized);
    buffer_parser.reset();
    EXPECT_EQ(buffer_parser.buffered_bytes(), 0U);
    EXPECT_EQ(buffer_parser.feed("+OK\r\n"), ParseStatus::incomplete);
    EXPECT_EQ(buffer_parser.next().status, ParseStatus::complete);
}

TEST(ProtocolParserTest, EnforcesNestingDepth) {
    ParserLimits limits;
    limits.max_depth = 2;

    Parser accepted{limits};
    EXPECT_EQ(accepted.feed("*1\r\n*1\r\n:1\r\n"), ParseStatus::incomplete);
    EXPECT_EQ(accepted.next().status, ParseStatus::complete);

    Parser rejected{limits};
    EXPECT_EQ(rejected.feed("*1\r\n*1\r\n*1\r\n:1\r\n"), ParseStatus::incomplete);
    EXPECT_EQ(rejected.next().status, ParseStatus::too_deep);
}

TEST(ProtocolParserTest, KeepsIncompleteFrameBuffered) {
    Parser parser;
    EXPECT_EQ(parser.feed("$5\r\nabc"), ParseStatus::incomplete);
    EXPECT_EQ(parser.next().status, ParseStatus::incomplete);
    EXPECT_EQ(parser.buffered_bytes(), 7U);
    EXPECT_EQ(parser.feed("de\r\n"), ParseStatus::incomplete);
    EXPECT_EQ(parser.next().value, RespValue::bulk(std::string{"abcde"}));
}

TEST(ProtocolEncoderTest, EncodesEveryTypeAndNestedArrays) {
    EXPECT_EQ(encode(RespValue::simple("OK")), "+OK\r\n");
    EXPECT_EQ(encode(RespValue::error("ERR failure")), "-ERR failure\r\n");
    EXPECT_EQ(encode(RespValue::integer(std::numeric_limits<std::int64_t>::min())),
              ":-9223372036854775808\r\n");
    EXPECT_EQ(encode(RespValue::bulk(std::nullopt)), "$-1\r\n");
    EXPECT_EQ(encode(RespValue::array(std::nullopt)), "*-1\r\n");

    const std::string binary{"a\0b", 3};
    const RespValue value = RespValue::array(std::vector<RespValue>{
        RespValue::bulk(binary), RespValue::array(std::vector<RespValue>{})});
    const std::string expected = std::string{"*2\r\n$3\r\na\0b", 11} + "\r\n*0\r\n";
    EXPECT_EQ(encode(value), expected);
}

TEST(ProtocolEncoderTest, AppendsWithoutClearingOutput) {
    std::string output = "prefix:";
    append_encoded(RespValue::integer(7), output);
    EXPECT_EQ(output, "prefix::7\r\n");
}

TEST(ProtocolEncoderTest, RejectsCrlfInLineTypes) {
    EXPECT_THROW((void)encode(RespValue::simple("bad\nvalue")), std::invalid_argument);
    EXPECT_THROW((void)encode(RespValue::error("bad\rvalue")), std::invalid_argument);
}

TEST(ProtocolEncoderTest, RoundTripsBinaryAndNestedValues) {
    const RespValue original = RespValue::array(std::vector<RespValue>{
        RespValue::simple("PONG"),
        RespValue::integer(42),
        RespValue::bulk(std::string{"x\0y", 3}),
        RespValue::bulk(std::nullopt),
        RespValue::array(std::nullopt),
    });

    EXPECT_EQ(parse_one(encode(original)), original);
}

} // namespace
} // namespace novacache::protocol
