/*
 * Copyright (c) 2025, ValkeySearch contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VALKEYSEARCH_SRC_RDB_SERIALIZATION_H_
#define VALKEYSEARCH_SRC_RDB_SERIALIZATION_H_

#include <cstddef>
#include <cstdlib>

#include "absl/algorithm/algorithm.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "src/metrics.h"
#include "src/rdb_section.pb.h"
#include "third_party/hnswlib/iostream.h"
#include "vmsdk/src/log.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/status/status_macros.h"
#include "vmsdk/src/type_conversions.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"

namespace valkey_search {

constexpr uint32_t kCurrentEncVer = 1;
// Format is 0xMMmmpp (M=major, m=minor, p=patch)
constexpr uint64_t kCurrentSemanticVersion = 0x010000;
constexpr absl::string_view kValkeySearchModuleTypeName{"Vk-Search"};

std::string HumanReadableSemanticVersion(uint64_t semantic_version) {
  return absl::StrFormat("%d.%d.%d", (semantic_version >> 16) & 0xFF,
                         (semantic_version >> 8) & 0xFF,
                         semantic_version & 0xFF);
}

/* SafeRDB wraps a RedisModuleIO object and performs IO error checking,
 * returning absl::StatusOr to force error handling on the caller side. */
class SafeRDB {
 public:
  explicit SafeRDB(RedisModuleIO *rdb) : rdb_(rdb) {}

  virtual absl::Status LoadSizeT(size_t &val) {
    val = RedisModule_LoadUnsigned(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadUnsigned failed");
    }
    return absl::OkStatus();
  }

  virtual absl::Status LoadUnsigned(unsigned int &val) {
    val = RedisModule_LoadUnsigned(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadUnsigned failed");
    }
    return absl::OkStatus();
  }

  virtual absl::Status LoadSigned(int &val) {
    val = RedisModule_LoadSigned(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadSigned failed");
    }
    return absl::OkStatus();
  }

  virtual absl::Status LoadDouble(double &val) {
    val = RedisModule_LoadDouble(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadDouble failed");
    }
    return absl::OkStatus();
  }

  virtual absl::StatusOr<vmsdk::UniqueRedisString> LoadString() {
    auto str = vmsdk::UniqueRedisString(RedisModule_LoadString(rdb_));
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadString failed");
    }
    return str;
  }
  bool operator==(const SafeRDB &other) const { return rdb_ == other.rdb_; }

  virtual absl::Status SaveSizeT(size_t val) {
    RedisModule_SaveUnsigned(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveUnsigned failed");
    }
    return absl::OkStatus();
  }

  virtual absl::Status SaveUnsigned(unsigned int val) {
    RedisModule_SaveUnsigned(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveUnsigned failed");
    }
    return absl::OkStatus();
  }

  virtual absl::Status SaveSigned(int val) {
    RedisModule_SaveSigned(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveSigned failed");
    }
    return absl::OkStatus();
  }

  virtual absl::Status SaveDouble(double val) {
    RedisModule_SaveDouble(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveDouble failed");
    }
    return absl::OkStatus();
  }

  virtual absl::Status SaveStringBuffer(const char *str, size_t len) {
    if (len == 0) {
      return absl::InvalidArgumentError("len must be > 0");
    }
    RedisModule_SaveStringBuffer(rdb_, str, len);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveStringBuffer failed");
    }
    return absl::OkStatus();
  }

 protected:
  SafeRDB() = default;

 private:
  RedisModuleIO *rdb_;
};

/* SupplementalContentChunkIter is an iterator over chunks of a supplemental
 * content section in the RDB. */
class SupplementalContentChunkIter {
 public:
  SupplementalContentChunkIter() = delete;
  SupplementalContentChunkIter(SafeRDB *rdb) : rdb_(rdb) {
    // Buffer one chunk ahead so that done_ is correctly reflected.
    ReadNextChunk();
  }

  SupplementalContentChunkIter(SupplementalContentChunkIter &&other) noexcept
      : rdb_(other.rdb_),
        curr_chunk_(std::move(other.curr_chunk_)),
        done_(other.done_) {
    other.curr_chunk_ = absl::InternalError("Use after move");
  };
  SupplementalContentChunkIter(SupplementalContentChunkIter &other) = delete;
  SupplementalContentChunkIter &operator=(
      SupplementalContentChunkIter &&other) noexcept {
    rdb_ = other.rdb_;
    done_ = other.done_;
    curr_chunk_ = std::move(other.curr_chunk_);
    other.curr_chunk_ = absl::InternalError("Use after move");
    return *this;
  }
  SupplementalContentChunkIter &operator=(SupplementalContentChunkIter &other) =
      delete;

  absl::StatusOr<std::unique_ptr<data_model::SupplementalContentChunk>> Next() {
    if (curr_chunk_.ok()) {
      std::unique_ptr<data_model::SupplementalContentChunk> result =
          std::move(*curr_chunk_);
      ReadNextChunk();
      return result;
    }
    return curr_chunk_.status();
  }
  bool HasNext() const { return !done_; }

 private:
  void ReadNextChunk() {
    if (done_) {
      curr_chunk_ = absl::NotFoundError("No more elements remaining");
      return;
    }
    auto serialized_chunk = rdb_->LoadString();
    if (!serialized_chunk.ok()) {
      curr_chunk_ = absl::InternalError(
          "IO error while reading serialized SupplementalContentChunk from "
          "RDB");
      return;
    }
    curr_chunk_ = std::make_unique<data_model::SupplementalContentChunk>();
    if (!(*curr_chunk_)
             ->ParseFromString(vmsdk::ToStringView(serialized_chunk->get()))) {
      curr_chunk_ = absl::InternalError(
          "Failed to deserialize "
          "SupplementalContentChunk read from RDB");
      return;
    }
    done_ = !(*curr_chunk_)->has_binary_content();
  }
  SafeRDB *rdb_;
  absl::StatusOr<std::unique_ptr<data_model::SupplementalContentChunk>>
      curr_chunk_;
  bool done_ = false;
};

/* SupplementalContentIter implements an iterator over the SupplementalContent
 * of an RDBSection. After each iteration via Next(), IterateChunks() must be
 * called and iterated or RDB loading will not succeed. */
class SupplementalContentIter {
 public:
  SupplementalContentIter() = delete;
  SupplementalContentIter(SafeRDB *rdb, size_t remaining)
      : rdb_(rdb), remaining_(remaining) {}

  SupplementalContentIter(SupplementalContentIter &&other) noexcept
      : rdb_(std::move(other.rdb_)), remaining_(other.remaining_) {
    other.remaining_ = 0;
  };
  SupplementalContentIter(SupplementalContentIter &other) = delete;
  SupplementalContentIter &operator=(SupplementalContentIter &&other) noexcept {
    rdb_ = std::move(other.rdb_);
    remaining_ = other.remaining_;
    other.remaining_ = 0;
    return *this;
  }
  SupplementalContentIter &operator=(SupplementalContentIter &other) = delete;

  absl::StatusOr<std::unique_ptr<data_model::SupplementalContentHeader>>
  Next() {
    if (remaining_ == 0) {
      return absl::NotFoundError("No more supplemental content chunks");
    }
    VMSDK_ASSIGN_OR_RETURN(auto serialized_supplemental_content_header,
                           rdb_->LoadString(),
                           _ << "IO error while reading serialized "
                                "SupplementalContentHeader from RDB");
    auto result = std::make_unique<data_model::SupplementalContentHeader>();
    if (!result->ParseFromString(vmsdk::ToStringView(
            serialized_supplemental_content_header.get()))) {
      return absl::InternalError(
          "Failed to deserialize SupplementalContentHeader read from RDB");
    }
    remaining_--;
    return result;
  }
  bool HasNext() { return remaining_ > 0; }
  SupplementalContentChunkIter IterateChunks() { return {rdb_}; }

 private:
  SafeRDB *rdb_;
  size_t remaining_;
};

/* RDBSectionIter implements an iterator over the RDBSections contained within
 * the RDB. After each iteration via Next(), IterateSupplementalContent() must
 * be called and iterated or RDB loading will not succeed. */
class RDBSectionIter {
 public:
  RDBSectionIter() = delete;
  RDBSectionIter(SafeRDB *rdb, size_t remaining)
      : rdb_(rdb), remaining_(remaining) {}

  RDBSectionIter(RDBSectionIter &&other) noexcept
      : rdb_(std::move(other.rdb_)),
        remaining_(other.remaining_),
        curr_supplemental_count_(other.curr_supplemental_count_) {
    other.remaining_ = 0;
  };
  RDBSectionIter(RDBSectionIter &other) = delete;
  RDBSectionIter &operator=(RDBSectionIter &&other) noexcept {
    rdb_ = std::move(other.rdb_);
    remaining_ = other.remaining_;
    curr_supplemental_count_ = other.curr_supplemental_count_;
    other.remaining_ = 0;
    other.curr_supplemental_count_ = 0;
    return *this;
  }
  RDBSectionIter &operator=(RDBSectionIter &other) = delete;

  SupplementalContentIter IterateSupplementalContent() {
    return {rdb_, curr_supplemental_count_};
  }
  absl::StatusOr<std::unique_ptr<data_model::RDBSection>> Next() {
    VMSDK_ASSIGN_OR_RETURN(
        auto serialized_rdb_section, rdb_->LoadString(),
        _ << "IO error while reading serialized RDBSection from RDB");
    auto result = std::make_unique<data_model::RDBSection>();
    if (!result->ParseFromString(
            std::string(vmsdk::ToStringView(serialized_rdb_section.get())))) {
      return absl::InternalError(
          "Failed to deserialize RDBSection read from RDB");
    }
    remaining_--;
    curr_supplemental_count_ = result->supplemental_count();
    return result;
  }
  bool HasNext() { return remaining_ > 0; }

 private:
  SafeRDB *rdb_;
  size_t remaining_;
  size_t curr_supplemental_count_ = 0;
};

class RDBChunkInputStream : public hnswlib::InputStream {
 public:
  explicit RDBChunkInputStream(SupplementalContentChunkIter &&iter)
      : iter_(std::move(iter)) {}

  RDBChunkInputStream(RDBChunkInputStream &&other) noexcept
      : iter_(std::move(other.iter_)) {}
  RDBChunkInputStream(RDBChunkInputStream &other) = delete;
  RDBChunkInputStream &operator=(RDBChunkInputStream &&other) noexcept {
    iter_ = std::move(other.iter_);
    return *this;
  }
  RDBChunkInputStream &operator=(RDBChunkInputStream &other) = delete;

  absl::StatusOr<std::unique_ptr<std::string>> LoadChunk() override {
    std::cerr << "Loading a chunk!" << std::endl;
    if (!iter_.HasNext()) {
      return absl::NotFoundError("No more elements remaining");
    }
    VMSDK_ASSIGN_OR_RETURN(auto result, iter_.Next());
    std::cerr << "Got chunk " << result->DebugString() << std::endl;
    return std::unique_ptr<std::string>(result->release_binary_content());
  }

 private:
  SupplementalContentChunkIter iter_;
};

class RDBChunkOutputStream : public hnswlib::OutputStream {
 public:
  explicit RDBChunkOutputStream(SafeRDB *rdb) : rdb_(rdb) {}

  RDBChunkOutputStream(RDBChunkOutputStream &&other) noexcept
      : rdb_(other.rdb_), closed_(other.closed_) {
    other.closed_ = true;
  }
  RDBChunkOutputStream(RDBChunkOutputStream &other) = delete;
  RDBChunkOutputStream &operator=(RDBChunkOutputStream &&other) noexcept {
    rdb_ = other.rdb_;
    closed_ = other.closed_;
    other.closed_ = true;
    return *this;
  }
  RDBChunkOutputStream &operator=(RDBChunkOutputStream &other) = delete;

  ~RDBChunkOutputStream() override {
    if (!closed_ && !Close().ok()) {
      VMSDK_LOG(WARNING, nullptr)
          << "Failed to write final chunk on closing output stream";
    }
  }

  absl::Status SaveChunk(const char *data, size_t len) override {
    if (closed_) {
      return absl::InternalError("RDBChunkOutputStream is closed");
    }
    std::cerr << "Saving a chunk!" << std::endl;
    data_model::SupplementalContentChunk chunk;
    chunk.set_binary_content(std::string(data, len));
    std::string serialized_string;
    if (!chunk.SerializeToString(&serialized_string)) {
      return absl::InternalError("Failed to serialize chunk to string");
    }
    VMSDK_RETURN_IF_ERROR(rdb_->SaveStringBuffer(serialized_string.data(),
                                                 serialized_string.size()));
    return absl::OkStatus();
  }

  absl::Status Close() {
    if (closed_) {
      return absl::InternalError("RDBChunkOutputStream is already closed");
    }
    std::cerr << "Saving final chunk!" << std::endl;
    data_model::SupplementalContentChunk chunk;
    std::string serialized_string;
    if (!chunk.SerializeToString(&serialized_string)) {
      return absl::InternalError("Failed to serialize chunk to string");
    }
    VMSDK_RETURN_IF_ERROR(rdb_->SaveStringBuffer(serialized_string.data(),
                                                 serialized_string.size()));
    closed_ = true;
    return absl::OkStatus();
  }

 private:
  SafeRDB *rdb_;
  bool closed_ = false;
};

// Callback to load a single RDBSection from the RDB. Return value is an error
// if the load fails. Supplemental content iterator must be fully iterated (and
// any chunks within it iterated as well) or loading will not succeed.
using RDBSectionLoadCallback = absl::AnyInvocable<absl::Status(
    RedisModuleCtx *ctx, std::unique_ptr<data_model::RDBSection> section,
    SupplementalContentIter supplemental_iter)>;

// Callback to save an arbitrary count of RDBSections to RDB on aux save events.
// Return value is an error status or the count of writted RDBSections.
using RDBSectionSaveCallback = absl::AnyInvocable<absl::Status(
    RedisModuleCtx *ctx, SafeRDB *rdb, int when)>;

using RDBSectionCountCallback =
    absl::AnyInvocable<int(RedisModuleCtx *ctx, int when)>;

using RDBSectionMinSemVerCallback =
    absl::AnyInvocable<int(RedisModuleCtx *ctx, int when)>;

using RDBSectionCallbacks = struct RDBSectionCallbacks {
  RDBSectionLoadCallback load;
  RDBSectionSaveCallback save;
  RDBSectionCountCallback section_count;
  RDBSectionMinSemVerCallback minimum_semantic_version;
};

static absl::flat_hash_map<data_model::RDBSectionType, RDBSectionCallbacks>
    kRegisteredRDBSectionCallbacks = {};

// Register for an RDB callback on RDB load and save, based on the RDBSection
// type. For load callbacks, a callback is made for each matching RDBSection in
// the loaded RDB. For save callbacks, a single callback is always given.
// Callbacks are made from the main thread, so thread safety should be
// guaranteed by the callback implementation.
static void RegisterRDBCallback(data_model::RDBSectionType type,
                                RDBSectionCallbacks callbacks) {
  vmsdk::VerifyMainThread();
  kRegisteredRDBSectionCallbacks[type] = std::move(callbacks);
}

static absl::Status PerformRDBLoad(RedisModuleCtx *ctx, SafeRDB *rdb,
                                   int encver) {
  vmsdk::VerifyMainThread();

  // Parse the header
  if (encver != kCurrentEncVer) {
    return absl::InternalError(absl::StrFormat(
        "Unable to load RDB with encoding version %d, we only support %d",
        encver, kCurrentEncVer));
  }
  unsigned int min_semantic_version;
  VMSDK_RETURN_IF_ERROR(rdb->LoadUnsigned(min_semantic_version))
      << "IO error reading semantic version from RDB. Failing RDB load.";
  if (min_semantic_version >= kCurrentSemanticVersion) {
    return absl::InternalError(absl::StrCat(
        "ValkeySearch RDB contents require minimum version ",
        HumanReadableSemanticVersion(min_semantic_version), " and we are on ",
        HumanReadableSemanticVersion(kCurrentSemanticVersion),
        ". If you are downgrading, ensure all feature usage on the new "
        "version of ValkeySearch is supported by this version and retry."));
  }

  unsigned int rdb_section_count;
  VMSDK_RETURN_IF_ERROR(rdb->LoadUnsigned(rdb_section_count))
      << "IO error reading RDB section count from RDB. Failing RDB load.";

  // Begin RDBSection iteration
  RDBSectionIter it(rdb, rdb_section_count);
  while (it.HasNext()) {
    VMSDK_ASSIGN_OR_RETURN(auto section, it.Next());

    if (kRegisteredRDBSectionCallbacks.contains(section->type())) {
      auto &load_callback =
          kRegisteredRDBSectionCallbacks.at(section->type()).load;
      VMSDK_RETURN_IF_ERROR(load_callback(ctx, std::move(section),
                                          it.IterateSupplementalContent()));
    } else {
      VMSDK_LOG(WARNING, ctx)
          << "Ignoring unknown RDB section with type "
          << data_model::RDBSectionType_Name(section->type());
    }
  }
  return absl::OkStatus();
}

static int AuxLoadCallback(RedisModuleIO *rdb, int encver, int when) {
  auto ctx = RedisModule_GetContextFromIO(rdb);
  SafeRDB safe_rdb(rdb);
  auto result = PerformRDBLoad(ctx, &safe_rdb, encver);
  if (result.ok()) {
    Metrics::GetStats().rdb_load_success_cnt++;

    return REDISMODULE_OK;
  }
  Metrics::GetStats().rdb_load_failure_cnt++;
  VMSDK_LOG_EVERY_N_SEC(WARNING, ctx, 0.1)
      << "Failed to load ValkeySearch aux section from RDB: "
      << result.message();
  return REDISMODULE_ERR;
}

static absl::Status PerformRDBSave(RedisModuleCtx *ctx, SafeRDB *rdb,
                                   int when) {
  vmsdk::VerifyMainThread();

  // Aggregate header information from save callbacks first
  auto rdb_section_count = 0;
  auto min_semantic_version = 0x000000;  // 0.0.0
  for (auto &kRegisteredRDBSectionCallback : kRegisteredRDBSectionCallbacks) {
    rdb_section_count +=
        kRegisteredRDBSectionCallback.second.section_count(ctx, when);
    min_semantic_version =
        std::max(min_semantic_version,
                 kRegisteredRDBSectionCallback.second.minimum_semantic_version(
                     ctx, when));
  }

  // Do nothing to satisfy AuxSave2 if there are no RDBSections.
  if (rdb_section_count == 0) {
    return absl::OkStatus();
  }

  // Save the header
  VMSDK_RETURN_IF_ERROR(rdb->SaveUnsigned(kCurrentSemanticVersion));
  VMSDK_RETURN_IF_ERROR(rdb->SaveUnsigned(rdb_section_count));

  // Now do the save of the contents
  for (auto &kRegisteredRDBSectionCallback : kRegisteredRDBSectionCallbacks) {
    VMSDK_RETURN_IF_ERROR(
        kRegisteredRDBSectionCallback.second.save(ctx, rdb, when));
  }

  return absl::OkStatus();
}

static void AuxSaveCallback(RedisModuleIO *rdb, int when) {
  auto ctx = RedisModule_GetContextFromIO(rdb);
  SafeRDB safe_rdb(rdb);
  auto result = PerformRDBSave(ctx, &safe_rdb, when);
  if (result.ok()) {
    Metrics::GetStats().rdb_save_success_cnt++;
    return;
  }
  Metrics::GetStats().rdb_save_failure_cnt++;
  VMSDK_LOG_EVERY_N_SEC(WARNING, ctx, 0.1)
      << "Failed to save ValkeySearch aux section to RDB: " << result.message();
}

// This module type is used purely to get aux callbacks.
static absl::Status RegisterModuleType(RedisModuleCtx *ctx) {
  static RedisModuleTypeMethods tm = {
      .version = REDISMODULE_TYPE_METHOD_VERSION,
      .rdb_load = [](RedisModuleIO *io, int encver) -> void * {
        DCHECK(false) << "Attempt to load ValkeySearch module type from RDB";
        return nullptr;
      },
      .rdb_save =
          [](RedisModuleIO *io, void *value) {
            DCHECK(false) << "Attempt to save ValkeySearch module type to RDB";
          },
      .aof_rewrite =
          [](RedisModuleIO *aof, RedisModuleString *key, void *value) {
            DCHECK(false)
                << "Attempt to rewrite ValkeySearch module type to AOF";
          },
      .free =
          [](void *value) {
            DCHECK(false) << "Attempt to free ValkeySearch module type object";
          },
      .aux_load = AuxLoadCallback,
      .aux_save_triggers = REDISMODULE_AUX_AFTER_RDB,
      .aux_save2 = AuxSaveCallback,
  };

  static RedisModuleType *kValkeySearchModuleType = nullptr;
  kValkeySearchModuleType = RedisModule_CreateDataType(
      ctx, kValkeySearchModuleTypeName.data(), kCurrentEncVer, &tm);
  if (!kValkeySearchModuleType) {
    return absl::InternalError(absl::StrCat(
        "failed to create ", kValkeySearchModuleTypeName, " type"));
  }
  return absl::OkStatus();
}

}  // namespace valkey_search

#endif  // VALKEYSEARCH_SRC_RDB_SERIALIZATION_H_
