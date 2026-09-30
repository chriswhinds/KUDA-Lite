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
