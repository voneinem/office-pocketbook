# The Qt Quick application. ARM: SDK Qt via absolute library paths, manual moc/rcc. HOST: host Qt6
# when the Qml/Quick dev packages are installed (otherwise only the core library and tests are built).

file(GLOB PBO_QML_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/qml/*.qml ${CMAKE_SOURCE_DIR}/qml/pages/*.qml
    ${CMAKE_SOURCE_DIR}/qml/qmldir ${CMAKE_SOURCE_DIR}/qml/icons/*.svg)
set(PBO_APP_SOURCES main.cpp device_bridge.cpp handoff_listener.cpp)

# The device probe has no Qt dependency and builds on every target.
add_subdirectory(probe)

if(TARGET_TYPE STREQUAL "ARM")
    pbo_moc(PBO_MOC_SOURCES device_bridge.h)
    pbo_rcc(PBO_QML_RCC qml ${CMAKE_SOURCE_DIR}/qml/qml.qrc ${PBO_QML_FILES})

    add_executable(pboffice_app ${PBO_APP_SOURCES} ${PBO_MOC_SOURCES} ${PBO_QML_RCC})
    target_include_directories(pboffice_app SYSTEM PRIVATE ${PBO_QT_INCLUDES})
    target_include_directories(pboffice_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(pboffice_app PRIVATE PLATFORM_POCKETBOOK PBO_VERSION="${PBO_VERSION}")
    # libinkview lives outside the default sysroot library directories.
    target_link_directories(pboffice_app PRIVATE ${SYSROOT}/usr/local/lib)
    target_link_libraries(pboffice_app PRIVATE pbo_bridge ${PBO_QT_LIBS} pthread inkview)
    # Qt's own dependencies and libcurl's are found next to them at link time.
    target_link_options(pboffice_app PRIVATE -Wl,-rpath-link,${QT6_ROOT}/lib -Wl,-rpath-link,${SYSROOT}/usr/lib)
    # The ELF is installed as <app>.bin and a shell launcher is installed as <app>.app.
    set_target_properties(pboffice_app PROPERTIES SUFFIX ".app" OUTPUT_NAME "${PBO_APP_NAME}")
else()
    find_package(Qt6 QUIET COMPONENTS Core Gui Qml Quick)
    if(Qt6Quick_FOUND AND Qt6Qml_FOUND AND TARGET pbo_bridge)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        add_executable(pboffice_app ${PBO_APP_SOURCES} ${CMAKE_SOURCE_DIR}/qml/qml.qrc)
        set_property(SOURCE ${CMAKE_SOURCE_DIR}/qml/qml.qrc PROPERTY OBJECT_DEPENDS ${PBO_QML_FILES})
        target_include_directories(pboffice_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(pboffice_app PRIVATE
            PBO_VERSION="${PBO_VERSION}"
            PBO_BUNDLED_CA="${CMAKE_SOURCE_DIR}/third_party/cacert/cacert.pem"
            PBO_ZONEINFO_DIR="${CMAKE_SOURCE_DIR}/third_party/tzdata/zoneinfo"
            PBO_LICENSES_ROOT="${CMAKE_SOURCE_DIR}"
            PBO_EMOJI_FONT="${CMAKE_SOURCE_DIR}/third_party/noto-emoji/NotoEmoji.ttf")
        target_link_libraries(pboffice_app PRIVATE pbo_bridge Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
        set_target_properties(pboffice_app PROPERTIES OUTPUT_NAME "${PBO_APP_NAME}")
        # The UI must load without QML errors (offscreen platform, no display needed).
        add_test(NAME app_self_test COMMAND pboffice_app --self-test)
        set_tests_properties(app_self_test PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "bridge" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        if(PBO_SANITIZE)
            set_property(TEST app_self_test APPEND PROPERTY ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")
        endif()
    else()
        message(STATUS "Qt6 Qml/Quick dev packages not found - building the core library and tests only")
    endif()
endif()
