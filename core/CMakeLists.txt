# office_core: Qt-free core library (mail, calendar, storage, sync). It MUST NOT depend on Qt or InkView
# (constitution Principle II); it builds on the host without the SDK and, for the device, with the SDK toolchain.

# Vendored SQLite amalgamation (pinned, see third_party/sqlite/VERSION).
add_library(pbo_sqlite STATIC ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
target_include_directories(pbo_sqlite PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(pbo_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED
    SQLITE_ENABLE_FTS5)
target_compile_options(pbo_sqlite PRIVATE -w)
target_link_libraries(pbo_sqlite PUBLIC pthread m)

set(PBO_CORE_SOURCES
    src/version.cpp
    src/handoff.cpp
    src/handoff_inbox.cpp
    src/types.cpp
    src/curl_session.cpp
    src/store.cpp
    src/store_migrations.cpp
    src/store_contacts.cpp
    src/store_mail.cpp
    src/store_outbox.cpp
    src/store_util.cpp
    src/store_calendar.cpp
    src/store_events.cpp
    src/store_organize.cpp
    src/store_images.cpp
    src/image_service.cpp
    src/event_edit.cpp
    src/imap_parse.cpp
    src/imap_transport.cpp
    src/smtp_transport.cpp
    src/connection_test.cpp
    src/mail_headers.cpp
    src/mail_roles.cpp
    src/charset.cpp
    src/html_text.cpp
    src/mime_parse.cpp
    src/mime_select.cpp
    src/bodystructure.cpp
    src/attachment_service.cpp
    src/uidvalidity_guard.cpp
    src/mime_headers.cpp
    src/xml_min.cpp
    src/draft_mirror.cpp
    src/mail_push.cpp
    src/outbox.cpp
    src/provider_presets.cpp
    src/compose.cpp
    src/caldav.cpp
    src/dav_common.cpp
    src/vcard.cpp
    src/carddav.cpp
    src/contacts_sync.cpp
    src/contact_edit.cpp
    src/calendar_sync.cpp
    src/ical.cpp
    src/rrule.cpp
    src/tz.cpp
    src/calendar_account_input.cpp
    src/credentials_file.cpp
    src/account_input.cpp
    src/mail_sync.cpp
    src/sync.cpp
    src/about.cpp
    src/http_transport.cpp
    src/utf8.cpp
    src/clock_check.cpp
    src/ca_selector.cpp
    src/log.cpp
    src/curl_transport.cpp
    src/paging.cpp
    src/sync_scheduler.cpp
)

# libcurl: the system package on the host (8.18.0 or newer), the bundled static build for the device.
if(TARGET_TYPE STREQUAL "ARM")
    include(${CMAKE_SOURCE_DIR}/cmake/curl_arm.cmake)
else()
    find_package(CURL REQUIRED)
endif()

add_library(office_core STATIC ${PBO_CORE_SOURCES})
target_include_directories(office_core PUBLIC include)
target_compile_features(office_core PUBLIC cxx_std_17)
target_compile_definitions(office_core PRIVATE PBO_VERSION="${PBO_VERSION}")
target_link_libraries(office_core PUBLIC pbo_sqlite CURL::libcurl)
