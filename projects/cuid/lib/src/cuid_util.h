// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#ifndef CUID_UTIL_H
#define CUID_UTIL_H

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "hmac.h"
#include "include/amd_cuid.h"
#include "src/cuid_internal.h"

enum LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
 public:
  static Logger& instance() {
    static Logger logger_;
    return logger_;
  }

  void set_level(LogLevel level) { level_ = level; }
  LogLevel level() const { return level_; }

  const char* LogLevelName(LogLevel level) const;

  void log(LogLevel level, const std::string& msg) const;

 private:
  Logger() : level_(INFO) {}
  LogLevel level_;
};

// NOLINTBEGIN(bugprone-macro-parentheses)
// `msg` is deliberately left unparenthesised. Callers pass a stream-
// continuation fragment such as `"failed: " << path`, which is only valid
// glued onto the left of `_log_stream_ <<`. Wrapping it would evaluate
// `const char[] << std::string` as an expression of its own, which does not
// compile. clang-tidy cannot see that, so the check is suppressed here rather
// than obeyed.
#define LOG(level, msg)                                  \
  do {                                                   \
    std::ostringstream _log_stream_;                     \
    _log_stream_ << msg;                                 \
    Logger::instance().log((level), _log_stream_.str()); \
  } while (0)
// NOLINTEND(bugprone-macro-parentheses)

namespace CuidUtilities {
// Thread-safe replacement for strerror(). strerror() returns a pointer into a
// static buffer, so two threads reporting errors at once can read a torn or
// wrong message. libamdcuid is linked into multithreaded hosts -- amd_smi and
// libhsa-runtime64.so among them -- so it must not use it.
std::string errno_string(int err);

// A zero hardware fingerprint is the absence of an identity, not an identity.
// Unprogrammed DSN capabilities and unconfigured MAC addresses both read back
// as all-zero, and reporting that as a successful fingerprint gives every such
// device on every machine the same primary CUID. Callers use this to convert
// "read succeeded, value is meaningless" into HW_FINGERPRINT_NOT_FOUND, which
// routes the device onto the temporary-CUID path it should have been on.
inline amdcuid_status_t validate_fingerprint(uint64_t fingerprint) {
  return (fingerprint == 0) ? AMDCUID_STATUS_HW_FINGERPRINT_NOT_FOUND : AMDCUID_STATUS_SUCCESS;
}

// The CUID attributes the amdgpu driver publishes per PCI device, under
// /sys/bus/pci/devices/<bdf>/. cuid_primary is 0400 and additionally gated on
// CAP_SYS_ADMIN because its payload embeds the raw serial number;
// cuid_secondary is 0444 and is the value unprivileged tools are expected to
// consume. cuid_seed also lives there and is deliberately absent from this
// list: it is the secret, and the library has no business reading it.
constexpr const char kDriverPrimaryAttribute[] = "cuid_primary";
constexpr const char kDriverSecondaryAttribute[] = "cuid_secondary";

// Read a driver-published CUID attribute and parse its RFC 9562 UUID string
// into `id`. `path` is the full path to the attribute file, so a test can point
// it at a fake sysfs root; read_driver_cuid() below is the BDF-based wrapper
// callers normally want.
//
// The three failure modes are deliberately distinct, because the caller acts
// differently on each:
//   AMDCUID_STATUS_FILE_NOT_FOUND    the attribute is not there. Either the
//                                    driver predates the CUID interface or it
//                                    found no serial for this device, in which
//                                    case there is no kernel CUID to have. This
//                                    is the ordinary case on most systems, so
//                                    it is not logged above DEBUG.
//   AMDCUID_STATUS_PERMISSION_DENIED the attribute exists but this process may
//                                    not read it -- an unprivileged reader of
//                                    cuid_primary. The kernel still has the
//                                    authoritative value, so the caller must
//                                    not treat this as "no driver value" and
//                                    compute a competing one.
//   AMDCUID_STATUS_INVALID_FORMAT    the attribute is there and readable but
//                                    does not hold a UUID.
//
// Never throws: sysfs is read with open()/read() rather than an ifstream both
// so that errno is meaningful and so that no exception escapes into hosts that
// build without them.
amdcuid_status_t read_driver_cuid_from_path(const std::string& path, amdcuid_id_t* id);

// Read `attribute` (kDriverPrimaryAttribute or kDriverSecondaryAttribute) for
// the device at `bdf`, in the standard "dddd:bb:dd.f" form.
amdcuid_status_t read_driver_cuid(const std::string& bdf, const std::string& attribute,
                                  amdcuid_id_t* id);

std::string read_sysfs_file(const std::string& path);
std::string readlink_bdf(const std::string& device_path);
std::string bdf_to_device_path(const std::string& bdf, amdcuid_device_type_t device_type);
std::string real_dev_path_from_fd(int fd);
std::string get_real_path(const std::string& path);
amdcuid_status_t generate_derived_cuid(const amdcuid_primary_id* primary_id,
                                       amdcuid_derived_id* derived_id, cuid_hmac* hmac);
// device_type is the enumeration, not an integer: it is written straight into
// the Component Type field, so a raw value must not be passable here.
amdcuid_status_t generate_primary_cuid(uint64_t serial_number, uint16_t unit_id,
                                       uint8_t revision_id, uint16_t device_id, uint16_t vendor_id,
                                       amdcuid_device_type_t device_type,
                                       amdcuid_primary_id* primary_id, bool temp = false);
void remove_UUIDv8_bits(amdcuid_id_t* id, uint8_t out_raw_bits[16]);
void add_UUIDv8_bits(const uint8_t raw_bits[16], amdcuid_id_t* id);
std::string get_cuid_as_string(const amdcuid_id_t* id);
amdcuid_status_t uuid_string_to_uint8(const std::string& uuid_str, uint8_t* uuid);
std::string device_type_to_string(amdcuid_device_type_t type);

bool is_valid_bdf(const std::string& bdf);

// Format field of the auxiliary input structure (bits 0:15).
constexpr uint16_t kAuxFormatPcie = 1;
constexpr uint16_t kAuxFormatCpu = 2;

// The auxiliary CUID's 256-bit input structure, as defined by the CUID
// specification with its two field boundaries repaired so the widths sum to
// 256 (Format 0:15 and Machine ID 16:143; the published table gave them 0-16
// and 17-143, which overlap and total 257).
//
//   bits   0:15   Format          1 = PCIe device, 2 = CPU
//   bits  16:143  Machine ID      128 bits, from /etc/machine-id
//   bits 144:175  PCIe Routing ID (segment<<16)|(bus<<8)|(device<<3)|function
//   bits 176:183  RevisionID      CPU: stepping
//   bits 184:199  DeviceID        CPU: family and model
//   bits 200:215  VendorID
//   bits 216:219  Component Type  on-wire numbering
//   bits 220:255  Reserved, zero
//
// Fixed-width binary, deliberately. This replaced a scheme that built a string
// and then stripped every character that was not a hex digit, which erased the
// separators: "0000:65:00.0" and "0000:65:0:00.0" reduced to the same input,
// and a CPU seed of "socket:" + id degenerated to the constant "cce" plus
// digits.
struct AuxiliaryInput {
  uint16_t format = 0;
  uint32_t routing_id = 0;
  uint8_t revision_id = 0;
  uint16_t device_id = 0;
  uint16_t vendor_id = 0;
  uint8_t component_type = 0;
};

// Pack "dddd:bb:dd.f" into the 32-bit Routing ID. Returns 0 for a malformed
// BDF, which is_valid_bdf() should have rejected already.
uint32_t routing_id_from_bdf(const std::string& bdf);

// The auxiliary serial: the first 8 octets of the unkeyed SHA-256 of the
// 32-octet input structure, little-endian. Placed in payload bits 0:63 of an
// otherwise normal primary that has bit 117 set.
amdcuid_status_t make_fallback_fingerprint(const AuxiliaryInput& input, uint64_t& fingerprint);

// GPU VF (SR-IOV Virtual Function) utilities
int extract_render_minor(const std::string& path);
uint16_t get_gpu_vf_id(const std::string& device_path);

inline const std::string& cuid_file() {
  static const std::string path = "/tmp/cuid";
  return path;
}
inline const std::string& priv_cuid_file() {
  static const std::string path = "/tmp/priv_cuid";
  return path;
}
}  // namespace CuidUtilities

#endif
