// T064 (I8, Principle III): the UIDVALIDITY guard. A stub transport answers STATUS; no network, no server.
#include <string>
#include <vector>

#include "pbo/uidvalidity_guard.h"
#include "test_support.h"

namespace {

// A transport that answers every command from a script and records what it was asked.
class StubImap : public pbo::ImapTransport {
public:
    std::string status_answer = "* STATUS \"INBOX\" (UIDVALIDITY 777)\r\n";
    pbo::Error status_error;
    std::vector<std::string> asked;

    pbo::Error connect(const pbo::ImapEndpoint&, const std::string&) override { return {}; }
    void disconnect() override {}
    pbo::Result<pbo::ImapRaw> command(const std::string& mailbox, const std::string& command) override {
        asked.push_back(mailbox + "|" + command);
        pbo::Result<pbo::ImapRaw> r;
        r.error = status_error;
        r.value.data = status_answer;
        return r;
    }
    pbo::Error fetch_to(const std::string&, uint32_t, const std::string&, uint64_t, pbo::ByteSink&) override { return {}; }
    pbo::Error append(const std::string&, const std::string&) override { return {}; }
    const std::set<std::string>& capabilities() const override { return caps_; }

private:
    std::set<std::string> caps_;
};

pbo::Folder folder(int64_t id, std::optional<int64_t> stored) {
    pbo::Folder f;
    f.id = id;
    f.name = f.raw_name = "INBOX";
    f.uidvalidity = stored;
    return f;
}

}  // namespace

PB_TEST(an_equal_value_is_ok_and_one_status_command_is_enough) {
    StubImap imap;
    uint32_t seen = 0;
    PB_CHECK(pbo::check_uidvalidity(imap, folder(1, 777), &seen) == pbo::GuardResult::Ok);
    PB_CHECK_EQ(seen, 777u);
    PB_CHECK_EQ(imap.asked.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(imap.asked[0], std::string("|STATUS \"INBOX\" (UIDVALIDITY)"));   // no mailbox selected, nothing but UIDVALIDITY asked
}

PB_TEST(a_changed_value_is_a_mismatch_and_the_new_value_is_reported) {
    StubImap imap;
    uint32_t seen = 0;
    PB_CHECK(pbo::check_uidvalidity(imap, folder(1, 555), &seen) == pbo::GuardResult::Mismatch);
    PB_CHECK_EQ(seen, 777u);
}

PB_TEST(a_folder_without_a_stored_value_is_ok_the_first_sync) {
    StubImap imap;
    uint32_t seen = 0;
    PB_CHECK(pbo::check_uidvalidity(imap, folder(1, std::nullopt), &seen) == pbo::GuardResult::Ok);
    PB_CHECK_EQ(seen, 777u);
}

PB_TEST(a_failing_or_unreadable_answer_fails_closed) {
    StubImap failing;
    failing.status_error = pbo::make_error(pbo::ErrorClass::Network, "timeout");
    PB_CHECK(pbo::check_uidvalidity(failing, folder(1, 777)) == pbo::GuardResult::Failed);

    for (const char* bad : {"", "* OK nothing useful\r\n", "* STATUS \"INBOX\" (MESSAGES 5)\r\n", "* STATUS \"INBOX\" (UIDVALIDITY 99999999999)\r\n",
                            "* STATUS \"INBOX\" (UIDVALIDITY x)\r\n", "* STATUS \"INBOX\" (UIDVALIDITY 0)\r\n"}) {
        StubImap imap;
        imap.status_answer = bad;
        if (pbo::check_uidvalidity(imap, folder(1, 777)) != pbo::GuardResult::Failed) PB_FAIL_AT(std::string("not failed closed for: ") + bad);
    }
    // not even a first sync (no stored value) accepts an unreadable answer
    StubImap imap;
    imap.status_answer = "garbage";
    PB_CHECK(pbo::check_uidvalidity(imap, folder(1, std::nullopt)) == pbo::GuardResult::Failed);
}

PB_TEST(a_folder_name_that_could_carry_a_second_command_is_not_sent) {
    StubImap imap;
    pbo::Folder f = folder(1, 777);
    f.raw_name = "INBOX\r\nA2 DELETE INBOX";
    PB_CHECK(pbo::check_uidvalidity(imap, f) == pbo::GuardResult::Failed);
    PB_CHECK(imap.asked.empty());
    f.raw_name = "Say \"hi\" \\ there";
    imap.status_answer = "* STATUS \"x\" (UIDVALIDITY 777)\r\n";
    PB_CHECK(pbo::check_uidvalidity(imap, f) == pbo::GuardResult::Ok);
    PB_CHECK_EQ(imap.asked[0], std::string("|STATUS \"Say \\\"hi\\\" \\\\ there\" (UIDVALIDITY)"));   // quoted, escaped
}

PB_TEST(the_guard_object_allows_writes_only_after_an_ok_check_of_that_folder) {
    StubImap imap;
    pbo::UidvalidityGuard guard;
    PB_CHECK(!guard.allows_writes(1));                                   // never checked
    PB_CHECK(guard.result_for(1) == pbo::GuardResult::Failed);
    PB_CHECK(guard.check(imap, folder(1, 777)) == pbo::GuardResult::Ok);
    PB_CHECK(guard.allows_writes(1));
    PB_CHECK(!guard.allows_writes(2));                                   // valid for one folder only
    PB_CHECK_EQ(*guard.server_value(1), 777u);

    imap.status_answer = "* STATUS \"INBOX\" (UIDVALIDITY 778)\r\n";     // the folder is reset in the middle of the sync
    PB_CHECK(guard.check(imap, folder(1, 777)) == pbo::GuardResult::Mismatch);   // asked again, not remembered from before
    PB_CHECK(!guard.allows_writes(1));
    PB_CHECK_EQ(imap.asked.size(), static_cast<size_t>(2));

    imap.status_error = pbo::make_error(pbo::ErrorClass::Network, "connection_lost");
    PB_CHECK(guard.check(imap, folder(1, 778)) == pbo::GuardResult::Failed);
    PB_CHECK(!guard.allows_writes(1));
    PB_CHECK(!guard.server_value(1).has_value());
}

PB_TEST(a_status_value_that_the_caller_already_has_is_judged_the_same_way) {
    pbo::UidvalidityGuard guard;
    PB_CHECK(guard.accept(folder(1, 5), 5) == pbo::GuardResult::Ok);
    PB_CHECK(guard.allows_writes(1));
    PB_CHECK(guard.accept(folder(1, 5), 6) == pbo::GuardResult::Mismatch);
    PB_CHECK(!guard.allows_writes(1));
    PB_CHECK_EQ(*guard.server_value(1), 6u);
    PB_CHECK(guard.accept(folder(2, std::nullopt), 9) == pbo::GuardResult::Ok);
    PB_CHECK(guard.accept(folder(3, 5), 0) == pbo::GuardResult::Failed);            // unreadable
    PB_CHECK(!guard.allows_writes(3));
}

PB_TEST_MAIN()
