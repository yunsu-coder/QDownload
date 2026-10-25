cmake_minimum_required(VERSION 3.24)

project(QuickDL VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Quick
    QuickControls2
    QuickDialogs2
    Network
    Sql
    Svg
)
find_package(Qt6 6.8 QUIET COMPONENTS Test)
find_package(CURL REQUIRED)

qt_standard_project_setup()

qt_add_executable(QuickDL
    MACOSX_BUNDLE
    src/main.cpp
    src/DownloadController.cpp
    src/DownloadController.h
    src/CurlMultiWorker.cpp
    src/CurlMultiWorker.h
    src/CurlFtpWorker.cpp
    src/CurlFtpWorker.h
    src/EmbeddedProxy.cpp
    src/EmbeddedProxy.h
    src/SecureStore.cpp
    src/SecureStore.h
    src/TaskStore.cpp
    src/TaskStore.h
    src/FileHashWorker.cpp
    src/FileHashWorker.h
    src/ProtocolRouter.cpp
    src/ProtocolRouter.h
    src/HlsPlaylist.cpp
    src/HlsPlaylist.h
    src/Aes128.cpp
    src/Aes128.h
    src/HlsDownloader.cpp
    src/HlsDownloader.h
)

qt_add_qml_module(QuickDL
    URI QuickDL
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/AppButton.qml
        qml/SettingsDialog.qml
)

qt_add_resources(QuickDL branding
    PREFIX "/branding"
    BASE "${CMAKE_SOURCE_DIR}/assets/branding"
    FILES
        "${CMAKE_SOURCE_DIR}/assets/branding/kuaixiazai-mark.svg"
)

target_link_libraries(QuickDL PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::Network
    Qt6::Sql
    Qt6::Svg
    CURL::libcurl
)

if(APPLE)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    target_link_libraries(QuickDL PRIVATE ${SECURITY_FRAMEWORK})

    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/assets/branding/kuaixiazai.icns"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(
        QuickDL
        PRIVATE "${CMAKE_SOURCE_DIR}/assets/branding/kuaixiazai.icns"
    )

    add_custom_command(TARGET QuickDL POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:QuickDL>/Resources/bin"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/mihomo/mihomo-darwin-arm64"
            "$<TARGET_BUNDLE_CONTENT_DIR:QuickDL>/Resources/bin/mihomo"
        COMMAND /bin/chmod 755
            "$<TARGET_BUNDLE_CONTENT_DIR:QuickDL>/Resources/bin/mihomo"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:QuickDL>/Resources/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/mihomo/LICENSE"
            "$<TARGET_BUNDLE_CONTENT_DIR:QuickDL>/Resources/licenses/mihomo-LICENSE"
    )
endif()

if(WIN32)
    target_link_libraries(QuickDL PRIVATE Advapi32)
endif()

set_target_properties(QuickDL PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "快下载"
    MACOSX_BUNDLE_ICON_FILE "kuaixiazai.icns"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.quickdl.app"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)

if(TARGET Qt6::Test)
enable_testing()
qt_add_executable(quickdl_tests
    tests/test_hls.cpp
    src/Aes128.cpp
    src/Aes128.h
    src/HlsPlaylist.cpp
    src/HlsPlaylist.h
)
target_include_directories(quickdl_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(quickdl_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME quickdl_tests COMMAND quickdl_tests)
set_tests_properties(quickdl_tests PROPERTIES LABELS unit)

qt_add_executable(quickdl_router_tests
    tests/test_protocol_router.cpp
    src/ProtocolRouter.cpp
    src/ProtocolRouter.h
)
target_include_directories(quickdl_router_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(quickdl_router_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME quickdl_router_tests COMMAND quickdl_router_tests)
set_tests_properties(quickdl_router_tests PROPERTIES LABELS unit)

qt_add_executable(quickdl_store_tests
    tests/test_taskstore.cpp
    src/TaskStore.cpp
    src/TaskStore.h
)
target_include_directories(quickdl_store_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(quickdl_store_tests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME quickdl_store_tests COMMAND quickdl_store_tests)
set_tests_properties(quickdl_store_tests PROPERTIES LABELS unit)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME quickdl_integration_protocols
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/integration_protocols.py"
            "$<TARGET_FILE:QuickDL>"
    )
    set_tests_properties(
        quickdl_integration_protocols
        PROPERTIES TIMEOUT 90 LABELS integration
    )
endif()
endif()

include(GNUInstallDirs)
install(
    TARGETS QuickDL
    BUNDLE DESTINATION .
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES README.md THIRD_PARTY_NOTICES.md DESTINATION ".")

set(extra_deploy_args)
if(APPLE)
    list(APPEND extra_deploy_args EXCLUDE_PLUGIN_TYPES sqldrivers)
endif()
qt_generate_deploy_qml_app_script(
    TARGET QuickDL
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    ${extra_deploy_args}
)
install(SCRIPT "${deploy_script}")

if(APPLE)
    get_filename_component(
        quickdl_qt_prefix
        "${Qt6_DIR}/../../.."
        ABSOLUTE
    )
    install(
        FILES "${quickdl_qt_prefix}/plugins/sqldrivers/libqsqlite.dylib"
        DESTINATION "QuickDL.app/Contents/PlugIns/sqldrivers"
    )
endif()

set(CPACK_PACKAGE_NAME "QuickDL")
set(CPACK_PACKAGE_VENDOR "QuickDL")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "QuickDL Beta")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "QuickDL")
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "QuickDL ${PROJECT_VERSION}")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
