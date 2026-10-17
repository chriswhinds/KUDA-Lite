// Copyright 2026 Christopher Hinds, Stratum Technologies LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Hooks between the host runtime and the unified memory view (src/host/unified.cpp).
#pragma once

#include <cstddef>

namespace kudalite::detail {

/// Called by clMemcpyDtoHAsync before it sends the request. If `dst` lies in a unified memory
/// window, the chunks it covers are fetched now, on the caller's thread. Otherwise the runtime's
/// network thread would fault on them when the data arrives, and the fault handler needs that
/// same thread to fetch them: a deadlock. No-op when the feature is not built in.
void unifiedPrepareHostWrite(void* dst, size_t bytes);

}  // namespace kudalite::detail
