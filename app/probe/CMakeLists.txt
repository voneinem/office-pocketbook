# pbo-probe: a console program (libcurl, office_core and the C library only, no Qt, no InkView) that prints what the
# reader's runtime can do (research R-02). It builds on the host (tests/launcher/test_probe.sh) and for the
# device with the SDK toolchain, where it is installed as pbo-probe.bin next to the app.
if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
endif()
add_executable(pbo-probe pbo_probe.cpp imap_selftest.cpp)
target_compile_features(pbo-probe PRIVATE cxx_std_17)
target_link_libraries(pbo-probe PRIVATE office_core CURL::libcurl)
if(TARGET_TYPE STREQUAL "ARM")
    set_target_properties(pbo-probe PROPERTIES SUFFIX ".bin")
    # pbo-handoff-probe: the InkView program of the hand-over experiments (INSTALL.md, "Hand-over probe"; spec 003): a test sender, and with the argument
    # `target` a stand-in app. Device only: no Qt, no network.
    add_executable(pbo-handoff-probe pbo_handoff_probe.cpp)
    target_compile_features(pbo-handoff-probe PRIVATE cxx_std_17)
    target_link_directories(pbo-handoff-probe PRIVATE ${SYSROOT}/usr/local/lib)
    target_link_libraries(pbo-handoff-probe PRIVATE pthread inkview)
    set_target_properties(pbo-handoff-probe PROPERTIES SUFFIX ".bin")
endif()
