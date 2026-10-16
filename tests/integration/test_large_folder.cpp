// T074 (part): a folder with 50,000 messages of which only a few fall in the window. The sync asks for the window only (UID SEARCH SINCE), fetches the
// headers of those, and does not walk the 50,000 UIDs one by one. UNVERIFIED against real servers (fake server).
#include <chrono>

#include "mail_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::MailEnv;

PB_TEST(a_folder_of_fifty_thousand_old_messages_syncs_only_its_window) {
    MailEnv e;
    for (int i = 0; i < 50000; ++i)
        e.imap->add_message("INBOX", "From: a@example.org\r\nSubject: old\r\nMessage-ID: <o" + std::to_string(i) + "@example.org>\r\nDate: Mon, 01 Jan 2024 10:00:00 +0000\r\n\r\nx\r\n", {},
                            e.clock.t - 600 * 86400);
    for (int i = 0; i < 20; ++i) e.add("INBOX", {}, 1, true);
    const auto start = std::chrono::steady_clock::now();
    const auto report = e.sync();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    PB_CHECK(report.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("20"));
    PB_CHECK_EQ(e.q("SELECT older_on_server FROM folder WHERE name = 'INBOX'"), std::string("1"));   // the rest is offered as "load older"
    PB_CHECK(seconds < 60.0);
    // the commands: the window search, no fetch per old message
    int fetches = 0;
    for (const std::string& c : e.imap->commands())
        if (c.find("FETCH") != std::string::npos) ++fetches;
    PB_CHECK(fetches < 200);
}

PB_TEST_MAIN()
