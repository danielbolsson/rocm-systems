// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#ifndef CUID_DEVICE_H
#define CUID_DEVICE_H

#include <cstdint>
#include <memory>
#include <string>

#include "include/amd_cuid.h"
#include "src/cuid_internal.h"
#include "src/hmac.h"

// Unpack the 109-bit hash out of a derived CUID's 16 raw payload octets.
// Shared by every path that reconstitutes a derived ID it did not compute
// itself -- the on-disk record and the driver interface -- so that all of them
// recover the same hash from the same value.
void get_hash_from_raw(uint8_t raw_bytes[16], uint8_t out_hash[14]);

class CuidDevice {
 public:
  virtual ~CuidDevice() = default;
  virtual amdcuid_device_type_t type() const = 0;
  virtual amdcuid_status_t get_primary_cuid(amdcuid_primary_id& id) const = 0;
  virtual amdcuid_status_t get_hardware_fingerprint(uint64_t& fingerprint) const = 0;
  // Stage 2 and 3 of the staged lookup: the daemon/config-file store, then the
  // library's own computation. Virtual so that a device class whose driver
  // publishes a CUID can put stage 1 -- the driver interface -- in front of it
  // and delegate here only when the driver has nothing to say. Two producers
  // that independently compute the same value eventually disagree and nothing
  // in the values reveals it, so where the kernel answers, it is the answer.
  virtual amdcuid_status_t get_derived_cuid(amdcuid_derived_id& id,
                                            cuid_hmac* hmac = nullptr) const;
  amdcuid_status_t is_temporary_cuid(bool* is_temporary) const;

  // Stage 1 of the staged lookup: read `attribute` (one of
  // CuidUtilities::kDriverPrimaryAttribute / kDriverSecondaryAttribute) for
  // this device's BDF, where it has one.
  //
  // Lives on the base class rather than on CuidGpu because the interface is a
  // property of the PCI device, not of amdgpu: a NIC or an NPU whose driver
  // grows the same attributes is answered by the kernel from that moment on,
  // with no further change here. A device class with no BDF -- the CPU and the
  // platform -- gets AMDCUID_STATUS_UNSUPPORTED from get_bdf() and falls
  // straight through.
  //
  // Returns AMDCUID_STATUS_UNSUPPORTED when the driver publishes nothing and
  // the caller should move on to the later stages. A present-but-unreadable
  // attribute is reported as PERMISSION_DENIED rather than as an absence: the
  // kernel holds the authoritative value, so computing a local one instead
  // would manufacture the divergence this ordering exists to prevent.
  amdcuid_status_t read_driver_published(const std::string& attribute, amdcuid_id_t& out,
                                         uint8_t raw_bits[16]) const;

  // Stage 1 for the primary specifically. Returns AMDCUID_STATUS_UNSUPPORTED
  // when the caller should carry on to the later stages, and any other failure
  // -- notably PERMISSION_DENIED on cuid_primary, which is CAP_SYS_ADMIN-gated
  // because its payload embeds the raw serial -- verbatim.
  amdcuid_status_t driver_primary_cuid(amdcuid_primary_id& id) const;

  // Virtual accessors for common device properties with default wrong device
  // type implementations
  virtual amdcuid_status_t get_vendor_id(uint16_t& vendor_id) const {
    vendor_id = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_family(uint16_t& family) const {
    family = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_model(uint16_t& model) const {
    model = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_device_id(uint16_t& device_id) const {
    device_id = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_revision_id(uint8_t& revision_id) const {
    revision_id = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_unit_id(uint16_t& unit_id) const {
    unit_id = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_pci_class(uint16_t& pci_class) const {
    pci_class = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_core(uint16_t& core) const {
    core = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_physical_id(uint16_t& physical_id) const {
    physical_id = 0;
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_bdf(std::string& bdf) const {
    bdf.clear();
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
  virtual amdcuid_status_t get_device_path(std::string& path) const {
    path.clear();
    return AMDCUID_STATUS_WRONG_DEVICE_TYPE;
  }
};

typedef std::shared_ptr<CuidDevice> DevicePtr;

#endif  // CUID_DEVICE_H
