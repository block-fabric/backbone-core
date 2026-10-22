# libbt_appui: the app layer of a drop-in node (doc/app.md of the drop-in repositories): the app RPC (a second,
# authenticated JSON-RPC server beside the unchanged LayerTwo one), the host of the chain's pages over it (the
# sidechain template's remote-host protocol), and the drop-in phrases of the unified backup phrase (BIP-85).
# No GUI: the daemons link it. Needs bt_net (BIP-32 secp256k1 for BIP-85).
add_library(bt_appui STATIC src/auth.cpp src/rpcserver.cpp src/host.cpp src/phrase.cpp)
target_include_directories(bt_appui PUBLIC include)
target_link_libraries(bt_appui PUBLIC bt_core bt_net)
if(WIN32)
  target_link_libraries(bt_appui PUBLIC ws2_32)
endif()
