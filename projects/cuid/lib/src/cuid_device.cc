// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#include "cuid_device.h"

#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <vector>

#include "cuid_cpu.h"
#include "cuid_file.h"
#include "cuid_gpu.h"
#include "cuid_nic.h"
#include "cuid_npu.h"
#include "cuid_platform.h"
#include "cuid_util.h"

namespace {

// helper function to get a hash from the raw bytes of a derived ID
void get_hash_from_raw(uint8_t raw_bytes[16], uint8_t out_hash[14]) {
  // just remove the reserved bits from the raw bytes to get the hash
  memcpy(out_hash, raw_bytes, 8);

  // byte 8 of raw bits is reserved which we can skip
  memcpy(&out_hash[8], &raw_bytes[9], 5);
  // The derived slot is 45 bits, so byte 14 carries only 5 hash bits: bit 5 is
  // the Auxiliary Value Identifier (payload bit 117) and bits 6:7 are reserved.
  // Recovering 6 bits here used to fold the auxiliary marker into the hash, so
  // an auxiliary derived ID written to the record and read back did not match
  // the one that was generated.
  out_hash[13] = raw_bytes[14] & 0x1F;
}

void build_derived_id_from_file_entry(const CuidFileEntry& entry, amdcuid_derived_id& id) {
  id.UUIDv8_representation = entry.derived_cuid;
  CuidUtilities::remove_UUIDv8_bits(&id.UUIDv8_representation, id.raw_bits);
  get_hash_from_raw(id.raw_bits, id.hash);
}

}  // namespace

amdcuid_status_t CuidDevice::get_derived_cuid(amdcuid_derived_id& id, cuid_hmac* hmac) const {
  // attempt to find the derived CUID in file first
  CuidFile derived_file(CuidUtilities::cuid_file(), false);
  amdcuid_status_t status = derived_file.load();

  if (status == AMDCUID_STATUS_SUCCESS) {
    amdcuid_device_type_t type = this->type();
    // there's only 1 platform entry, so handle that case first
    switch (type) {
      case AMDCUID_DEVICE_TYPE_PLATFORM: {
        // for platform, just return the first entry found
        CuidFileEntry entry;
        status = derived_file.find_by_device_type(AMDCUID_DEVICE_TYPE_PLATFORM, entry);
        if (status == AMDCUID_STATUS_SUCCESS) {
          build_derived_id_from_file_entry(entry, id);
          return AMDCUID_STATUS_SUCCESS;
        }
      } break;
      case AMDCUID_DEVICE_TYPE_GPU:
        // search by render node
        {
          auto gpu = reinterpret_cast<CuidGpu*>(const_cast<CuidDevice*>(this));
          if (gpu) {
            auto info = gpu->get_info();
            CuidFileEntry entry;
            status = derived_file.find_by_device_node(info.render_node, entry);
            if (status == AMDCUID_STATUS_SUCCESS) {
              build_derived_id_from_file_entry(entry, id);
              return AMDCUID_STATUS_SUCCESS;
            }
          }
        }
        break;
      case AMDCUID_DEVICE_TYPE_CPU: {
        auto cpu = reinterpret_cast<CuidCpu*>(const_cast<CuidDevice*>(this));
        if (cpu) {
          // Try device_node first - unique per logical CPU on SMT systems
          std::string device_path;
          if (cpu->get_device_path(device_path) == AMDCUID_STATUS_SUCCESS && !device_path.empty()) {
            CuidFileEntry entry;
            status = derived_file.find_by_device_node(device_path, entry);
            if (status == AMDCUID_STATUS_SUCCESS) {
              build_derived_id_from_file_entry(entry, id);
              return AMDCUID_STATUS_SUCCESS;
            }
          }
          const auto& info = cpu->get_info();
          CuidFileEntry entry;
          status = derived_file.find_by_package_id(info.header.fields.cpu.physical_id, entry);
          if (status == AMDCUID_STATUS_SUCCESS) {
            build_derived_id_from_file_entry(entry, id);
            return AMDCUID_STATUS_SUCCESS;
          }
        }
      } break;
      case AMDCUID_DEVICE_TYPE_NIC:
        // search by device node
        {
          auto nic = reinterpret_cast<CuidNic*>(const_cast<CuidDevice*>(this));
          if (nic) {
            const auto& info = nic->get_info();
            CuidFileEntry entry;
            amdcuid_status_t status =
                derived_file.find_by_device_node(info.network_interface, entry);
            if (status == AMDCUID_STATUS_SUCCESS) {
              build_derived_id_from_file_entry(entry, id);
              return AMDCUID_STATUS_SUCCESS;
            }
          }
        }
        break;
      case AMDCUID_DEVICE_TYPE_NPU:
        // search by accel node
        {
          auto npu = reinterpret_cast<CuidNpu*>(const_cast<CuidDevice*>(this));
          if (npu) {
            const auto& info = npu->get_info();
            CuidFileEntry entry;
            amdcuid_status_t status = derived_file.find_by_device_node(info.accel_node, entry);
            if (status == AMDCUID_STATUS_SUCCESS) {
              build_derived_id_from_file_entry(entry, id);
              return AMDCUID_STATUS_SUCCESS;
            }
          }
        }
        break;
      default:
        break;
        // Will expand with different devices as we implement them
    }
  }

  // if not found, generate derived CUID
  amdcuid_primary_id primary = {};
  if (get_primary_cuid(primary) != AMDCUID_STATUS_SUCCESS) {
    primary = {};  // the temp bit below was read uninitialised
  }
  // check the temporary bit in the primary CUID to determine whether to use the
  // real HMAC key or the temp key for derived CUID generation
  bool temp = primary.raw_bits[14] & 0x20;  // check the temp indicator bit in the reserved bits
  if (temp) {
    // An auxiliary CUID is derived with the fixed public temporary key, in the
    // same operand order as every other derivation: the key is the constant and
    // the message is the 16 auxiliary primary octets.
    //
    // This previously ran the other way round -- key = the primary payload,
    // message = a fixed application UUID -- on the argument that the machine ID
    // inside the primary needed protecting. It does not gain anything: with a
    // public key HMAC is a keyed hash, and its preimage resistance protects the
    // message whichever way round the operands go. What the swap did cost was a
    // second derivation path and a real defect -- generate_derived_cuid() reads
    // bit 117 out of whatever it is handed as the primary, so with a fixed
    // constant in that position the derived value was never marked auxiliary.
    cuid_hmac temp_hmac(kTemporaryKey, kTemporaryKeyLen);
    status = temp_hmac.set_hmac_algorithm("SHA256");
    if (status != AMDCUID_STATUS_SUCCESS) return status;
    status = CuidUtilities::generate_derived_cuid(&primary, &id, &temp_hmac);
  } else {
    status = CuidUtilities::generate_derived_cuid(&primary, &id, hmac);
  }

  return status;
}

amdcuid_status_t CuidDevice::is_temporary_cuid(bool* is_temp) const {
  if (!is_temp) {
    return AMDCUID_STATUS_INVALID_ARGUMENT;
  }
  // Check the temporary bit in the primary CUID to determine if the CUID is
  // temporary
  amdcuid_primary_id primary;
  amdcuid_status_t status = get_primary_cuid(primary);
  if (status != AMDCUID_STATUS_SUCCESS) {
    return status;
  }
  *is_temp = primary.raw_bits[14] & 0x20;  // check the temp indicator bit in the reserved bits

  return AMDCUID_STATUS_SUCCESS;
}
