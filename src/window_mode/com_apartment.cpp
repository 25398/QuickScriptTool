#include "com_apartment.h"

#include <objbase.h>

namespace windowmode {

void EnsureThreadComApartment() {
    // thread_local: one CoInitializeEx per thread; never CoUninitialize.
    thread_local const bool kReady = []() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // S_OK / S_FALSE = apartment ready; RPC_E_CHANGED_MODE = already MTA/other — still usable.
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    (void)kReady;
}

}  // namespace windowmode
