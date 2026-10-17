// T034: the `credentials` file (research R-13): atomic write, 0600 from the first byte, honest protection status,
// round trip of awkward passwords, overwrite-then-unlink on forget, and no password anywhere else.
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

#include "pbo/credentials_file.h"
#include "pbo/log.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

mode_t mode_of(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0;
}

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
    }
    return out;
}

pbo::Credentials sample() {
    pbo::Credentials c;
    c.mail_password = "mail-pw-1";
    c.smtp_password = "smtp pw = with=equals and spaces ";
    c.calendar_password = "cal\\pw%\xC3\xA4\xE2\x82\xAC";
    return c;
}

}  // namespace

PB_TEST(save_and_load_round_trip_awkward_passwords) {
    pbtest::TmpDir dir;
    const auto in = sample();
    PB_CHECK(pbo::save_credentials(dir.path(), in).ok());
    auto out = pbo::load_credentials(dir.path());
    PB_CHECK(out.ok());
    PB_CHECK_EQ(out.value.mail_password, in.mail_password);
    PB_CHECK_EQ(out.value.smtp_password, in.smtp_password);
    PB_CHECK_EQ(out.value.calendar_password, in.calendar_password);
}

PB_TEST(passwords_with_line_breaks_round_trip_too) {
    pbtest::TmpDir dir;
    pbo::Credentials c;
    c.mail_password = "line1\nline2\r\nend";
    PB_CHECK(pbo::save_credentials(dir.path(), c).ok());
    PB_CHECK_EQ(pbo::load_credentials(dir.path()).value.mail_password, c.mail_password);
    PB_CHECK_EQ(entries(dir.path()).size(), static_cast<size_t>(1));   // the line breaks did not make extra files
}

PB_TEST(a_missing_file_loads_as_empty_passwords) {
    pbtest::TmpDir dir;
    auto r = pbo::load_credentials(dir.path());
    PB_CHECK(r.ok());
    PB_CHECK(r.value.mail_password.empty() && r.value.smtp_password.empty() && r.value.calendar_password.empty());
    PB_CHECK(pbo::credentials_protection(dir.path()) == pbo::ProtectionStatus::Unknown);
}

PB_TEST(the_file_is_private_whatever_the_umask_and_leaves_no_temp_file) {
    pbtest::TmpDir dir;
    const mode_t old = ::umask(0);   // a permissive umask must not widen the file (K6: 0600 from the start)
    pbo::ProtectionStatus status = pbo::ProtectionStatus::Unknown;
    const auto err = pbo::save_credentials(dir.path(), sample(), &status);
    ::umask(old);
    PB_CHECK(err.ok());
    PB_CHECK_EQ(mode_of(pbo::credentials_path(dir.path())), static_cast<mode_t>(0600));
    PB_CHECK(status == pbo::ProtectionStatus::Enforced);   // /tmp honors chmod
    PB_CHECK(pbo::credentials_protection(dir.path()) == pbo::ProtectionStatus::Enforced);
    const auto names = entries(dir.path());
    PB_CHECK_EQ(names.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(names[0], std::string("credentials"));
}

PB_TEST(a_group_readable_file_is_reported_as_not_enforced) {
    pbtest::TmpDir dir;
    PB_CHECK(pbo::save_credentials(dir.path(), sample()).ok());
    PB_CHECK(::chmod(pbo::credentials_path(dir.path()).c_str(), 0644) == 0);
    PB_CHECK(pbo::credentials_protection(dir.path()) == pbo::ProtectionStatus::NotEnforced);   // the FAT case
}

PB_TEST(a_missing_data_directory_is_created_private) {
    pbtest::TmpDir dir;
    const std::string sub = dir.file("data");
    PB_CHECK(pbo::save_credentials(sub, sample()).ok());
    PB_CHECK_EQ(mode_of(sub), static_cast<mode_t>(0700));
}

PB_TEST(saving_again_replaces_the_content_atomically) {
    pbtest::TmpDir dir;
    PB_CHECK(pbo::save_credentials(dir.path(), sample()).ok());
    pbo::Credentials c;
    c.mail_password = "second";
    PB_CHECK(pbo::save_credentials(dir.path(), c).ok());
    auto r = pbo::load_credentials(dir.path());
    PB_CHECK_EQ(r.value.mail_password, std::string("second"));
    PB_CHECK(r.value.smtp_password.empty());
    PB_CHECK_EQ(entries(dir.path()).size(), static_cast<size_t>(1));
}

PB_TEST(forget_overwrites_then_unlinks) {
    pbtest::TmpDir dir;
    PB_CHECK(pbo::save_credentials(dir.path(), sample()).ok());
    const std::string alias = dir.file("alias");   // a second name for the same inode shows what is left on disk
    PB_CHECK(::link(pbo::credentials_path(dir.path()).c_str(), alias.c_str()) == 0);
    PB_CHECK(pbo::forget_credentials(dir.path()).ok());
    PB_CHECK(!dir.exists("credentials"));
    const std::string left = dir.read("alias");
    PB_CHECK(left.find("mail-pw-1") == std::string::npos);
    PB_CHECK(left.find("smtp pw") == std::string::npos);
    PB_CHECK(pbo::forget_credentials(dir.path()).ok());   // missing file: success
}

PB_TEST(a_damaged_or_newer_file_is_reported_not_guessed) {
    pbtest::TmpDir dir;
    dir.write("credentials", "garbage without a version line\nmail_password=x\n");
    PB_CHECK_EQ(pbo::load_credentials(dir.path()).error.key, std::string("credentials_unreadable"));
    dir.write("credentials", "pbo-credentials 99\nmail_password=x\n");
    PB_CHECK_EQ(pbo::load_credentials(dir.path()).error.key, std::string("credentials_unreadable"));
}

PB_TEST(passwords_never_reach_the_log) {
    pbtest::TmpDir dir;
    std::string logged;
    pbo::log::set_sink([&](const std::string& line) { logged += line + "\n"; });
    const auto c = sample();
    pbo::save_credentials(dir.path(), c);
    pbo::load_credentials(dir.path());
    pbo::forget_credentials(dir.path());
    dir.write("credentials", "broken");
    pbo::load_credentials(dir.path());
    pbo::log::reset_sink();
    for (const std::string& pw : {c.mail_password, c.smtp_password, c.calendar_password})
        PB_CHECK(logged.find(pw) == std::string::npos);
}

PB_TEST_MAIN()
