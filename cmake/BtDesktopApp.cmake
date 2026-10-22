# The app of a drop-in chain (doc/app.md of the drop-in repositories): bt_desktop_app(<target> ...).
#
# "The template is the library": the window, the bridge, the trusted dialog and the interface kit are the sidechain
# template's own, taken from its repository at the commit the chain pins (app/kit.commit), never copied:
#   - its webui host files (src/webui/window_*.cpp, bridge.cpp, dialog.cpp, ...) compiled as they are, with the compat
#     headers of libs/desktop/compat standing for the parts of Bitcoin Core they use;
#   - its interface (src/webui/app, src/webui/locale) built with npm, with the drop-in pages shared by every drop-in
#     (core/app/ui, in src/chain/dropin) and the chain's own layer (its app/chain, over src/chain) put over it.
# The template repository: BT_KIT_DIR (default: ../sidechain-template next to the chain's repository); the commit must
# be in it (git fetch https://github.com/block-fabric/sidechain-template <commit> otherwise).
#
#   bt_desktop_app(thunder-app STEM thunder NAME Thunder VERSION 0.1.0
#                  CHAIN ${CMAKE_SOURCE_DIR}/app/chain MANIFEST ${CMAKE_SOURCE_DIR}/app/manifest.json
#                  KIT_COMMIT_FILE ${CMAKE_SOURCE_DIR}/app/kit.commit MAIN ${CMAKE_SOURCE_DIR}/chains/thunder/app/main.cpp)
# Also makes <target>-pack: the chain's pages as a pack for the unified wallet (build/pack/<stem>, pack.json with the
# manifest's methods).

set(BT_KIT_DIR "${CMAKE_SOURCE_DIR}/../sidechain-template" CACHE PATH "The sidechain template's git repository (the app's window, dialog and interface kit, at the pinned commit)")
option(BT_APP_TEST_HOOKS "Build the drop-in apps with the hooks of their end-to-end tests (never for a release)" OFF)
set(BT_DESKTOP_DIR "${CMAKE_CURRENT_LIST_DIR}/../libs/desktop")
set(BT_APP_UI_DIR "${CMAKE_CURRENT_LIST_DIR}/../app/ui")

function(bt_desktop_app target)
  cmake_parse_arguments(A "" "STEM;NAME;VERSION;CHAIN;MANIFEST;KIT_COMMIT_FILE;MAIN;NODE_TARGET" "" ${ARGN})
  foreach(v STEM NAME VERSION CHAIN MANIFEST KIT_COMMIT_FILE MAIN)
    if(NOT A_${v})
      message(FATAL_ERROR "bt_desktop_app(${target}): ${v} is needed")
    endif()
  endforeach()
  file(READ "${A_KIT_COMMIT_FILE}" commit)
  string(STRIP "${commit}" commit)
  if(NOT commit MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "${A_KIT_COMMIT_FILE}: a commit of the sidechain template, in hex")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${A_KIT_COMMIT_FILE}")
  find_package(Git REQUIRED)
  find_package(Threads REQUIRED)

  # ---- the template at the pinned commit (its webui files only), once per commit
  set(kit "${CMAKE_BINARY_DIR}/kit/${commit}")
  if(NOT EXISTS "${kit}/.extracted")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BT_KIT_DIR}" cat-file -e "${commit}^{commit}" RESULT_VARIABLE missing OUTPUT_QUIET ERROR_QUIET)
    if(missing)
      message(FATAL_ERROR "The sidechain template's commit ${commit} (${A_KIT_COMMIT_FILE}) is not in ${BT_KIT_DIR} (BT_KIT_DIR): "
                          "git -C ${BT_KIT_DIR} fetch https://github.com/block-fabric/sidechain-template ${commit}")
    endif()
    file(REMOVE_RECURSE "${kit}")
    file(MAKE_DIRECTORY "${kit}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BT_KIT_DIR}" archive --format=tar -o "${kit}/kit.tar" "${commit}" src/webui RESULT_VARIABLE failed)
    if(failed)
      message(FATAL_ERROR "git archive of the template at ${commit} failed")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf kit.tar WORKING_DIRECTORY "${kit}" RESULT_VARIABLE failed)
    file(REMOVE "${kit}/kit.tar")
    if(failed)
      message(FATAL_ERROR "cannot unpack the template at ${commit}")
    endif()
    file(TOUCH "${kit}/.extracted")
  endif()
  set(web "${kit}/src/webui")

  # ---- the chain of the app: chain.json -> bitcoin-build-config.h and webui/chainconfig.h, as the template's CMake makes them
  set(chain_json "${A_CHAIN}/chain.json")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${chain_json}" "${A_MANIFEST}")
  file(READ "${chain_json}" chain)
  string(JSON WEBUI_CHAIN_NAME GET "${chain}" name)
  string(JSON WEBUI_CHAIN_BIN GET "${chain}" bin)
  string(JSON WEBUI_CHAIN_URI_SCHEME GET "${chain}" uriScheme)
  string(JSON WEBUI_CHAIN_TICKER GET "${chain}" coin ticker)
  string(JSON WEBUI_CHAIN_COIN_NAME GET "${chain}" coin name)
  string(JSON WEBUI_CHAIN_ICON_HUE GET "${chain}" brand iconHue)
  string(JSON WEBUI_MAINCHAIN_NAME GET "${chain}" mainchain name)
  set(WEBUI_SIDECHAIN 1)
  if(NOT WEBUI_CHAIN_BIN STREQUAL A_STEM)
    message(FATAL_ERROR "${chain_json} names the program ${WEBUI_CHAIN_BIN}, the app is ${A_STEM}")
  endif()
  set(WEBUI_CHAIN_NETWORKS "")
  foreach(network main test signet regtest)
    string(JSON p2p GET "${chain}" networks ${network} p2p)
    string(JSON rpc GET "${chain}" networks ${network} rpc)
    string(JSON hrp GET "${chain}" networks ${network} hrp)
    string(JSON main_rpc GET "${chain}" mainchain networks ${network} rpc)
    string(APPEND WEBUI_CHAIN_NETWORKS "Network{\"${network}\", ${p2p}, ${rpc}, \"${hrp}\", ${main_rpc}},\n    ")
  endforeach()
  string(STRIP "${WEBUI_CHAIN_NETWORKS}" WEBUI_CHAIN_NETWORKS)
  set(WEBUI_CHAIN_LINKS "")
  foreach(link website source issues guide)
    string(JSON url ERROR_VARIABLE no_link GET "${chain}" links ${link})
    if(NOT no_link AND url MATCHES "^https://[A-Za-z0-9./_#?=%-]+$")
      string(APPEND WEBUI_CHAIN_LINKS "\"${url}\", ")
    endif()
  endforeach()
  string(REGEX MATCHALL "\"," commas "${WEBUI_CHAIN_LINKS}")
  list(LENGTH commas WEBUI_CHAIN_LINKS_COUNT)
  set(gen "${CMAKE_CURRENT_BINARY_DIR}/${target}-gen")
  configure_file("${web}/chainconfig.h.in" "${gen}/webui/chainconfig.h" @ONLY)
  # CLIENT_NAME names the folder of the app's settings: "Backbone-<Name>", never the template-based app's own.
  file(WRITE "${gen}/bitcoin-build-config.h.new"
       "// Made by core/cmake/BtDesktopApp.cmake for ${target}.\n#pragma once\n#define CLIENT_NAME \"Backbone-${A_NAME}\"\n"
       "#define CLIENT_BUGREPORT \"https://github.com/block-fabric/backbone-${A_STEM}/issues\"\n#define CLIENT_URL \"https://github.com/block-fabric/backbone-${A_STEM}\"\n"
       "#define CLIENT_VERSION_STRING \"${A_VERSION}\"\n")
  file(COPY_FILE "${gen}/bitcoin-build-config.h.new" "${gen}/bitcoin-build-config.h" ONLY_IF_DIFFERENT)

  # ---- the manifest: what the app's user may call at the node, and the node's host methods the pages call
  file(READ "${A_MANIFEST}" manifest)
  set(whitelist "")
  set(host_methods "")
  foreach(group protocol reads confirmed host app)
    string(JSON n ERROR_VARIABLE none LENGTH "${manifest}" ${group})
    if(none OR n EQUAL 0)
      continue()
    endif()
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
      string(JSON m GET "${manifest}" ${group} ${i})
      if(NOT m MATCHES "^[a-zA-Z0-9_.]+$")
        message(FATAL_ERROR "${A_MANIFEST}: ${group} has a bad name ${m}")
      endif()
      string(APPEND whitelist "\"${m}\", ")
      if(group STREQUAL "host")
        string(APPEND host_methods "\"${m}\", ")
      endif()
    endforeach()
  endforeach()
  string(JSON networks GET "${manifest}" networks)
  string(JSON n_net LENGTH "${manifest}" networks)
  set(nets "")
  math(EXPR last "${n_net} - 1")
  foreach(i RANGE ${last})
    string(JSON nm GET "${manifest}" networks ${i})
    string(APPEND nets "\"${nm}\", ")
  endforeach()
  string(JSON url_flag ERROR_VARIABLE no_flag GET "${manifest}" mainchainUrlFlag)
  if(no_flag)
    set(url_flag "--mainchain-grpc-url")
  endif()
  if(NOT url_flag MATCHES "^--[a-z-]+$")
    message(FATAL_ERROR "${A_MANIFEST}: mainchainUrlFlag is a long option")
  endif()
  string(JSON rpc_port GET "${manifest}" rpcPort)
  string(JSON p2p_port GET "${manifest}" p2pPort)
  file(WRITE "${gen}/app_manifest.h.new"
       "// Made from ${A_MANIFEST} by core/cmake/BtDesktopApp.cmake.\n#pragma once\n#include <string>\n#include <vector>\n"
       "namespace app_manifest {\ninline const std::vector<std::string> WHITELIST{${whitelist}};\ninline const std::vector<std::string> HOST{${host_methods}};\n"
       "inline const std::vector<std::string> NETWORKS{${nets}};\ninline constexpr unsigned RPC_PORT{${rpc_port}};\ninline constexpr unsigned P2P_PORT{${p2p_port}};\n"
       "inline constexpr const char* MAINCHAIN_URL_FLAG{\"${url_flag}\"};\ninline constexpr const char* NAME{\"${A_NAME}\"};\ninline constexpr const char* STEM{\"${A_STEM}\"};\ninline constexpr const char* VERSION{\"${A_VERSION}\"};\n}\n")
  file(COPY_FILE "${gen}/app_manifest.h.new" "${gen}/app_manifest.h" ONLY_IF_DIFFERENT)

  # ---- the interface: the kit, the drop-in pages, the chain's layer; built with npm, embedded
  find_program(NPM_EXECUTABLE npm REQUIRED)
  set(ui "${CMAKE_CURRENT_BINARY_DIR}/${target}-ui")
  set(dist "${CMAKE_CURRENT_BINARY_DIR}/${target}-dist")
  file(GLOB_RECURSE chain_files CONFIGURE_DEPENDS "${A_CHAIN}/*")
  file(GLOB_RECURSE dropin_files CONFIGURE_DEPENDS "${BT_APP_UI_DIR}/*")
  add_custom_command(
    OUTPUT "${dist}/index.html"
    COMMAND ${CMAKE_COMMAND} -DKIT=${kit} -DKIT_REPO=${BT_KIT_DIR} -DDROPIN=${BT_APP_UI_DIR} -DCHAIN=${A_CHAIN} -DUI=${ui} -DOUT=${dist} -DNPM=${NPM_EXECUTABLE}
            -DMANIFEST=${A_MANIFEST} -DCOMMIT=${commit} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/assemble_ui.cmake
    DEPENDS ${chain_files} ${dropin_files} "${A_MANIFEST}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/assemble_ui.cmake" "${kit}/.extracted"
    COMMENT "The interface of ${target}: the template's kit at ${commit}, the drop-in pages, ${A_NAME}'s own (npm)"
    VERBATIM)
  add_custom_command(
    OUTPUT "${gen}/assets_data.cpp"
    COMMAND ${CMAKE_COMMAND} -DDIST_DIR=${dist} -DOUTPUT=${gen}/assets_data.cpp -P ${web}/cmake/embed.cmake
    DEPENDS "${dist}/index.html" "${web}/cmake/embed.cmake"
    COMMENT "Embedding the interface of ${target}"
    VERBATIM)
  add_custom_target(${target}-pack
    COMMAND ${CMAKE_COMMAND} -DKIT=${kit} -DKIT_REPO=${BT_KIT_DIR} -DDROPIN=${BT_APP_UI_DIR} -DCHAIN=${A_CHAIN} -DUI=${ui} -DOUT=${CMAKE_BINARY_DIR}/pack/${A_STEM}
            -DNPM=${NPM_EXECUTABLE} -DMANIFEST=${A_MANIFEST} -DCOMMIT=${commit} -DPACK=ON -DREPO=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/assemble_ui.cmake
    COMMENT "The pack of ${A_NAME}'s pages for the unified wallet: ${CMAKE_BINARY_DIR}/pack/${A_STEM}"
    VERBATIM)

  # ---- the host: the template's files as they are, the compat headers before Core's names
  set(host_sources
    "${web}/assets.cpp" "${web}/bridge.cpp" "${web}/dialog.cpp" "${web}/guard.cpp" "${web}/events.cpp" "${web}/i18n.cpp"
    "${web}/icontint.cpp" "${web}/quitguard.cpp" "${web}/singleinstance.cpp" "${web}/windowframe.cpp" "${web}/appconfig.cpp")
  if(WIN32)
    find_path(WEBVIEW2_INCLUDE_DIR WebView2.h PATH_SUFFIXES webview2 REQUIRED)
    list(APPEND host_sources "${web}/window_win32.cpp")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
    list(APPEND host_sources "${web}/window_gtk.cpp" "${web}/tray_gtk.cpp")
  endif()
  add_executable(${target} WIN32
    "${A_MAIN}"
    "${BT_DESKTOP_DIR}/src/app.cpp"
    "${BT_DESKTOP_DIR}/src/child.cpp"
    "${BT_DESKTOP_DIR}/src/compat.cpp"
    ${host_sources}
    "${gen}/assets_data.cpp")
  target_include_directories(${target} BEFORE PRIVATE "${BT_DESKTOP_DIR}/compat" "${gen}" "${BT_DESKTOP_DIR}/include")
  target_include_directories(${target} PRIVATE "${kit}/src")
  target_include_directories(${target} SYSTEM PRIVATE "${web}/webview")
  target_compile_options(${target} PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_libraries(${target} PRIVATE bt_appui bt_core Threads::Threads)
  if(BT_APP_TEST_HOOKS)
    target_compile_definitions(${target} PRIVATE WEBUI_TEST_HOOKS)
  endif()
  if(A_NODE_TARGET)
    # Next to its node, where the app looks for it.
    add_dependencies(${target} ${A_NODE_TARGET})
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:${A_NODE_TARGET}>")
  endif()
  if(WIN32)
    target_include_directories(${target} SYSTEM PRIVATE "${WEBVIEW2_INCLUDE_DIR}")
    target_compile_definitions(${target} PRIVATE WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1 WEBVIEW_MSWEBVIEW2_EXPLICIT_LINK=1)
    target_link_libraries(${target} PRIVATE advapi32 dwmapi gdi32 ole32 shell32 shlwapi user32 uuid version)
  else()
    target_link_libraries(${target} PRIVATE PkgConfig::WEBKITGTK)
  endif()
endfunction()
