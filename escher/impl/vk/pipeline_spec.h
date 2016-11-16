// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <vector>

namespace escher {
namespace impl {

// Used as a key to obtain a Pipeline from a PipelineCache or a PipelineFactory.
class PipelineSpec {
 public:
  PipelineSpec(size_t type, std::vector<uint8_t> data);

  struct Hash {
    size_t operator()(const PipelineSpec& spec) const { return spec.hash_; }
  };

  size_t type() const { return type_; }
  const std::vector<uint8_t>& data() const { return data_; }
  size_t hash() const { return hash_; }

 private:
  size_t type_;
  std::vector<uint8_t> data_;
  size_t hash_;
};

// Inline function definitions.

inline bool operator==(const PipelineSpec& spec1, const PipelineSpec& spec2) {
  return spec1.hash() == spec2.hash() && spec1.type() == spec2.type() &&
         spec1.data() == spec2.data();
}

inline bool operator!=(const PipelineSpec& spec1, const PipelineSpec& spec2) {
  return !(spec1 == spec2);
}

}  // namespace impl
}  // namespace escher
