// T169 (About part, FR-004, FR-038): the About page also says which time zone data are bundled, how the password file is protected on this storage
// (measured, not assumed) and recommends app-specific passwords; and it lists every component of the manifest with its license text, offline.
#include <sys/stat.h>

#include <QCoreApplication>
#include <QTest>

#include "about_bridge.h"
#include "pbo/credentials_file.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_about_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

AboutBridge::Config config(const std::string& data_dir) {
    AboutBridge::Config c;
    c.licensesRoot = QStringLiteral(PBO_LICENSES_ROOT);
    c.version = QStringLiteral("0.2.0");
    c.dataDir = QString::fromStdString(data_dir);
    c.tzRelease = QStringLiteral("2026c");
    return c;
}

}  // namespace

PB_TEST(about_names_the_version_and_the_bundled_time_zone_release) {
    pbtest::TmpDir d;
    AboutBridge about(config(d.path()));
    PB_CHECK_EQ(about.version().toStdString(), std::string("0.2.0"));
    PB_CHECK_EQ(about.tzRelease().toStdString(), std::string("2026c"));
}

PB_TEST(the_password_file_state_is_measured_and_always_comes_with_the_advice) {
    pbtest::TmpDir d;
    AboutBridge about(config(d.path()));
    PB_CHECK_EQ(about.credentialState().toStdString(), std::string("unknown"));   // no file yet
    pbo::Credentials cr;
    cr.mail_password = "secret-for-tests";
    PB_CHECK(pbo::save_credentials(d.path(), cr).ok());
    ::chmod(pbo::credentials_path(d.path()).c_str(), 0600);
    PB_CHECK_EQ(about.credentialState().toStdString(), std::string("enforced"));
    ::chmod(pbo::credentials_path(d.path()).c_str(), 0644);
    PB_CHECK_EQ(about.credentialState().toStdString(), std::string("not_enforced"));
}

PB_TEST(every_component_of_the_manifest_has_a_license_page_offline) {
    pbtest::TmpDir d;
    AboutBridge about(config(d.path()));
    PB_CHECK(about.components().size() >= 5);
    for (const QVariant& v : about.components()) {
        const QVariantMap c = v.toMap();
        const QVariantMap page = about.componentDetail(c.value("id").toString());
        PB_CHECK(!page.isEmpty());
        PB_CHECK(page.value("available").toBool());
        PB_CHECK(!page.value("paragraphs").toStringList().isEmpty());
    }
    PB_CHECK(about.appLicense().value("available").toBool());
}

PB_TEST_MAIN()
