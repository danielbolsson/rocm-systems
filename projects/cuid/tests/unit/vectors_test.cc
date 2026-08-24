/*
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The cross-layer conformance vectors. These are the CUID wire format stated
// as values rather than prose: the kernel driver and this library must both
// reproduce every one of them bit for bit, or a fleet sees two different names
// for the same component.
//
// The vectors are read from tests/vectors/cuid_vectors.txt rather than
// transcribed here on purpose. A transcribed vector decays silently, and a
// vector that has decayed in the same direction as the bug it was meant to
// catch confirms the bug against itself -- which is exactly what happened to
// the component-type check in reverse_lookup_test.cc.

#include "unit/vectors_test.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/cuid_util.h"
#include "src/hmac.h"

namespace {

struct Vector {
  std::string kind;     // primary | derived | aux-input
  std::string name;     // P-1, D-2, A-1, ...
  std::string payload;  // hex
  std::string hmac;     // hex, empty for primaries
  std::string uuid;     // rendered, empty for aux-input
};

std::string to_hex(const uint8_t* data, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

// The vectors file sits next to the sources; the test binary can be run from
// anywhere, so allow an override and otherwise try the in-tree location.
std::string vectors_path() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe) - nothing in this library calls setenv
  const char* env = std::getenv("AMDCUID_VECTORS_PATH");
  if (env && env[0]) return env;
  return AMDCUID_VECTORS_FILE;
}

bool load_vectors(std::vector<Vector>& out, std::string& why) {
  const std::string path = vectors_path();
  std::ifstream in(path);
  if (!in.is_open()) {
    why = "cannot open " + path;
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    Vector v;
    std::getline(fields, v.kind, '\t');
    std::getline(fields, v.name, '\t');
    std::getline(fields, v.payload, '\t');
    std::getline(fields, v.hmac, '\t');
    std::getline(fields, v.uuid, '\t');
    out.push_back(v);
  }
  if (out.empty()) {
    why = path + " contains no vectors";
    return false;
  }
  return true;
}

const Vector* find(const std::vector<Vector>& v, const std::string& kind, const std::string& name) {
  for (const auto& e : v) {
    if (e.kind == kind && e.name == name) return &e;
  }
  return nullptr;
}

// Every primary vector must render to its stated UUID and survive the
// round-trip back to its stated payload.
void check_framing(const std::vector<Vector>& vectors) {
  for (const auto& v : vectors) {
    if (v.kind != "primary" && v.kind != "derived") continue;
    ASSERT_EQ(v.payload.size(), 32u) << v.name;

    uint8_t raw[16] = {0};
    for (size_t i = 0; i < 16; ++i) {
      raw[i] = static_cast<uint8_t>(std::stoul(v.payload.substr(2 * i, 2), nullptr, 16));
    }

    amdcuid_id_t id = {{0}};
    CuidUtilities::add_UUIDv8_bits(raw, &id);
    EXPECT_EQ(CuidUtilities::get_cuid_as_string(&id), v.uuid) << "framing: " << v.name;

    // Version 8, variant 10b, per RFC 9562.
    EXPECT_EQ(id.bytes[6] >> 4, 0x8) << "version nibble: " << v.name;
    EXPECT_EQ(id.bytes[8] >> 6, 0x2) << "variant bits: " << v.name;

    uint8_t back[16] = {0};
    CuidUtilities::remove_UUIDv8_bits(&id, back);
    EXPECT_EQ(to_hex(back, sizeof(back)), v.payload) << "round-trip: " << v.name;
  }
}

// The six bits the framing discards must be exactly the padding, payload
// 122:127 -- not payload 120:121, which carry the Component Type's high bits.
void check_framing_drops_only_padding() {
  amdcuid_id_t base = {{0}};
  const uint8_t zero[16] = {0};
  CuidUtilities::add_UUIDv8_bits(zero, &base);

  for (int bit = 0; bit < 128; ++bit) {
    uint8_t raw[16] = {0};
    raw[bit >> 3] = static_cast<uint8_t>(1u << (bit & 7));
    amdcuid_id_t id = {{0}};
    CuidUtilities::add_UUIDv8_bits(raw, &id);

    const bool dropped = std::memcmp(id.bytes, base.bytes, 16) == 0;
    const bool is_padding = bit >= 122;
    EXPECT_EQ(dropped, is_padding)
        << "payload bit " << bit << (is_padding ? " should be discarded" : " must be preserved");
  }
}

}  // namespace

TestVectors::TestVectors() {
  SetTitle("Conformance Vectors");
  SetDescription(
      "Reproduce the shared cross-layer CUID conformance vectors: primary "
      "packing, UUIDv8 framing, the derived fold and the auxiliary "
      "construction.");
}

void TestVectors::SetUp() {}

void TestVectors::Run() {
  std::vector<Vector> vectors;
  std::string why;
  ASSERT_TRUE(load_vectors(vectors, why)) << why;

  check_framing(vectors);
  check_framing_drops_only_padding();

  // ---- primary packing -------------------------------------------------
  struct PrimaryCase {
    const char* name;
    uint64_t serial;
    uint16_t unit_id;
    uint8_t revision_id;
    uint16_t device_id;
    uint16_t vendor_id;
    amdcuid_device_type_t type;
    bool aux;
  };
  static const PrimaryCase kPrimaries[] = {
      {"P-1", 0x06C5349BD3AAABD4ULL, 0, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_GPU, false},
      {"P-2", 0x8E8C71777252EBFFULL, 0, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_GPU, false},
      {"U-1", 0x06C5349BD3AAABD4ULL, 0x0123, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_GPU, false},
      {"T-PLATFORM", 0x06C5349BD3AAABD4ULL, 0, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_PLATFORM,
       false},
      {"T-CPU", 0x06C5349BD3AAABD4ULL, 0, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_CPU, false},
      {"T-GPU", 0x06C5349BD3AAABD4ULL, 0, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_GPU, false},
      {"T-NIC", 0x06C5349BD3AAABD4ULL, 0, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_NIC, false},
      {"T-NPU", 0x06C5349BD3AAABD4ULL, 0, 0x00, 0x73A3, 0x1002, AMDCUID_DEVICE_TYPE_NPU, false},
  };

  for (const auto& c : kPrimaries) {
    const Vector* v = find(vectors, "primary", c.name);
    ASSERT_NE(v, nullptr) << "missing vector " << c.name;

    amdcuid_primary_id id = {};
    ASSERT_EQ(CuidUtilities::generate_primary_cuid(c.serial, c.unit_id, c.revision_id, c.device_id,
                                                   c.vendor_id, c.type, &id, c.aux),
              AMDCUID_STATUS_SUCCESS);
    EXPECT_EQ(to_hex(id.raw_bits, sizeof(id.raw_bits)), v->payload) << "payload: " << c.name;
    EXPECT_EQ(CuidUtilities::get_cuid_as_string(&id.UUIDv8_representation), v->uuid)
        << "uuid: " << c.name;
  }

  // An NPU must not render identically to a Platform. It did, for as long as
  // payload bits 120:121 were written to 126:127 and then discarded.
  {
    const Vector* npu = find(vectors, "primary", "T-NPU");
    const Vector* platform = find(vectors, "primary", "T-PLATFORM");
    ASSERT_NE(npu, nullptr);
    ASSERT_NE(platform, nullptr);
    EXPECT_NE(npu->uuid, platform->uuid);
  }

  // ---- derived fold ----------------------------------------------------
  const Vector* p1 = find(vectors, "primary", "P-1");
  ASSERT_NE(p1, nullptr);

  amdcuid_primary_id primary = {};
  ASSERT_EQ(CuidUtilities::generate_primary_cuid(0x06C5349BD3AAABD4ULL, 0, 0x00, 0x73A3, 0x1002,
                                                 AMDCUID_DEVICE_TYPE_GPU, &primary, false),
            AMDCUID_STATUS_SUCCESS);

  {
    // D-1: the canonical fallback seed. Its exact bytes are what make an
    // unprovisioned machine agree with the kernel.
    const Vector* v = find(vectors, "derived", "D-1");
    ASSERT_NE(v, nullptr);
    cuid_hmac h(kDefaultSeed, kDefaultSeedLen);
    ASSERT_TRUE(h.is_valid());
    amdcuid_derived_id derived = {};
    ASSERT_EQ(CuidUtilities::generate_derived_cuid(&primary, &derived, &h), AMDCUID_STATUS_SUCCESS);
    EXPECT_EQ(to_hex(derived.raw_bits, sizeof(derived.raw_bits)), v->payload) << "D-1 payload";
    EXPECT_EQ(CuidUtilities::get_cuid_as_string(&derived.UUIDv8_representation), v->uuid)
        << "D-1 uuid";
  }

  {
    // D-2: a key that is not a placeholder, so this vector pins the fold, the
    // framing and the operand order independently of any constant that might
    // later move.
    const Vector* v = find(vectors, "derived", "D-2");
    ASSERT_NE(v, nullptr);
    uint8_t key[key_length];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i);
    cuid_hmac h(key);
    ASSERT_TRUE(h.is_valid());
    amdcuid_derived_id derived = {};
    ASSERT_EQ(CuidUtilities::generate_derived_cuid(&primary, &derived, &h), AMDCUID_STATUS_SUCCESS);
    EXPECT_EQ(to_hex(derived.raw_bits, sizeof(derived.raw_bits)), v->payload) << "D-2 payload";
    EXPECT_EQ(CuidUtilities::get_cuid_as_string(&derived.UUIDv8_representation), v->uuid)
        << "D-2 uuid";
    // Payload octet 14 is digest octet 13 masked to five bits: the derived slot
    // is 45 bits, not 46, because bit 117 belongs to the auxiliary marker.
    EXPECT_EQ(derived.raw_bits[14], 0x1A);
  }

  // ---- auxiliary -------------------------------------------------------
  {
    const Vector* input = find(vectors, "aux-input", "A-1");
    const Vector* aux_primary = find(vectors, "primary", "A-1");
    const Vector* aux_derived = find(vectors, "derived", "A-2");
    ASSERT_NE(input, nullptr);
    ASSERT_NE(aux_primary, nullptr);
    ASSERT_NE(aux_derived, nullptr);

    // The auxiliary serial is the low 8 octets of the structure's digest.
    ASSERT_EQ(input->hmac.size(), 64u);
    uint64_t serial = 0;
    for (size_t i = 0; i < 8; ++i) {
      serial |= static_cast<uint64_t>(std::stoul(input->hmac.substr(2 * i, 2), nullptr, 16))
                << (8 * i);
    }

    amdcuid_primary_id aux = {};
    ASSERT_EQ(CuidUtilities::generate_primary_cuid(serial, 0, 0x00, 0x73A3, 0x1002,
                                                   AMDCUID_DEVICE_TYPE_GPU, &aux, true),
              AMDCUID_STATUS_SUCCESS);
    EXPECT_EQ(to_hex(aux.raw_bits, sizeof(aux.raw_bits)), aux_primary->payload) << "A-1 payload";
    EXPECT_EQ(CuidUtilities::get_cuid_as_string(&aux.UUIDv8_representation), aux_primary->uuid)
        << "A-1 uuid";
    EXPECT_NE(aux.raw_bits[14] & 0x20, 0) << "A-1 auxiliary bit not set";

    cuid_hmac h(kTemporaryKey, kTemporaryKeyLen);
    ASSERT_TRUE(h.is_valid());
    amdcuid_derived_id derived = {};
    ASSERT_EQ(CuidUtilities::generate_derived_cuid(&aux, &derived, &h), AMDCUID_STATUS_SUCCESS);
    EXPECT_EQ(to_hex(derived.raw_bits, sizeof(derived.raw_bits)), aux_derived->payload)
        << "A-2 payload";
    EXPECT_EQ(CuidUtilities::get_cuid_as_string(&derived.UUIDv8_representation), aux_derived->uuid)
        << "A-2 uuid";
    // The marker must survive derivation, or an auxiliary value cannot be
    // recognised as one without its primary.
    EXPECT_NE(derived.raw_bits[14] & 0x20, 0) << "A-2 auxiliary bit not carried";
  }
}

void TestVectors::DisplayTestInfo() { TestBase::DisplayTestInfo(); }
void TestVectors::DisplayResults() const { TestBase::DisplayResults(); }
void TestVectors::Close() {}
