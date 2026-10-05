// Copyright 2026 Christopher Hinds, Stratum Labs
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

#include "kudalite/kudalite.h"

const char* clGetErrorString(clError_t err) {
  switch (err) {
    case clSuccess: return "no error";
    case clErrorInvalidValue: return "invalid argument";
    case clErrorMemoryAllocation: return "out of cluster memory";
    case clErrorInitializationError: return "could not connect to the KUDA-Lite controller";
    case clErrorInvalidDevicePointer: return "invalid device pointer or range";
    case clErrorInvalidResourceHandle: return "invalid stream or event handle";
    case clErrorKernelNotFound: return "kernel not registered on the workers";
    case clErrorLaunchFailure: return "kernel launch failed";
    case clErrorNotReady: return "work not yet complete";
    case clErrorNetwork: return "network error";
    case clErrorWorkerLost: return "a worker left the cluster";
    case clErrorProtocol: return "protocol error";
    case clErrorNoWorkers: return "no workers available";
    case clErrorUnknown: return "unknown error";
  }
  return "unrecognised error code";
}
