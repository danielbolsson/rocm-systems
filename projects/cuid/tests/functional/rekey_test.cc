// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

// A derived CUID is a keyed function of the primary, so replacing the seed
// replaces every derived value on the node -- that is what re-keying is for,
// and the reason it is an administrative invalidation rather than a routine
// operation. But CuidDevice::get_derived_cuid() consults the record file before
// it derives anything, so a record written under the old seed outlives the key
// it was computed with: a node with an existing record went on serving its
// pre-re-key derived CUIDs indefinitely, which is the one outcome a re-key is
// supposed to make impossible.
//
// The second test here covers the other half of the same area: a privileged
// caller asking for a handle must be given the key, or a device the record
// names but holds no derived entry for cannot be answered at all.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "src/cuid_device.h"
#include "src/cuid_device_manager.h"
#include "src/cuid_file.h"
#include "src/cuid_util.h"
#include "src/hmac.h"
#include "test_common.h"

namespace {

// A component whose primary CUID is fixed and needs no hardware. Reported as a
// Platform because that is the one device type CuidDevice::get_derived_cuid()
// resolves against the record without downcasting to a concrete class.
class FixedPrimaryDevice : public CuidDevice {
 public:
  explicit FixedPrimaryDevice(uint64_t serial) {
    CuidUtilities::generate_primary_cuid(serial, 0, 0, 0, 0, AMDCUID_DEVICE_TYPE_PLATFORM,
                                         &primary_, false);
  }
  amdcuid_device_type_t type() const override { return AMDCUID_DEVICE_TYPE_PLATFORM; }
  amdcuid_status_t get_primary_cuid(amdcuid_primary_id& id) const override {
    id = primary_;
    return AMDCUID_STATUS_SUCCESS;
  }
  amdcuid_status_t get_hardware_fingerprint(uint64_t& fingerprint) const override {
    fingerprint = 1;
    return AMDCUID_STATUS_SUCCESS;
  }

 private:
  amdcuid_primary_id primary_ = {};
};

// Put the node's store back the way the test found it.
//
// Both files are restored, and the device manager is emptied: a lookup that
// resolves out of the record adopts the device it built into the manager, and
// a fabricated entry left there would be re-recorded by the next thing that
// saves the registry and then read as a real device by every test after that.
class NodeStoreGuard {
 public:
  NodeStoreGuard() {
    save(CuidUtilities::cuid_file(), &unpriv_);
    save(CuidUtilities::priv_cuid_file(), &priv_);
  }
  ~NodeStoreGuard() {
    CuidDeviceManager::instance().shutdown();
    restore(CuidUtilities::cuid_file(), unpriv_, unpriv_present_);
    restore(CuidUtilities::priv_cuid_file(), priv_, priv_present_);
  }
  NodeStoreGuard(const NodeStoreGuard&) = delete;
  NodeStoreGuard& operator=(const NodeStoreGuard&) = delete;

 private:
  void save(const std::string& path, std::vector<char>* into) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    const bool present = f != nullptr;
    if (f) {
      char buf[4096];
      size_t n = 0;
      while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        into->insert(into->end(), buf, buf + n);
      }
      std::fclose(f);
    }
    if (path == CuidUtilities::cuid_file()) {
      unpriv_present_ = present;
    } else {
      priv_present_ = present;
    }
  }
  static void restore(const std::string& path, const std::vector<char>& from, bool present) {
    if (!present) {
      std::remove(path.c_str());
      return;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    if (!from.empty()) std::fwrite(from.data(), 1, from.size(), f);
    std::fclose(f);
  }

  std::vector<char> unpriv_;
  std::vector<char> priv_;
  bool unpriv_present_ = false;
  bool priv_present_ = false;
};

// Record one derived CUID for the Platform, which is what FixedPrimaryDevice
// reports itself as and therefore what get_derived_cuid() will look up.
void RecordPlatformDerived(const std::string& path, bool privileged, const amdcuid_id_t& derived) {
  CuidFile file(path, privileged);
  CuidFileEntry entry;
  entry.device_type = AMDCUID_DEVICE_TYPE_PLATFORM;
  entry.derived_cuid = derived;
  entry.last_update = 0;
  ASSERT_EQ(file.add_entry(entry), AMDCUID_STATUS_SUCCESS);
  ASSERT_EQ(file.save(), AMDCUID_STATUS_SUCCESS);
}

bool SameId(const amdcuid_id_t& a, const amdcuid_id_t& b) {
  return std::memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}

// True when any entry of the record at `path` carries `derived`.
bool RecordHolds(const std::string& path, bool privileged, const amdcuid_id_t& derived) {
  CuidFile file(path, privileged);
  if (file.load() != AMDCUID_STATUS_SUCCESS) return false;
  for (const auto& entry : file.get_entries()) {
    if (SameId(entry.derived_cuid, derived)) return true;
  }
  return false;
}

}  // namespace

// The behaviour re-keying is defined by: after the seed is replaced, a query
// returns the value derived from the new seed and not the recorded one.
TEST(cuidtstUnprivileged, RekeyRetiresTheRecordedDerivedValue) {
  NodeStoreGuard guard;
  CuidDeviceManager& mgr = CuidDeviceManager::instance();

  // The record store is a fixed path shared with anything else on the node
  // that has written it, including a root-owned run of this same suite. If it
  // is not ours to replace there is nothing here to observe.
  std::remove(CuidUtilities::cuid_file().c_str());
  std::remove(CuidUtilities::priv_cuid_file().c_str());
  if (access(CuidUtilities::cuid_file().c_str(), F_OK) == 0) {
    GTEST_SKIP() << CuidUtilities::cuid_file()
                 << " is not writable by this user; run as its owner to enable.";
  }

  uint8_t key_a[32];
  uint8_t key_b[32];
  for (size_t i = 0; i < sizeof(key_a); ++i) {
    key_a[i] = static_cast<uint8_t>(0x10 + i);
    key_b[i] = static_cast<uint8_t>(0xA0 + i);
  }
  cuid_hmac hmac_a(key_a);
  cuid_hmac hmac_b(key_b);

  const FixedPrimaryDevice device(0xD3AAABD406C5349Bull);
  amdcuid_primary_id primary = {};
  ASSERT_EQ(device.get_primary_cuid(primary), AMDCUID_STATUS_SUCCESS);

  // The two values this component has, one per seed. Derived directly, so
  // these are the seeds' own answers with no record in the way.
  amdcuid_derived_id under_a = {};
  ASSERT_EQ(CuidUtilities::generate_derived_cuid(&primary, &under_a, &hmac_a),
            AMDCUID_STATUS_SUCCESS);
  amdcuid_derived_id under_b = {};
  ASSERT_EQ(CuidUtilities::generate_derived_cuid(&primary, &under_b, &hmac_b),
            AMDCUID_STATUS_SUCCESS);
  ASSERT_FALSE(SameId(under_a.UUIDv8_representation, under_b.UUIDv8_representation))
      << "the seed must determine the derived value, or this test proves nothing";

  // Record the value that was handed out under the old seed.
  RecordPlatformDerived(CuidUtilities::cuid_file(), false, under_a.UUIDv8_representation);
  ASSERT_TRUE(RecordHolds(CuidUtilities::cuid_file(), false, under_a.UUIDv8_representation));

  // The record is consulted ahead of the key, which is what makes this a
  // defect rather than a caching detail: the new seed is already in hand here
  // and the old value comes back anyway.
  {
    amdcuid_derived_id served = {};
    ASSERT_EQ(device.get_derived_cuid(served, &hmac_b), AMDCUID_STATUS_SUCCESS);
    ASSERT_TRUE(SameId(served.UUIDv8_representation, under_a.UUIDv8_representation))
        << "the record is expected to shadow the key until it is invalidated";
  }

  // Nothing is regenerated for devices this process has not discovered, so the
  // assertion below is about the invalidation and not about rediscovery.
  mgr.shutdown();

  ASSERT_EQ(mgr.invalidate_derived_cuids(key_b), AMDCUID_STATUS_SUCCESS);

  {
    amdcuid_derived_id served = {};
    ASSERT_EQ(device.get_derived_cuid(served, &hmac_b), AMDCUID_STATUS_SUCCESS);
    EXPECT_FALSE(SameId(served.UUIDv8_representation, under_a.UUIDv8_representation))
        << "a derived CUID recorded under the previous seed survived the re-key";
    EXPECT_TRUE(SameId(served.UUIDv8_representation, under_b.UUIDv8_representation))
        << "the value served after a re-key must be the one the new seed derives";
  }
}

// The same thing through the public entry point. Provisioning requires
// privilege, so this is where the end-to-end assertion has to live.
TEST(cuidtstPrivileged, SetHashKeyRetiresRecordedDerivedValues) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "Requires root; run with sudo to enable.";
  }
  NodeStoreGuard guard;
  KeyStoreGuard key_guard;

  // A derived CUID no seed would ever produce, so finding it afterwards can
  // only mean the record outlived the key.
  amdcuid_id_t sentinel = {{0}};
  ASSERT_EQ(
      CuidUtilities::uuid_string_to_uint8("abababab-abab-8bab-9bab-abababababab", sentinel.bytes),
      AMDCUID_STATUS_SUCCESS);
  RecordPlatformDerived(CuidUtilities::cuid_file(), false, sentinel);
  RecordPlatformDerived(CuidUtilities::priv_cuid_file(), true, sentinel);
  ASSERT_TRUE(RecordHolds(CuidUtilities::cuid_file(), false, sentinel));

  uint8_t key[32] = {0};
  ASSERT_EQ(amdcuid_generate_hash_key(key), AMDCUID_STATUS_SUCCESS);
  ASSERT_EQ(amdcuid_set_hash_key(key), AMDCUID_STATUS_SUCCESS);

  // Whether the store was regenerated from this process's devices or simply
  // dropped, the pre-re-key value must not still be in it.
  EXPECT_FALSE(RecordHolds(CuidUtilities::cuid_file(), false, sentinel))
      << "a derived CUID recorded under the previous seed survived amdcuid_set_hash_key()";
  EXPECT_FALSE(RecordHolds(CuidUtilities::priv_cuid_file(), true, sentinel))
      << "a derived CUID recorded under the previous seed survived amdcuid_set_hash_key()";
}

// A privileged caller that has the key must be allowed to use it. The record
// can name a device -- by BDF, in the privileged store -- without holding a
// derived entry the device's own lookup will match, and the lookup then has to
// derive. Handing it no key made that return INVALID_ARGUMENT, which amd-smi
// reports as NOT_SUPPORTED for a device it can see.
TEST(cuidtstPrivileged, HandleByBdfDerivesWhenTheRecordHasNoDerivedEntry) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "Requires root; run with sudo to enable.";
  }
  NodeStoreGuard guard;

  // A BDF no device occupies, so nothing about this test depends on what is
  // plugged into the machine: sysfs has no driver-published CUID for it and
  // its configuration space cannot be read.
  const std::string kBdf = "0000:ff:1f.7";
  const std::string kNode = "/sys/class/drm/renderD9999";

  amdcuid_id_t primary = {{0}};
  ASSERT_EQ(
      CuidUtilities::uuid_string_to_uint8("d4abaad3-9b34-8c50-9800-028dcc084200", primary.bytes),
      AMDCUID_STATUS_SUCCESS);

  // The privileged store names the device but no derived entry for it exists
  // anywhere the device's own lookup will find one: the unprivileged record is
  // where get_derived_cuid() looks, and it is not there.
  {
    CuidFile file(CuidUtilities::priv_cuid_file(), true);
    CuidFileEntry entry;
    entry.device_type = AMDCUID_DEVICE_TYPE_GPU;
    entry.device_index = 0;
    entry.primary_cuid = primary;
    entry.bdf = kBdf;
    entry.device_node = kNode;
    entry.vendor_id = 0x1002;
    entry.is_temporary = false;
    entry.last_update = 0;
    ASSERT_EQ(file.add_entry(entry), AMDCUID_STATUS_SUCCESS);
    ASSERT_EQ(file.save(), AMDCUID_STATUS_SUCCESS);
  }
  std::remove(CuidUtilities::cuid_file().c_str());

  amdcuid_id_t handle = {{0}};
  const amdcuid_status_t status =
      amdcuid_get_handle_by_bdf(kBdf.c_str(), AMDCUID_DEVICE_TYPE_GPU, &handle);

  EXPECT_NE(status, AMDCUID_STATUS_INVALID_ARGUMENT)
      << "a privileged caller was refused because the lookup was not given the key";
  ASSERT_EQ(status, AMDCUID_STATUS_SUCCESS) << amdcuid_status_to_string(status);

  IF_VERB(1) { printf("  handle: %s\n", amdcuid_id_to_string(handle)); }

  // What came back is a derivation and not a placeholder. Which key the process
  // opened was settled before main(), so the octets are not predictable here --
  // the conformance vectors pin those -- but a derived CUID is a constructed
  // UUIDv8, is not the primary it was derived from, and is not zero.
  const amdcuid_id_t zero = {{0}};
  EXPECT_FALSE(SameId(handle, zero));
  EXPECT_FALSE(SameId(handle, primary));
  EXPECT_TRUE(CuidUtilities::is_constructed(&handle));

  // Not the auxiliary path either: an auxiliary derived CUID is keyed with the
  // public temporary key, which is what this lookup would fall back to if it
  // had failed to reach the recorded primary at all.
  uint8_t raw[16] = {0};
  amdcuid_id_t framed = handle;
  CuidUtilities::remove_UUIDv8_bits(&framed, raw);
  EXPECT_EQ(raw[14] & 0x20, 0) << "the derived CUID was marked auxiliary";

  // And it is stable: the same request answers the same way.
  amdcuid_id_t again = {{0}};
  ASSERT_EQ(amdcuid_get_handle_by_bdf(kBdf.c_str(), AMDCUID_DEVICE_TYPE_GPU, &again),
            AMDCUID_STATUS_SUCCESS);
  EXPECT_TRUE(SameId(handle, again));
}
