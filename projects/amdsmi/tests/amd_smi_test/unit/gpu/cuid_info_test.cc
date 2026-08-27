// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

/*
 * Tests for the CUID API surface.
 *
 * These run with or without CUID support compiled in, and with or without a
 * GPU: the point of most of them is that the ABI behaves the same either way,
 * which is the property that lets a consumer call these entry points
 * unconditionally. The checks that need a device are skipped rather than
 * failed, so they are useful in CI and stronger on a developer's machine.
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "amd_smi/amdsmi.h"

namespace {

// A CUID is rendered as the standard 8-4-4-4-12 UUID string.
bool LooksLikeUuid(const char* value) {
  const std::string s(value);
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

// Payload bit 117 -- the Auxiliary Value Identifier, payload octet 14 mask 0x20
// -- read back out of a rendered UUIDv8 string. The framing puts payload bits
// 120:121 in the low two bits of the last rendered octet and shifts the rest
// along by six: the last rendered octet is
// `((payload[14] & 0x3F) << 2) | (payload[15] & 0x03)`, so payload bit 117
// lands in bit 7 of rendered octet 15, the last one.
//
// Checked against all thirteen vectors in
// projects/cuid/tests/vectors/cuid_vectors.txt. Reading bit 7 of rendered octet
// 14 instead -- which this did -- reads a VendorID bit: it disagrees with the
// payload on A-1, A-2 and D-2, which is to say it is wrong on both auxiliary
// vectors and right on ordinary ones by luck.
bool AuxiliaryBitFromUuidString(const std::string& uuid, bool* aux) {
  std::string hex;
  for (char c : uuid) {
    if (c != '-') hex.push_back(c);
  }
  if (hex.size() != 32) return false;

  const int last = std::stoi(hex.substr(30, 2), nullptr, 16);
  *aux = (last & 0x80) != 0;
  return true;
}

// The BDF string the driver's sysfs directory is named after, and the one
// amd-smi hands libamdcuid. Mirrors stringify_bdf() in
// src/amd_smi/amd_smi_utils.cc, which is internal to the library.
std::string BdfString(const amdsmi_bdf_t& bdf) {
  char out[32] = {};
  snprintf(out, sizeof(out), "%04x:%02x:%02x.%x", static_cast<unsigned>(bdf.domain_number),
           static_cast<unsigned>(bdf.bus_number), static_cast<unsigned>(bdf.device_number),
           static_cast<unsigned>(bdf.function_number));
  return out;
}

// A private directory under TMPDIR, removed when the test finishes.
class ScopedTempDir {
 public:
  ScopedTempDir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/amdsmi-cuid-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    path_ = mkdtemp(buf.data()) ? buf.data() : "";
  }
  ~ScopedTempDir() {
    if (!path_.empty()) {
      // Only ever holds the fabricated sysfs tree created below.
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  const std::string& path() const { return path_; }
  bool valid() const { return !path_.empty(); }

 private:
  std::string path_;
};

// Sets AMDSMI_CUID_SYSFS_ROOT for the duration of a test and restores whatever
// was there, so one case cannot change what a later one sees.
class ScopedSysfsRoot {
 public:
  explicit ScopedSysfsRoot(const std::string& root) {
    const char* previous = std::getenv(kVar);
    had_previous_ = previous != nullptr;
    if (had_previous_) previous_ = previous;
    setenv(kVar, root.c_str(), 1);
  }
  ~ScopedSysfsRoot() {
    if (had_previous_) {
      setenv(kVar, previous_.c_str(), 1);
    } else {
      unsetenv(kVar);
    }
  }
  ScopedSysfsRoot(const ScopedSysfsRoot&) = delete;
  ScopedSysfsRoot& operator=(const ScopedSysfsRoot&) = delete;

 private:
  static constexpr const char* kVar = "AMDSMI_CUID_SYSFS_ROOT";
  bool had_previous_ = false;
  std::string previous_;
};

// mkdir -p, for the fabricated sysfs tree.
bool MakeDirs(const std::string& path) {
  std::string partial;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      partial = path.substr(0, i);
      if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  }
  return true;
}

// Whatever the driver published in `attribute` for this device, trimmed, or an
// empty string when it publishes nothing. Reads the real /sys, never the
// fabricated root.
std::string DriverPublished(const std::string& bdf, const char* attribute) {
  std::ifstream in("/sys/bus/pci/devices/" + bdf + "/" + attribute);
  if (!in) return "";
  std::string value;
  std::getline(in, value);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

std::vector<amdsmi_processor_handle> GpuHandles() {
  std::vector<amdsmi_processor_handle> handles;

  uint32_t socket_count = 0;
  if (amdsmi_get_socket_handles(&socket_count, nullptr) != AMDSMI_STATUS_SUCCESS) return handles;
  std::vector<amdsmi_socket_handle> sockets(socket_count);
  if (amdsmi_get_socket_handles(&socket_count, sockets.data()) != AMDSMI_STATUS_SUCCESS) {
    return handles;
  }

  for (auto socket : sockets) {
    uint32_t count = 0;
    if (amdsmi_get_processor_handles(socket, &count, nullptr) != AMDSMI_STATUS_SUCCESS) continue;
    std::vector<amdsmi_processor_handle> procs(count);
    if (amdsmi_get_processor_handles(socket, &count, procs.data()) != AMDSMI_STATUS_SUCCESS) {
      continue;
    }
    for (auto proc : procs) {
      processor_type_t type = AMDSMI_PROCESSOR_TYPE_UNKNOWN;
      if (amdsmi_get_processor_type(proc, &type) != AMDSMI_STATUS_SUCCESS) continue;
      if (type == AMDSMI_PROCESSOR_TYPE_AMD_GPU) handles.push_back(proc);
    }
  }
  return handles;
}

// RAII init/shutdown. A fixture would be the obvious way, but every other test
// under unit/gpu/ uses the bare TEST() macro against the GpuUnit suite, and
// GTest rejects a suite that mixes TEST and TEST_F.
class AmdSmiSession {
 public:
  AmdSmiSession() : status_(amdsmi_init(AMDSMI_INIT_AMD_GPUS)) {}
  ~AmdSmiSession() {
    if (status_ == AMDSMI_STATUS_SUCCESS) amdsmi_shut_down();
  }
  AmdSmiSession(const AmdSmiSession&) = delete;
  AmdSmiSession& operator=(const AmdSmiSession&) = delete;

  amdsmi_status_t status() const { return status_; }

 private:
  amdsmi_status_t status_;
};

// Whether this build linked libamdcuid. The ABI is deliberately identical
// either way, so nothing at runtime distinguishes "built without the library"
// from "the library is here but has nothing to say about this device" -- which
// is why every case used to treat AMDSMI_STATUS_NOT_SUPPORTED as a reason to
// skip, and why a build without the library therefore asserted nothing. The
// build system passes BUILD_CUID to this target so the distinction is
// available, and not-supported becomes an assertion where it is required.
#ifdef BUILD_CUID
constexpr bool kCuidBuiltIn = true;
#else
constexpr bool kCuidBuiltIn = false;
#endif

// First 8 octets of the unkeyed SHA-256 of the canonical fallback seed, the
// public 24-octet string "AMD-CUID-DEFAULT-SEED-v1" that libamdcuid derives
// with until an administrator provisions a real one. Pinned here rather than
// recomputed so that a producer which fingerprints the wrong bytes -- the key
// file's contents, a NUL-terminated copy of the seed, the HMAC rather than the
// plain digest -- fails instead of agreeing with itself.
constexpr uint8_t kFallbackSeedFingerprint[AMDSMI_CUID_SEED_FINGERPRINT_SIZE] = {
    0xbe, 0x89, 0x37, 0xfb, 0xa7, 0xed, 0x4e, 0x6f};

std::string HexOf(const uint8_t* octets, size_t count) {
  std::string out;
  char buf[3] = {};
  for (size_t i = 0; i < count; ++i) {
    snprintf(buf, sizeof(buf), "%02x", octets[i]);
    out += buf;
  }
  return out;
}

// What the seed API answers is not a property of the build: it depends on the
// state of the node's key store *and* on who is asking. There are four
// distinguishable states and libamdcuid reports each of them differently, so a
// test that asserts one of them is a test that only passes on one kind of
// machine -- which is how three of the cases below came to fail the first time
// they ran on a node that had been provisioned.
//
// The key store is 0600 root-owned, so kUnreadable is the ordinary state for an
// unprivileged caller on a provisioned node. It is emphatically not the same as
// kAbsent: reporting it as "unprovisioned, fingerprint of the public fallback
// seed" tells an operator that a node carrying a secret carries none.
enum class SeedStore {
  kAbsent,       // no key store: the node derives with the public fallback seed
  kProvisioned,  // a key store this caller can read
  kUnreadable,   // a key store this caller cannot open
  kCorrupt,      // a key store that exists and is not a key
};

const char* SeedStoreName(SeedStore state) {
  switch (state) {
    case SeedStore::kAbsent:
      return "absent";
    case SeedStore::kProvisioned:
      return "provisioned and readable";
    case SeedStore::kUnreadable:
      return "present but unreadable by this caller";
    case SeedStore::kCorrupt:
      return "present but not a key";
  }
  return "unknown";
}

// Where libamdcuid looked for the key store in *this* process. The library
// resolves it once, in a namespace-scope cuid_hmac constructed before main(),
// so the answer is fixed by the environment the process was launched with and
// no test can change it from in here -- which is also what makes it safe to
// read the variable now and get the same answer the library got. The default
// mirrors the library's AMDCUID_CONFIG_DIR, which is private to its build.
std::string SeedStorePath() {
  const char* env = std::getenv("AMDCUID_HMAC_KEY_PATH");
  return (env && env[0]) ? env : "/etc/amdcuid/hmac_key.bin";
}

SeedStore SeedStoreState() {
  const std::string path = SeedStorePath();

  struct stat st;
  if (stat(path.c_str(), &st) != 0) return SeedStore::kAbsent;

  // Whether the open succeeds, not access(R_OK): root passes an access() check
  // on a mode-0 file it can still open, and it is the open that decides what
  // the library reports. Asking the same question the library asks is the point.
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return SeedStore::kUnreadable;

  return st.st_size == static_cast<off_t>(AMDSMI_CUID_SEED_SIZE) ? SeedStore::kProvisioned
                                                                 : SeedStore::kCorrupt;
}

// The status amdsmi_get_cuid_seed_info() must return in each state. Absence is
// a successful answer -- an unprovisioned node is an ordinary node, not a
// broken one -- while the two failures are distinct because what an operator
// must do about them is distinct: gain privilege, or repair the store.
amdsmi_status_t ExpectedSeedInfoStatus(SeedStore state) {
  switch (state) {
    case SeedStore::kAbsent:
    case SeedStore::kProvisioned:
      return AMDSMI_STATUS_SUCCESS;
    case SeedStore::kUnreadable:
      return AMDSMI_STATUS_NO_PERM;
    case SeedStore::kCorrupt:
      return AMDSMI_STATUS_API_FAILED;
  }
  return AMDSMI_STATUS_API_FAILED;
}

// On any failure the output struct must come back untouched. This is the
// specific regression: the wrong answer was never a garbage fingerprint, it was
// a plausible one -- the public fallback's -- next to provisioned = 0.
void ExpectNothingReported(const amdsmi_cuid_seed_info_t& info) {
  EXPECT_EQ(info.provisioned, 0) << "a failed query must not report a provisioning state";
  for (size_t i = 0; i < sizeof(info.fingerprint); ++i) {
    EXPECT_EQ(info.fingerprint[i], 0)
        << "a failed query must not report a fingerprint (octet " << i << ")";
  }
}

}  // namespace

// A null argument is a caller error, not a "not supported": this must be true
// whether or not the CUID library was linked, so that a caller cannot tell the
// two apart by passing garbage.
TEST(GpuUnit, CuidNullArgumentsRejected) {
  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  EXPECT_EQ(amdsmi_get_gpu_cuid_info(nullptr, nullptr), AMDSMI_STATUS_INVAL);
  EXPECT_EQ(amdsmi_set_cuid_seed(nullptr), AMDSMI_STATUS_INVAL);
  EXPECT_EQ(amdsmi_get_cuid_seed_info(nullptr), AMDSMI_STATUS_INVAL);
}

// Built without libamdcuid, every CUID entry point reports not-supported --
// with valid arguments, so this is not the null-argument case in disguise. The
// symbols are exported either way; only the answer differs. This is the case
// that gives a CUID-less build something to assert: it is the one build where
// the whole feature can regress to a link error or to a status nobody checks.
TEST(GpuUnit, CuidEntryPointsNotSupportedWithoutTheLibrary) {
  if (kCuidBuiltIn) GTEST_SKIP() << "built with CUID support";

  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  amdsmi_cuid_seed_info_t seed_info = {};
  EXPECT_EQ(amdsmi_get_cuid_seed_info(&seed_info), AMDSMI_STATUS_NOT_SUPPORTED);

  const uint8_t seed[AMDSMI_CUID_SEED_SIZE] = {};
  EXPECT_EQ(amdsmi_set_cuid_seed(seed), AMDSMI_STATUS_NOT_SUPPORTED);

  // A real handle where there is one. Without the library neither of the two
  // device entry points dereferences the handle before returning, so a
  // GPU-less machine still exercises them; on a machine with GPUs every one of
  // them must answer the same way.
  auto handles = GpuHandles();
  if (handles.empty()) handles.push_back(reinterpret_cast<amdsmi_processor_handle>(&seed_info));

  for (auto handle : handles) {
    amdsmi_cuid_info_t info = {};
    EXPECT_EQ(amdsmi_get_gpu_cuid_info(handle, &info), AMDSMI_STATUS_NOT_SUPPORTED);

    char cuid[AMDSMI_GPU_CUID_SIZE] = {};
    unsigned int length = sizeof(cuid);
    EXPECT_EQ(amdsmi_get_gpu_device_cuid(handle, &length, cuid), AMDSMI_STATUS_NOT_SUPPORTED);
  }
}

// Built without libamdcuid the entry points still exist and report
// not-supported. Built with it they answer -- and which answer is correct is
// decided by the state of the key store, so this asserts the state it is
// actually in rather than the one state a developer's machine happened to be in.
TEST(GpuUnit, CuidSeedInfoAnsweredOrUnsupported) {
  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  amdsmi_cuid_seed_info_t info = {};
  const amdsmi_status_t status = amdsmi_get_cuid_seed_info(&info);

  if (!kCuidBuiltIn) {
    ASSERT_EQ(status, AMDSMI_STATUS_NOT_SUPPORTED);
    return;
  }

  const SeedStore state = SeedStoreState();
  ASSERT_EQ(status, ExpectedSeedInfoStatus(state))
      << "key store " << SeedStorePath() << " is " << SeedStoreName(state) << " (euid " << geteuid()
      << ")";

  if (status != AMDSMI_STATUS_SUCCESS) {
    ExpectNothingReported(info);
    return;
  }

  // On a successful answer the fingerprint is always populated: an
  // unprovisioned node reports the fingerprint of the canonical fallback seed
  // rather than nothing, so that "these two nodes match" is answerable before
  // anyone has provisioned anything.
  bool any_set = false;
  for (uint8_t octet : info.fingerprint) {
    if (octet != 0) any_set = true;
  }
  EXPECT_TRUE(any_set) << "seed fingerprint should never be all zeroes";

  // And the flag agrees with the store it came from, which is the pairing that
  // went wrong: provisioned = 0 on a node whose store exists and holds a key.
  EXPECT_EQ(info.provisioned == 0, state == SeedStore::kAbsent)
      << "reported provisioned = " << info.provisioned << " with a key store that is "
      << SeedStoreName(state);
}

// The unprovisioned fingerprint is a specific known value, not merely a
// non-zero one. "Not all zeroes" is satisfied by fingerprinting the wrong
// bytes; this is not. It also pins the value across releases, which is what
// makes "do these two nodes carry the same seed" answerable at all -- a
// fingerprint that drifted between versions would report two identically
// provisioned nodes as different.
TEST(GpuUnit, CuidSeedFingerprintMatchesTheCanonicalFallbackSeed) {
  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  amdsmi_cuid_seed_info_t info = {};
  const amdsmi_status_t status = amdsmi_get_cuid_seed_info(&info);
  if (!kCuidBuiltIn) {
    ASSERT_EQ(status, AMDSMI_STATUS_NOT_SUPPORTED);
    return;
  }

  const SeedStore state = SeedStoreState();
  const std::string fallback = HexOf(kFallbackSeedFingerprint, sizeof(kFallbackSeedFingerprint));
  ASSERT_EQ(status, ExpectedSeedInfoStatus(state))
      << "key store " << SeedStorePath() << " is " << SeedStoreName(state) << " (euid " << geteuid()
      << ")";

  switch (state) {
    case SeedStore::kAbsent: {
      // The one state in which the canonical constant genuinely applies, and
      // the reason it is pinned: nothing has been provisioned, so the node
      // derives with the public 24-octet seed and must say so with that seed's
      // fingerprint. Reach this state by pointing AMDCUID_HMAC_KEY_PATH at a
      // path that does not exist before launching this binary.
      EXPECT_EQ(info.provisioned, 0) << "no key store, so nothing is provisioned";
      EXPECT_EQ(HexOf(info.fingerprint, sizeof(info.fingerprint)), fallback)
          << "unprovisioned node should fingerprint the fallback seed";
      break;
    }
    case SeedStore::kProvisioned: {
      // A node that reports itself provisioned while still deriving with the
      // public seed is the worst of both: every derived CUID on it is
      // reproducible by anyone, and the flag says otherwise.
      EXPECT_EQ(info.provisioned, 1) << "a readable key store holding a key is provisioned";
      EXPECT_NE(HexOf(info.fingerprint, sizeof(info.fingerprint)), fallback)
          << "provisioned node reports the public fallback fingerprint";
      break;
    }
    case SeedStore::kUnreadable:
    case SeedStore::kCorrupt: {
      // The defect this pins. The old answer here was SUCCESS, provisioned = 0
      // and the fallback fingerprint -- an unprivileged caller on a provisioned
      // node being told the node carries the public seed. That is a wrong
      // answer rather than a missing one, and it is worse than a failure: an
      // operator auditing a fleet reads it as "this node was never
      // provisioned". Nothing may be reported unless it is known.
      ExpectNothingReported(info);
      break;
    }
  }
}

// The seed is write-only through this API. This test is the executable form of
// that requirement: if a future change adds a way to read it back, the struct
// grows a field and this stops being true.
TEST(GpuUnit, CuidSeedInfoCarriesNoSeedMaterial) {
  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  // The shape of the struct is the requirement, and it holds in every state and
  // in a build without the library: 8 octets of digest, and nothing else that
  // could hold 32 octets of secret. Asserting it only on a successful query --
  // which this did -- made a property of the ABI conditional on the machine.
  amdsmi_cuid_seed_info_t info = {};
  EXPECT_EQ(sizeof(info.fingerprint), 8u);
  EXPECT_LT(sizeof(info.fingerprint), 32u);

  const amdsmi_status_t status = amdsmi_get_cuid_seed_info(&info);
  if (!kCuidBuiltIn) {
    ASSERT_EQ(status, AMDSMI_STATUS_NOT_SUPPORTED);
    return;
  }

  const SeedStore state = SeedStoreState();
  ASSERT_EQ(status, ExpectedSeedInfoStatus(state))
      << "key store " << SeedStorePath() << " is " << SeedStoreName(state) << " (euid " << geteuid()
      << ")";

  // A failed query discloses less, never more.
  if (status != AMDSMI_STATUS_SUCCESS) ExpectNothingReported(info);

  // The struct carries 32 octets of reserved space -- exactly the width of a
  // seed. It is the one place a future change could park seed material without
  // widening the fingerprint, so assert it comes back as it went in, in every
  // state, rather than trusting that nobody will.
  for (size_t i = 0; i < sizeof(info.reserved_flags); ++i) {
    EXPECT_EQ(info.reserved_flags[i], 0) << "reserved_flags[" << i << "] carries something";
  }
  for (size_t i = 0; i < sizeof(info.reserved) / sizeof(info.reserved[0]); ++i) {
    EXPECT_EQ(info.reserved[i], 0u) << "reserved[" << i << "] carries something";
  }
}

// The decoder the auxiliary check below relies on, against the published
// conformance vectors. Without this, "the flag agrees with the value" is only
// as good as the decoder, and a decoder that reads the wrong octet agrees with
// a producer that writes the wrong octet. Needs neither a GPU nor libamdcuid.
//
// Payload octet 14 of each vector, mask 0x20, is the Auxiliary Value
// Identifier; the expectation below is that bit taken from
// projects/cuid/tests/vectors/cuid_vectors.txt.
TEST(GpuUnit, CuidAuxiliaryBitDecoderMatchesConformanceVectors) {
  const struct {
    const char* name;
    const char* uuid;
    bool auxiliary;
  } kVectors[] = {
      {"P-1", "d4abaad3-9b34-8c50-9800-028dcc084200", false},
      {"P-2", "ffeb5272-7771-88c8-b800-028dcc084200", false},
      {"U-1", "d4abaad3-9b34-8c50-988c-028dcc084204", false},
      {"T-PLATFORM", "d4abaad3-9b34-8c50-9800-028dcc084000", false},
      {"T-CPU", "d4abaad3-9b34-8c50-9800-028dcc084100", false},
      {"T-GPU", "d4abaad3-9b34-8c50-9800-028dcc084200", false},
      {"T-NIC", "d4abaad3-9b34-8c50-9800-028dcc084300", false},
      {"T-NPU", "d4abaad3-9b34-8c50-9800-028dcc084001", false},
      {"T-OTHER", "d4abaad3-9b34-8c50-9800-028dcc084303", false},
      {"D-1", "61ffe99a-b3e0-8e16-a802-4b1d515d5438", false},
      {"D-2", "73488f9e-ea52-86ce-8401-2627fa41b068", false},
      {"A-1", "3395667e-f8fa-840e-bc00-028dcc084280", true},
      {"A-2", "808fa59f-6949-80ef-a400-c97fe9a704f0", true},
  };

  for (const auto& vector : kVectors) {
    bool decoded = false;
    ASSERT_TRUE(AuxiliaryBitFromUuidString(vector.uuid, &decoded)) << vector.name;
    EXPECT_EQ(decoded, vector.auxiliary) << vector.name << ": " << vector.uuid;
  }
}

TEST(GpuUnit, CuidSnapshotIsSelfConsistent) {
  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  const auto handles = GpuHandles();
  if (handles.empty()) GTEST_SKIP() << "no GPU present";

  for (auto handle : handles) {
    amdsmi_cuid_info_t info = {};
    const amdsmi_status_t status = amdsmi_get_gpu_cuid_info(handle, &info);
    if (status == AMDSMI_STATUS_NOT_SUPPORTED) {
      GTEST_SKIP() << (kCuidBuiltIn ? "no CUID for this device"
                                    : "built without CUID support; asserted by "
                                      "CuidEntryPointsNotSupportedWithoutTheLibrary");
    }
    ASSERT_EQ(status, AMDSMI_STATUS_SUCCESS);

    // The derived CUID is the value an unprivileged caller is meant to get, so
    // it is always populated on success.
    EXPECT_TRUE(LooksLikeUuid(info.derived)) << "derived: " << info.derived;

    // Version nibble is always 8, auxiliary or not. A consumer parses every
    // CUID with one code path.
    EXPECT_EQ(info.derived[14], '8') << "derived: " << info.derived;

    EXPECT_EQ(info.component_type, AMDSMI_CUID_COMPONENT_GPU);

    // The reported auxiliary flag must agree with payload bit 117 of the value
    // it describes. A producer that reads the flag out of the wrong place --
    // which has happened -- emits an auxiliary value that reports itself as
    // canonical.
    bool aux_from_value = false;
    ASSERT_TRUE(AuxiliaryBitFromUuidString(info.derived, &aux_from_value));
    EXPECT_EQ(static_cast<bool>(info.auxiliary), aux_from_value);

    // The primary is privilege-gated, and which answer is correct depends on
    // who is running: libamdcuid gates its primary query on an effective UID of
    // zero, so a non-root caller must get the empty string and a root caller
    // must get a value. Asserting only "empty or well-formed" accepted both
    // answers from both callers, which is no assertion at all -- a snapshot
    // that handed an unprivileged process the serial-bearing primary passed.
    // Each run asserts the branch it is in; between an ordinary CI run and a
    // developer's sudo run, both get exercised.
    if (geteuid() == 0) {
      EXPECT_NE(info.primary[0], '\0') << "root caller should receive the primary CUID";
      if (info.primary[0] != '\0') {
        EXPECT_TRUE(LooksLikeUuid(info.primary)) << "primary: " << info.primary;
        EXPECT_EQ(info.primary[14], '8');
      }
    } else {
      EXPECT_EQ(info.primary[0], '\0')
          << "unprivileged caller should receive an empty primary, got: " << info.primary;
    }
  }
}

// The source field, against a fabricated sysfs tree. Two things are asserted,
// and they are different things:
//
//   * that amd-smi reports a driver-published derived CUID as driver-sourced.
//     Whether the driver publishes anything is a property of the machine, so
//     the attribute is fabricated under AMDSMI_CUID_SYSFS_ROOT and the case
//     runs on any machine with a GPU rather than only on one with the CUID
//     driver loaded. This is amd-smi's own resolution, not libamdcuid's.
//
//   * that where the driver really does publish cuid_secondary, the derived
//     CUID that comes back is the driver's value verbatim. That is the whole
//     point of the driver stage -- the kernel and the library producing
//     different values for one device is the failure this work exists to
//     prevent -- and it is only checkable where the attribute is real, so it
//     is skipped where it is not.
TEST(GpuUnit, CuidSourceIsDriverWhenTheAttributeIsPublished) {
  if (!kCuidBuiltIn) GTEST_SKIP() << "built without CUID support";

  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  const auto handles = GpuHandles();
  if (handles.empty()) GTEST_SKIP() << "no GPU present";

  ScopedTempDir published;
  ScopedTempDir bare;
  ASSERT_TRUE(published.valid() && bare.valid()) << "could not create a temporary sysfs root";
  ASSERT_TRUE(MakeDirs(bare.path() + "/bus/pci/devices"));

  std::vector<std::string> bdfs;
  for (auto handle : handles) {
    amdsmi_bdf_t bdf = {};
    ASSERT_EQ(amdsmi_get_gpu_device_bdf(handle, &bdf), AMDSMI_STATUS_SUCCESS);
    const std::string bdf_str = BdfString(bdf);
    bdfs.push_back(bdf_str);

    const std::string dir = published.path() + "/bus/pci/devices/" + bdf_str;
    ASSERT_TRUE(MakeDirs(dir)) << dir;
    std::ofstream attr(dir + "/cuid_secondary");
    ASSERT_TRUE(attr.is_open()) << dir;
    attr << "00000000-0000-8000-8000-000000000000\n";
  }

  for (size_t i = 0; i < handles.size(); ++i) {
    {
      const ScopedSysfsRoot root(published.path());
      amdsmi_cuid_info_t info = {};
      const amdsmi_status_t status = amdsmi_get_gpu_cuid_info(handles[i], &info);
      if (status == AMDSMI_STATUS_NOT_SUPPORTED) {
        GTEST_SKIP() << "no CUID for " << bdfs[i];
      }
      ASSERT_EQ(status, AMDSMI_STATUS_SUCCESS);
      EXPECT_EQ(info.source, AMDSMI_CUID_SOURCE_DRIVER) << bdfs[i];
    }

    {
      // The same device with nothing published: not driver-sourced. Which of
      // the remaining stages answered is not observable from out here -- see
      // cuid_source_for() -- so the assertion is that amd-smi does not claim
      // the driver stood behind a value the driver never published.
      const ScopedSysfsRoot root(bare.path());
      amdsmi_cuid_info_t info = {};
      ASSERT_EQ(amdsmi_get_gpu_cuid_info(handles[i], &info), AMDSMI_STATUS_SUCCESS);
      EXPECT_NE(info.source, AMDSMI_CUID_SOURCE_DRIVER) << bdfs[i];
    }

    const std::string driver_value = DriverPublished(bdfs[i], "cuid_secondary");
    if (driver_value.empty()) continue;

    amdsmi_cuid_info_t info = {};
    ASSERT_EQ(amdsmi_get_gpu_cuid_info(handles[i], &info), AMDSMI_STATUS_SUCCESS);
    EXPECT_EQ(info.source, AMDSMI_CUID_SOURCE_DRIVER) << bdfs[i];
    EXPECT_EQ(std::string(info.derived), driver_value)
        << bdfs[i] << ": amd-smi and the driver disagree about the derived CUID";
  }
}

// One value, one lookup path. Two paths to one identifier is how the kernel and
// the library came to disagree in the first place.
TEST(GpuUnit, CuidSingleStringCallMatchesSnapshot) {
  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  const auto handles = GpuHandles();
  if (handles.empty()) GTEST_SKIP() << "no GPU present";

  for (auto handle : handles) {
    amdsmi_cuid_info_t info = {};
    if (amdsmi_get_gpu_cuid_info(handle, &info) != AMDSMI_STATUS_SUCCESS) continue;

    char cuid[AMDSMI_GPU_CUID_SIZE] = {};
    unsigned int length = sizeof(cuid);
    ASSERT_EQ(amdsmi_get_gpu_device_cuid(handle, &length, cuid), AMDSMI_STATUS_SUCCESS);
    EXPECT_STREQ(cuid, info.derived);
  }
}

// The legacy device UUID is retained and is a different value. Redefining it to
// return a CUID would change the meaning of a published ABI under consumers who
// have already recorded its output.
TEST(GpuUnit, CuidLegacyUuidIsStillItsOwnValue) {
  const AmdSmiSession session;
  ASSERT_EQ(session.status(), AMDSMI_STATUS_SUCCESS);

  const auto handles = GpuHandles();
  if (handles.empty()) GTEST_SKIP() << "no GPU present";

  for (auto handle : handles) {
    amdsmi_cuid_info_t info = {};
    if (amdsmi_get_gpu_cuid_info(handle, &info) != AMDSMI_STATUS_SUCCESS) continue;

    char uuid[AMDSMI_GPU_UUID_SIZE] = {};
    unsigned int length = sizeof(uuid);
    if (amdsmi_get_gpu_device_uuid(handle, &length, uuid) != AMDSMI_STATUS_SUCCESS) continue;

    EXPECT_TRUE(LooksLikeUuid(uuid)) << "uuid: " << uuid;
    EXPECT_STRNE(uuid, info.derived);
  }
}
