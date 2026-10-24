/**
 * ReXGlue runtime - AC6 Recompilation project
 * Copyright (c) 2026 Tom Clay. All rights reserved.
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_HAVE_AC6NET
#include "netplay_glue.h"
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

ppc_u32_result_t XMsgInProcessCall_entry(ppc_u32_t app, ppc_u32_t message, ppc_u32_t arg1,
                                         ppc_u32_t arg2) {
  auto result = REX_KERNEL_STATE()->app_manager()->DispatchMessageSync(app, message, arg1, arg2);
  if (result == X_ERROR_NOT_FOUND) {
    REXKRNL_ERROR("XMsgInProcessCall: app {:08X} undefined", app);
  }
  return result;
}

ppc_u32_result_t XMsgSystemProcessCall_entry(ppc_u32_t app, ppc_u32_t message, ppc_u32_t buffer,
                                             ppc_u32_t buffer_length) {
  auto result =
      REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer, buffer_length);
  if (result == X_ERROR_NOT_FOUND) {
    REXKRNL_ERROR("XMsgSystemProcessCall: app {:08X} undefined", app);
  }
  return result;
}

struct XMSGSTARTIOREQUEST_UNKNOWNARG {
  be<uint32_t> unk_0;
  be<uint32_t> unk_1;
};

X_HRESULT xeXMsgStartIORequestEx(uint32_t app, uint32_t message, uint32_t overlapped_ptr,
                                 uint32_t buffer_ptr, uint32_t buffer_length,
                                 XMSGSTARTIOREQUEST_UNKNOWNARG* unknown) {
#if REX_HAVE_AC6NET
  // Netplay: XGI and XLiveBase requests with an overlapped run on the glue workers (their
  // handlers may block on lobby RPCs) and complete later. Without an overlapped they stay
  // synchronous.
  if (overlapped_ptr && netplay::ShouldDispatchAsync(app, message)) {
    const X_RESULT async_result = netplay::DispatchMessageAsync(
        REX_KERNEL_STATE(), app, message, buffer_ptr, buffer_length, overlapped_ptr);
    if (async_result == X_ERROR_SUCCESS || async_result == X_ERROR_IO_PENDING) {
      XThread::SetLastError(0);
    }
    return async_result;
  }
#endif
  auto result = REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer_ptr,
                                                                        buffer_length);
  if (result == X_E_NOTFOUND) {
    REXKRNL_ERROR("XMsgStartIORequestEx: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    XThread::SetLastError(X_ERROR_NOT_FOUND);
  }
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, result);
    result = X_ERROR_IO_PENDING;
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    XThread::SetLastError(0);
  }
  return result;
}

ppc_u32_result_t XMsgStartIORequestEx_entry(ppc_u32_t app, ppc_u32_t message,
                                            ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                                            ppc_u32_t buffer_ptr, ppc_u32_t buffer_length,
                                            ppc_ptr_t<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                buffer_length, unknown_ptr);
}

ppc_u32_result_t XMsgStartIORequest_entry(ppc_u32_t app, ppc_u32_t message,
                                          ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                                          ppc_u32_t buffer_ptr, ppc_u32_t buffer_length) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                buffer_length, nullptr);
}

ppc_u32_result_t XMsgCancelIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                                           ppc_u32_t wait) {
#if REX_HAVE_AC6NET
  // Netplay requests cannot be aborted mid-RPC; with `wait`, block until the worker has
  // completed the overlapped so the guest never reuses it under a pending write.
  if (wait && netplay::Enabled() && netplay::IsAsyncPending(overlapped_ptr.guest_address())) {
    if (!netplay::WaitAsyncRequest(overlapped_ptr.guest_address(), 15000)) {
      REXKRNL_WARN("XMsgCancelIORequest: request on {:08X} still running after 15 s",
                   overlapped_ptr.guest_address());
    }
    return 0;
  }
#endif
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Wait(0, 0, true, nullptr);
    }
  }

  return 0;
}

ppc_u32_result_t XMsgCompleteIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                                             ppc_u32_t result, ppc_u32_t extended_error,
                                             ppc_u32_t length) {
  REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result,
                                                    extended_error, length);
  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XamGetOverlappedResult_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                                              ppc_pu32_t length_ptr, ppc_u32_t unknown) {
  uint32_t result;
  if (overlapped_ptr->result != X_ERROR_IO_PENDING) {
    result = overlapped_ptr->result;
  } else if (!overlapped_ptr->event) {
    result = X_ERROR_IO_INCOMPLETE;
  } else {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped_ptr->event);
    result = ev->Wait(3, 1, 0, nullptr);
    if (XSUCCEEDED(result)) {
      result = overlapped_ptr->result;
    } else {
      result = xboxkrnl::xeRtlNtStatusToDosError(result);
    }
  }
  if (XSUCCEEDED(result) && length_ptr) {
    *length_ptr = overlapped_ptr->length;
  }
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XMsgInProcessCall, rex::kernel::xam::XMsgInProcessCall_entry)
XAM_EXPORT(__imp__XMsgSystemProcessCall, rex::kernel::xam::XMsgSystemProcessCall_entry)
XAM_EXPORT(__imp__XMsgStartIORequestEx, rex::kernel::xam::XMsgStartIORequestEx_entry)
XAM_EXPORT(__imp__XMsgStartIORequest, rex::kernel::xam::XMsgStartIORequest_entry)
XAM_EXPORT(__imp__XMsgCancelIORequest, rex::kernel::xam::XMsgCancelIORequest_entry)
XAM_EXPORT(__imp__XMsgCompleteIORequest, rex::kernel::xam::XMsgCompleteIORequest_entry)
XAM_EXPORT(__imp__XamGetOverlappedResult, rex::kernel::xam::XamGetOverlappedResult_entry)

XAM_EXPORT_STUB(__imp__XMsgAcquireAsyncMessageFromOverlapped);
XAM_EXPORT_STUB(__imp__XMsgProcessRequest);
XAM_EXPORT_STUB(__imp__XMsgReleaseAsyncMessageToOverlapped);
