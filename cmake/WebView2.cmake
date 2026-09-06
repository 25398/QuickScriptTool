# WebView2.cmake — download Microsoft.Web.WebView2 NuGet and expose Qst::WebView2
# Usage: include(cmake/WebView2.cmake) then qst_link_webview2(<target>)

set(QST_WEBVIEW2_VERSION "1.0.2903.40" CACHE STRING "Microsoft.Web.WebView2 NuGet version")

function(qst_ensure_webview2)
    if(TARGET Qst::WebView2)
        return()
    endif()

    set(_pkg_dir "${CMAKE_BINARY_DIR}/_deps/Microsoft.Web.WebView2")
    set(_header "${_pkg_dir}/build/native/include/WebView2.h")
    if(NOT EXISTS "${_header}")
        set(_zip "${CMAKE_BINARY_DIR}/_deps/Microsoft.Web.WebView2.${QST_WEBVIEW2_VERSION}.nupkg")
        message(STATUS "Downloading WebView2 ${QST_WEBVIEW2_VERSION} from NuGet…")
        file(DOWNLOAD
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${QST_WEBVIEW2_VERSION}"
            "${_zip}"
            SHOW_PROGRESS
            STATUS _dl_status
            TLS_VERIFY ON
        )
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            list(GET _dl_status 1 _dl_msg)
            message(FATAL_ERROR "WebView2 download failed: ${_dl_msg}")
        endif()
        file(MAKE_DIRECTORY "${_pkg_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_pkg_dir}")
    endif()

    if(NOT EXISTS "${_header}")
        message(FATAL_ERROR "WebView2.h missing after extract: ${_header}")
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_arch "x64")
    else()
        set(_arch "x86")
    endif()
    set(_lib_dir "${_pkg_dir}/build/native/${_arch}")
    set(_static "${_lib_dir}/WebView2LoaderStatic.lib")
    set(_shared_lib "${_lib_dir}/WebView2Loader.dll.lib")
    set(_shared_dll "${_lib_dir}/WebView2Loader.dll")

    add_library(Qst_WebView2 INTERFACE)
    add_library(Qst::WebView2 ALIAS Qst_WebView2)
    target_include_directories(Qst_WebView2 INTERFACE
        "${_pkg_dir}/build/native/include"
    )
    if(EXISTS "${_static}")
        target_link_libraries(Qst_WebView2 INTERFACE "${_static}" version)
        set(QST_WEBVIEW2_NEED_LOADER_DLL FALSE CACHE INTERNAL "")
    elseif(EXISTS "${_shared_lib}")
        target_link_libraries(Qst_WebView2 INTERFACE "${_shared_lib}" version)
        set(QST_WEBVIEW2_NEED_LOADER_DLL TRUE CACHE INTERNAL "")
        set(QST_WEBVIEW2_LOADER_DLL "${_shared_dll}" CACHE INTERNAL "")
    else()
        message(FATAL_ERROR "WebView2Loader lib not found under ${_lib_dir}")
    endif()
endfunction()

function(qst_link_webview2 target)
    qst_ensure_webview2()
    target_link_libraries(${target} PRIVATE
        Qst::WebView2
        ole32
        oleaut32
        user32
        shell32
        shlwapi
    )
    if(QST_WEBVIEW2_NEED_LOADER_DLL AND EXISTS "${QST_WEBVIEW2_LOADER_DLL}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QST_WEBVIEW2_LOADER_DLL}"
                "$<TARGET_FILE_DIR:${target}>/WebView2Loader.dll"
            COMMENT "Copy WebView2Loader.dll"
            VERBATIM)
    endif()
endfunction()
