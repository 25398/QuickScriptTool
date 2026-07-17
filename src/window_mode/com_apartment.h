#pragma once

namespace windowmode {

/// Keep a thread COM apartment for the process lifetime of this thread.
/// Must not be paired with CoUninitialize: VirtualDesktopAccessor caches COM
/// objects; tearing down the apartment later causes ACCESS_VIOLATION on VDA calls.
void EnsureThreadComApartment();

}  // namespace windowmode
