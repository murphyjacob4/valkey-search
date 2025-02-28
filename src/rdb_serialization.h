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

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
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

class SafeRDB {
 public:
  explicit SafeRDB(RedisModuleIO *rdb) : rdb_(rdb) {}

  absl::Status LoadSizeT(size_t &val) {
    val = RedisModule_LoadUnsigned(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadUnsigned failed");
    }
    return absl::OkStatus();
  }

  absl::Status LoadUnsigned(unsigned int &val) {
    val = RedisModule_LoadUnsigned(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadUnsigned failed");
    }
    return absl::OkStatus();
  }

  absl::Status LoadSigned(int &val) {
    val = RedisModule_LoadSigned(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadSigned failed");
    }
    return absl::OkStatus();
  }

  absl::Status LoadDouble(double &val) {
    val = RedisModule_LoadDouble(rdb_);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadDouble failed");
    }
    return absl::OkStatus();
  }

  absl::StatusOr<hnswlib::StringBufferUniquePtr> LoadStringBuffer(
      const size_t len) override {
    if (len == 0) {
      return absl::InvalidArgumentError("len must be > 0");
    }
    size_t len_read;
    auto str = hnswlib::StringBufferUniquePtr(
        RedisModule_LoadStringBuffer(rdb_, &len_read));
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadStringBuffer failed");
    }
    if (len_read != len) {
      return absl::InternalError(
          "Inconsistency between the expected and the actual string length");
    }
    return str;
  }

  virtual absl::StatusOr<vmsdk::UniqueRedisString> LoadString() {
    auto str = vmsdk::UniqueRedisString(RedisModule_LoadString(rdb_));
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_LoadString failed");
    }
    return str;
  }
  bool operator==(const SafeRDB &other) const { return rdb_ == other.rdb_; }

  absl::Status SaveSizeT(size_t val) {
    RedisModule_SaveUnsigned(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveUnsigned failed");
    }
    return absl::OkStatus();
  }

  absl::Status SaveUnsigned(unsigned int val) {
    RedisModule_SaveUnsigned(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveUnsigned failed");
    }
    return absl::OkStatus();
  }

  absl::Status SaveSigned(int val) {
    RedisModule_SaveSigned(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveSigned failed");
    }
    return absl::OkStatus();
  }

  absl::Status SaveDouble(double val) {
    RedisModule_SaveDouble(rdb_, val);
    if (RedisModule_IsIOError(rdb_)) {
      return absl::InternalError("RedisModule_SaveDouble failed");
    }
    return absl::OkStatus();
  }

  absl::Status SaveStringBuffer(const char *str, size_t len) {
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

class SupplementalContentChunkIter {
 public:
  static absl::StatusOr<std::unique_ptr<data_model::SupplementalContentChunk>>
  ReadSupplementalContentChunk(RDBInputStream rdb) {
    VMSDK_ASSIGN_OR_RETURN(auto serialized_chunk, rdb.LoadString(),
                           _ << "IO error while reading serialized "
                                "SupplementalContentChunk from RDB");
    auto result = std::make_unique<data_model::SupplementalContentChunk>();
    if (!result->ParseFromString(
            std::string(vmsdk::ToStringView(serialized_chunk.get())))) {
      return absl::InternalError(
          "Failed to deserialize SupplementalContentChunk read from RDB");
    }
    return result;
  }

  SupplementalContentChunkIter(RDBInputStream rdb) : rdb_(rdb) {
    current_ = ReadSupplementalContentChunk(rdb);
    CheckIfDone();
  }
  static SupplementalContentChunkIter End(RDBInputStream rdb) {
    return {rdb, true};
  }
  SupplementalContentChunkIter &operator++(int _) {
    current_ = ReadSupplementalContentChunk(rdb_);
    CheckIfDone();
    return *this;
  }
  bool operator==(const SupplementalContentChunkIter &other) const {
    return rdb_ == other.rdb_ && done_ == other.done_;
  }
  absl::StatusOr<data_model::SupplementalContentChunk> operator*() {
    if (!current_.ok()) {
      return current_.status();
    }
    return *(current_.value());
  }

 private:
  SupplementalContentChunkIter(RDBInputStream rdb, bool done)
      : rdb_(rdb), done_(done) {}
  void CheckIfDone() {
    // We are done only if there is not an error and there is an EOF marker (no
    // binary_content present). If there is an error, client needs to handle
    // this, since we will never know when it ends.
    done_ = current_.ok() && !current_.value()->has_binary_content();
  }
  RDBInputStream rdb_;
  absl::StatusOr<std::unique_ptr<data_model::SupplementalContentChunk>>
      current_;
  bool done_;
};

class SupplementalContentIter {
 public:
  static absl::StatusOr<std::unique_ptr<data_model::SupplementalContentHeader>>
  ReadSupplementalContentHeader(RDBInputStream rdb) {
    VMSDK_ASSIGN_OR_RETURN(auto serialized_supplemental_content_header,
                           rdb.LoadString(),
                           _ << "IO error while reading serialized "
                                "SupplementalContentHeader from RDB");
    auto result = std::make_unique<data_model::SupplementalContentHeader>();
    if (!result->ParseFromString(std::string(vmsdk::ToStringView(
            serialized_supplemental_content_header.get())))) {
      return absl::InternalError(
          "Failed to deserialize SupplementalContentHeader read from RDB");
    }
    return result;
  }
  SupplementalContentIter(RDBInputStream rdb, uint64_t count)
      : rdb_(rdb), remaining_(count) {
    if (remaining_ == 0) {
      current_ = absl::NotFoundError("No such element");
      return;
    }
    current_ = ReadSupplementalContentHeader(rdb);
    remaining_--;
  }
  SupplementalContentChunkIter ChunkBegin() { return {rdb_}; }
  SupplementalContentChunkIter ChunkEnd() {
    return SupplementalContentChunkIter::End(rdb_);
  }
  SupplementalContentIter &operator++(int _) {
    if (remaining_ == 0) {
      current_ = absl::NotFoundError("No such element");
    } else {
      current_ = ReadSupplementalContentHeader(rdb_);
      remaining_--;
    }
    return *this;
  }
  bool operator==(const SupplementalContentIter &other) const {
    return rdb_ == other.rdb_ && remaining_ == other.remaining_;
  }
  absl::StatusOr<data_model::SupplementalContentHeader> operator*() {
    if (!current_.ok()) {
      return current_.status();
    }
    return *(current_.value());
  }

 private:
  RDBInputStream rdb_;
  absl::StatusOr<std::unique_ptr<data_model::SupplementalContentHeader>>
      current_;
  // Count of remaining headers, excluding current_.
  uint64_t remaining_;
};

class RDBSectionIter {
 public:
  static absl::StatusOr<std::unique_ptr<data_model::RDBSection>> ReadRDBSection(
      RDBInputStream rdb) {
    VMSDK_ASSIGN_OR_RETURN(
        auto serialized_rdb_section, rdb.LoadString(),
        _ << "IO error while reading serialized RDBSection from RDB");
    auto result = std::make_unique<data_model::RDBSection>();
    if (!result->ParseFromString(
            std::string(vmsdk::ToStringView(serialized_rdb_section.get())))) {
      return absl::InternalError(
          "Failed to deserialize RDBSection read from RDB");
    }
    return result;
  }
  RDBSectionIter(RDBInputStream rdb, uint64_t remaining)
      : rdb_(rdb), remaining_(remaining) {
    if (remaining_ == 0) {
      current_ = absl::NotFoundError("No such element");
      return;
    }
    current_ = ReadRDBSection(rdb);
    remaining_--;
  }
  SupplementalContentIter SupplementalBegin() const {
    if (!current_.ok()) {
      return SupplementalEnd();
    }
    return {rdb_, current_.value()->supplemental_count()};
  }
  SupplementalContentIter SupplementalEnd() const { return {rdb_, 0}; }
  RDBSectionIter &operator++(int _) {
    if (remaining_ == 0) {
      current_ = absl::NotFoundError("No such element");
    } else {
      current_ = ReadRDBSection(rdb_);
      remaining_--;
    }
    return *this;
  }
  bool operator==(const RDBSectionIter &other) const {
    return rdb_ == other.rdb_ && remaining_ == other.remaining_;
  }
  absl::StatusOr<data_model::RDBSection> operator*() {
    if (!current_.ok()) {
      return current_.status();
    }
    return *(current_.value());
  }

 private:
  RDBInputStream rdb_;
  absl::StatusOr<std::unique_ptr<data_model::RDBSection>> current_;
  // Count of remaining sections, excluding current_.
  uint64_t remaining_;
};
class RDBDeserializer {
 public:
  static absl::StatusOr<RDBDeserializer> Create(RDBInputStream rdb,
                                                int encver) {
    if (encver != kCurrentEncVer) {
      return absl::InternalError(absl::StrFormat(
          "Unable to load RDB with encoding version %d, we only support %d",
          encver, kCurrentEncVer));
    }
    unsigned int min_semantic_version;
    VMSDK_RETURN_IF_ERROR(rdb.LoadUnsigned(min_semantic_version))
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
    VMSDK_RETURN_IF_ERROR(rdb.LoadUnsigned(rdb_section_count))
        << "IO error reading RDB section count from RDB. Failing RDB load.";
    return RDBDeserializer(rdb, rdb_section_count);
  }
  RDBSectionIter Begin() { return {rdb_, rdb_section_count_}; }
  RDBSectionIter End() { return {rdb_, 0}; }

 private:
  RDBDeserializer(RDBInputStream rdb, uint64_t rdb_section_count)
      : rdb_(rdb), rdb_section_count_(rdb_section_count) {}
  RDBInputStream rdb_;
  uint64_t rdb_section_count_;
};

class RDBSerializer {};

class RDBChunkInputStream : public hnswlib::InputStream {
 public:
  explicit RDBChunkInputStream(SupplementalContentChunkIter begin,
                               SupplementalContentChunkIter end)
      : current_(begin), end_(end) {}

  absl::StatusOr<std::unique_ptr<std::string>> LoadChunk() override {
    if (current_ == end_) {
      return absl::NotFoundError("No more elements remaining");
    }
    VMSDK_ASSIGN_OR_RETURN(auto result, *current_);
    current_++;
    return std::unique_ptr<std::string>(result->release_binary_content());
  }

 private:
  SupplementalContentChunkIter current_;
  SupplementalContentChunkIter end_;
};

class RDBChunkOutputStream : public hnswlib::OutputStream {
 public:
  explicit RDBChunkOutputStream(SafeRDB rdb) : rdb_(rdb) {}

  absl::Status SaveChunk(char *data, size_t len) override {
    data_model::SupplementalContentChunk chunk;
    chunk.set_binary_content(std::string(data, len));
    std::string serialized_string;
    if (!chunk.SerializeToString(&serialized_string)) {
      return absl::InternalError("Failed to serialize chunk to string");
    }
    VMSDK_RETURN_IF_ERROR(rdb_.SaveStringBuffer(serialized_string.data(),
                                                serialized_string.size()));
    return absl::OkStatus();
  }

 private:
  SafeRDB rdb_;
};

using RDBSectionLoadCallback =
    absl::Status (*)(RedisModuleCtx *ctx, data_model::RDBSection section,
                     SupplementalContentIter supplemental_begin,
                     SupplementalContentIter supplemental_end);

static const absl::flat_hash_map<data_model::RDBSectionType,
                                 RDBSectionLoadCallback> = {{
    data_model::RDBSectionType::RDB_SECTION_INDEX_SCHEMA,

}}

static absl::Status
PerformRDBLoad(RedisModuleIO * rdb, int encver) {
  RDBInputStream rdb_is(rdb);
  VMSDK_ASSIGN_OR_RETURN(auto rdb_deserializer,
                         RDBDeserializer::Create(rdb_is, encver));
  for (auto section_it = rdb_deserializer.Begin();
       section_it != rdb_deserializer.End(); section_it++) {
    VMSDK_ASSIGN_OR_RETURN(auto section, *section_it);

    for (auto supp_it = section_it.SupplementalBegin();
         supp_it != section_it.SupplementalEnd(); supp_it++) {
      VMSDK_ASSIGN_OR_RETURN(auto supplemental_header, *supp_it);
      for (auto chunk_it = supp_it.ChunkBegin(); chunk_it != supp_it.ChunkEnd();
           chunk_it++) {
        VMSDK_ASSIGN_OR_RETURN(auto chunk, *chunk_it);
        // TODO process
      }
    }
  }
}

static int AuxLoadCallback(RedisModuleIO *rdb, int encver, int when) {
  auto result = PerformRDBLoad(rdb, encver);
  if (result.ok()) {
    return REDISMODULE_OK;
  }
  VMSDK_LOG(WARNING, nullptr)
      << "Failed to load Valkey Search aux data from RDB: " << result.message();
  return REDISMODULE_ERR;
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

  static auto kValkeySearchModuleType = RedisModule_CreateDataType(
      ctx, kValkeySearchModuleTypeName.data(), kCurrentEncVer, &tm);
  if (!kValkeySearchModuleType) {
    return absl::InternalError(absl::StrCat(
        "failed to create ", kValkeySearchModuleTypeName, " type"));
  }
  return absl::OkStatus();
}

}  // namespace valkey_search

#endif  // VALKEYSEARCH_SRC_RDB_SERIALIZATION_H_