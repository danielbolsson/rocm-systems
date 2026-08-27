// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#include "unit/utilities_test.h"

#include <gtest/gtest.h>

#include <cstring>

#include "src/cuid_util.h"

TestUtilities::TestUtilities() {
  SetTitle("Utilities");
  SetDescription(
      "Verify CuidUtilities::remove_UUIDv8_bits correctly strips UUIDv8 "
      "overhead and handles null pointer inputs safely.");
}

void TestUtilities::SetUp() {}

void TestUtilities::Run() {
  // Roundtrip: verify that remove_UUIDv8_bits recovers the expected raw bits
  // from a canned UUIDv8-encoded amdcuid_id_t.
  {
    amdcuid_id_t id = {{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0x8C, 0xDE, 0xBC, 0x48, 0xD1, 0x59,
                        0xE2, 0x6A, 0xF3, 0x7B}};
    // The last raw octet carries payload bits 120:121 in its low two bits.
    // This vector previously ended 0xC0, which encoded the withdrawn
    // convention where the rendered value's final two bits were read into
    // payload 126:127 -- padding -- and payload 120:121 were dropped.
    const uint8_t expected[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                  0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0x03};
    uint8_t out[16] = {0};
    CuidUtilities::remove_UUIDv8_bits(&id, out);

    for (int i = 0; i < 16; ++i) {
      EXPECT_EQ(out[i], expected[i]) << "Mismatch at byte " << i;
    }

    // The framing must be the exact inverse: re-adding it reproduces the input.
    amdcuid_id_t reframed = {{0}};
    CuidUtilities::add_UUIDv8_bits(out, &reframed);
    for (int i = 0; i < 16; ++i) {
      EXPECT_EQ(reframed.bytes[i], id.bytes[i]) << "Round-trip mismatch at byte " << i;
    }
  }

  // Null safety: neither call should crash or modify the output buffer.
  {
    amdcuid_id_t id = {{0}};
    uint8_t out[16] = {0xFF};

    CuidUtilities::remove_UUIDv8_bits(nullptr, out);
    EXPECT_EQ(out[0], 0xFF);

    CuidUtilities::remove_UUIDv8_bits(&id, nullptr);
  }
}

void TestUtilities::DisplayTestInfo() { TestBase::DisplayTestInfo(); }
void TestUtilities::DisplayResults() const { TestBase::DisplayResults(); }
void TestUtilities::Close() {}

// A Platform CUID adopted verbatim from firmware is not a constructed UUIDv8,
// and none of its payload fields may be decoded. Reading them anyway reported a
// Platform as an NPU, and as auxiliary, depending on what the firmware wrote.
TEST(cuidtstUnprivileged, AdoptedFirmwareUuidIsNotConstructed) {
  struct {
    const char* uuid;
    bool constructed;
  } cases[] = {
      // Real-world shaped SMBIOS system UUIDs: versions 3, 0 and 4.
      {"4c4c4544-0037-3010-8055-b4c04f434332", false},
      {"03000200-0400-0500-0006-000700080009", false},
      {"aabbccdd-eeff-4011-9222-3344556677ff", false},
      // A CUID this library constructed: conformance vector P-1.
      {"d4abaad3-9b34-8c50-9800-028dcc084200", true},
  };

  for (const auto& c : cases) {
    amdcuid_id_t id{};
    ASSERT_EQ(CuidUtilities::uuid_string_to_uint8(c.uuid, id.bytes), AMDCUID_STATUS_SUCCESS)
        << c.uuid;
    EXPECT_EQ(CuidUtilities::is_constructed(&id), c.constructed) << c.uuid;
  }
}

// The HMAC message for an adopted identifier is the firmware UUID itself, all
// sixteen octets. De-framing it first drops six bits, so two platforms whose
// system UUIDs differ only in their version and variant bits would collide.
TEST(cuidtstUnprivileged, DeframingAnAdoptedUuidWouldLoseBits) {
  amdcuid_id_t id{};
  ASSERT_EQ(CuidUtilities::uuid_string_to_uint8("aabbccdd-eeff-4011-9222-3344556677ff", id.bytes),
            AMDCUID_STATUS_SUCCESS);

  uint8_t raw[16];
  CuidUtilities::remove_UUIDv8_bits(&id, raw);

  amdcuid_id_t reframed{};
  CuidUtilities::add_UUIDv8_bits(raw, &reframed);

  // Not a round trip: this is why the adopted path must not de-frame.
  EXPECT_NE(0, std::memcmp(id.bytes, reframed.bytes, sizeof(id.bytes)));
}
