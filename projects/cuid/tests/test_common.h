// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#ifndef CUID_TEST_COMMON_H_
#define CUID_TEST_COMMON_H_

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "include/amd_cuid.h"
#include "src/hmac.h"

struct CUIDTstGlobals {
  uint32_t verbosity = 0;
  bool dont_fail = false;
};

extern CUIDTstGlobals sCUIDGlvalues;

// Conditionally execute a block at or above the given verbosity level.
#define IF_VERB(V) if (sCUIDGlvalues.verbosity >= (V))

// Assert that ret == AMDCUID_STATUS_SUCCESS, unless dont_fail is set.
#define CHK_ERR_ASRT(RET)                       \
  do {                                          \
    if (!sCUIDGlvalues.dont_fail) {             \
      ASSERT_EQ(AMDCUID_STATUS_SUCCESS, (RET)); \
    }                                           \
  } while (0)

// Put the node's provisioned seed back after a test that has to replace it.
//
// amdcuid_set_hash_key() writes the node's key store, and the store is the
// machine's, not the test's: every derived CUID for a component the kernel does
// not answer for is a function of it. The original is re-applied through the
// same call, so the store and the process's in-memory key end up agreeing again.
// Where there was no key to begin with the store is removed; the in-memory key
// cannot be un-set through the public API, which is why this only runs in a
// test that was going to change it anyway.
class KeyStoreGuard {
 public:
  KeyStoreGuard() {
    path_ = cuid_hmac().get_key_file_path();
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) return;
    present_ = std::fread(key_, 1, sizeof(key_), f) == sizeof(key_);
    std::fclose(f);
  }
  ~KeyStoreGuard() {
    if (present_) {
      amdcuid_set_hash_key(key_);
    } else {
      std::remove(path_.c_str());
    }
  }
  KeyStoreGuard(const KeyStoreGuard&) = delete;
  KeyStoreGuard& operator=(const KeyStoreGuard&) = delete;

 private:
  std::string path_;
  uint8_t key_[key_length] = {0};
  bool present_ = false;
};

void ProcessCmdline(CUIDTstGlobals* globals, int argc, char** argv);

#endif  // CUID_TEST_COMMON_H_
