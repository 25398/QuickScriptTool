# Copy WebView2 Fixed Runtime next to the product exe.
# Never delete the destination first: a running shell can lock files, and
# cmake -E remove_directory then leaves a half-empty WebView2Fixed that
# ResolveFixedBrowserFolder rejects ("安装包不完整").
if(NOT DEFINED QST_WV2_SRC OR NOT DEFINED QST_WV2_DST)
    message(FATAL_ERROR "CopyWebView2Fixed.cmake needs QST_WV2_SRC and QST_WV2_DST")
endif()
if(NOT EXISTS "${QST_WV2_SRC}/msedgewebview2.exe")
    message(FATAL_ERROR "WebView2Fixed source missing: ${QST_WV2_SRC}/msedgewebview2.exe")
endif()
if(EXISTS "${QST_WV2_DST}/msedgewebview2.exe")
    message(STATUS "WebView2Fixed already present — skip copy")
else()
    message(STATUS "Copying WebView2Fixed from ${QST_WV2_SRC}")
    file(MAKE_DIRECTORY "${QST_WV2_DST}")
    file(COPY "${QST_WV2_SRC}/" DESTINATION "${QST_WV2_DST}")
    if(NOT EXISTS "${QST_WV2_DST}/msedgewebview2.exe")
        message(FATAL_ERROR "WebView2Fixed copy failed: ${QST_WV2_DST}/msedgewebview2.exe missing")
    endif()
endif()
file(MAKE_DIRECTORY "${QST_WV2_USERDATA}")
