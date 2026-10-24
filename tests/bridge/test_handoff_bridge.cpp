// T012 (spec 003): HandoffBridge turns a hand-over (arguments of another app, or a pending entry written by the launcher) into an ordinary draft. The draft is saved
// before any screen is asked to show it; one draft per hand-over; nothing is merged, replaced or sent. The real core and a throw-away store.
#include <sys/stat.h>
#include <cstdlib>
#include <sys/time.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "handoff_bridge.h"
#include "pbo/handoff.h"
#include "pbo/log.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_handoff_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

constexpr int64_t kNow = 1791979200;
struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct Rng : pbo::Random {
    uint8_t n = 0;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

std::string fixture(const std::string& name) {
    std::ifstream f(std::string(PBO_FIXTURE_DIR) + "/handoff/" + name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// the arguments of a hand-over, as the system hands them to the app (with the marker)
std::vector<std::string> args_of(const std::string& text, const std::string& subject = "", const std::string& to = "") {
    std::vector<std::string> a = {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size())};
    if (!subject.empty()) a.push_back("subject=" + subject);
    if (!to.empty()) a.push_back("to=" + to);
    return a;
}

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    Rng rng;
    std::unique_ptr<pbo::Store> store;
    Env(bool with_account = true) {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        if (with_account) {
            pbo::MailAccount a;
            a.imap_host = "mail.example.org";
            a.smtp_host = "smtp.example.org";
            a.username = "alice";
            a.address = "alice@example.org";
            a.display_name = "Alice Example";
            store->save_mail_account(a);
        }
        ::mkdir(handoff_dir().c_str(), 0700);
    }
    std::string handoff_dir() const { return dir.file("handoff"); }
    HandoffBridge::Config config() {
        HandoffBridge::Config c;
        c.store = store.get();
        c.clock = &clock;
        c.random = &rng;
        c.handoff_dir = handoff_dir();
        return c;
    }
    // what the launcher writes: the arguments after the marker, one raw file each, in a folder named <id>
    void write_entry(const std::string& id, const std::vector<std::string>& args_with_marker) {
        const std::string folder = handoff_dir() + "/" + id;
        ::mkdir(folder.c_str(), 0700);
        for (size_t i = 1; i < args_with_marker.size(); ++i) dir.write("handoff/" + id + "/" + std::to_string(i - 1), args_with_marker[i]);
        struct timeval tv[2] = {{static_cast<time_t>(kNow - 1), 0}, {static_cast<time_t>(kNow - 1), 0}};
        ::utimes(folder.c_str(), tv);
    }
    bool entry_exists(const std::string& id) const { return dir.exists("handoff/" + id); }
};

void pump() { QCoreApplication::processEvents(); }

}  // namespace

PB_TEST(a_handoff_becomes_one_draft_whose_body_is_the_text_byte_for_byte) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    for (const std::string& text : {fixture("umlaut_euro_emoji.txt"), fixture("link_with_password.txt"), std::string(1500, 'x'), std::string("a\nb\n")}) {
        for (const auto& d : e.store->drafts()) e.store->delete_draft(d.id);
        shown.clear();
        b.deliver(args_of(text), "listener");
        pump();
        const auto drafts = e.store->drafts();
        PB_CHECK_EQ(drafts.size(), size_t(1));
        PB_CHECK_EQ(drafts[0].body, text);
        PB_CHECK_EQ(drafts[0].kind, std::string("new"));
        PB_CHECK_EQ(drafts[0].to_text, std::string(""));
        PB_CHECK_EQ(drafts[0].cc_text, std::string(""));
        PB_CHECK_EQ(drafts[0].bcc_text, std::string(""));
        PB_CHECK_EQ(drafts[0].subject, std::string(""));
        PB_CHECK(drafts[0].message_id.size() > 2 && drafts[0].message_id[0] == '<');
        PB_CHECK(drafts[0].message_id.find("@example.org>") != std::string::npos);
        PB_CHECK_EQ(shown.count(), 1);
        PB_CHECK_EQ(shown.at(0).at(0).toLongLong(), static_cast<qlonglong>(drafts[0].id));
        PB_CHECK_EQ(shown.at(0).at(1).toInt(), 0);
        b.shown();   // the page showed it
    }
}

PB_TEST(a_pending_entry_written_by_the_launcher_becomes_a_draft_and_is_removed_after_it) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    const std::string text = fixture("umlaut_euro_emoji.txt");
    e.write_entry("1761212401-977", args_of(text, "Hello", "bob@example.org"));
    b.checkInbox();
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(1));
    PB_CHECK_EQ(drafts[0].body, text);
    PB_CHECK_EQ(drafts[0].subject, std::string("Hello"));
    PB_CHECK_EQ(drafts[0].to_text, std::string("bob@example.org"));
    PB_CHECK(!e.entry_exists("1761212401-977"));   // removed after the draft was saved
    PB_CHECK_EQ(shown.count(), 1);
    b.checkInbox();                                // nothing left: no second draft
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    PB_CHECK_EQ(shown.count(), 1);
}

PB_TEST(the_draft_is_saved_before_the_screen_is_asked_to_show_it) {
    Env e;
    HandoffBridge b(e.config());
    std::string body_at_show;
    std::string subject_at_show;
    bool entry_present_at_show = true;
    QObject::connect(&b, &HandoffBridge::show, &b, [&](qlonglong id, int) {
        const auto d = e.store->draft(id);
        body_at_show = d ? d->body : "(no draft)";
        subject_at_show = d ? d->subject : "(no draft)";
        entry_present_at_show = e.entry_exists("1-1");
    });
    e.write_entry("1-1", args_of("the text", "the subject"));
    b.checkInbox();
    PB_CHECK_EQ(body_at_show, std::string("the text"));
    PB_CHECK_EQ(subject_at_show, std::string("the subject"));
    PB_CHECK(!entry_present_at_show);              // the entry goes after the draft and before the screen
}

PB_TEST(subject_and_recipient_are_prefilled_and_a_subject_alone_makes_a_draft_with_an_empty_body) {
    Env e;
    HandoffBridge b(e.config());
    b.deliver(args_of("Text", "Plan", "bob@example.org"), "listener");
    pump();
    b.deliver({"pbo-mail-v1", "subject=Only a subject"}, "listener");
    pump();
    b.deliver({"pbo-mail-v1", "to=carol@example.org"}, "listener");
    pump();
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(3));
    int seen = 0;
    for (const auto& d : drafts) {
        if (d.subject == "Plan") { ++seen; PB_CHECK_EQ(d.to_text, std::string("bob@example.org")); PB_CHECK_EQ(d.body, std::string("Text")); }
        else if (d.subject == "Only a subject") { ++seen; PB_CHECK_EQ(d.body, std::string("")); PB_CHECK_EQ(d.to_text, std::string("")); }
        else { ++seen; PB_CHECK_EQ(d.to_text, std::string("carol@example.org")); PB_CHECK_EQ(d.body, std::string("")); PB_CHECK_EQ(d.subject, std::string("")); }
    }
    PB_CHECK_EQ(seen, 3);
}

PB_TEST(a_start_without_arguments_or_without_the_marker_makes_nothing) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    QSignalSpy notices(&b, &HandoffBridge::notice);
    b.checkInbox();                                  // no entry
    b.deliver({}, "listener");
    b.deliver({"/mnt/ext1/applications/pboffice.app"}, "listener");
    b.deliver({"text=hi", "textlen=2"}, "listener");
    pump();
    PB_CHECK(e.store->drafts().empty());
    PB_CHECK_EQ(shown.count(), 0);
    PB_CHECK_EQ(notices.count(), 0);
}

PB_TEST(two_handoffs_are_two_drafts_the_newest_is_shown_and_the_other_waits_in_the_drafts_list) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    b.deliver(args_of("first"), "listener");
    b.deliver(args_of("second"), "listener");
    pump();
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(2));                  // never merged, never replaced
    PB_CHECK_EQ(shown.count(), 2);
    PB_CHECK_EQ(shown.at(0).at(1).toInt(), 0);
    PB_CHECK_EQ(shown.at(1).at(1).toInt(), 1);               // one more is waiting in the drafts
    const qlonglong newest = shown.at(1).at(0).toLongLong();
    PB_CHECK_EQ(e.store->draft(newest)->body, std::string("second"));
    PB_CHECK_EQ(b.waiting(), 2);
    b.shown();                                               // the newest was opened: the older one is in the drafts list, not waiting to be shown
    PB_CHECK_EQ(b.waiting(), 0);
}

PB_TEST(the_same_text_twice_is_two_drafts_and_nothing_guesses_it_is_a_duplicate) {
    Env e;
    HandoffBridge b(e.config());
    b.deliver(args_of("same"), "listener");
    b.deliver(args_of("same"), "listener");
    pump();
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(2));
    PB_CHECK(drafts[0].message_id != drafts[1].message_id);   // two messages, two identities
}

PB_TEST(a_crash_between_the_draft_and_the_removal_gives_a_second_draft_never_none) {
    Env e;
    HandoffBridge b(e.config());
    e.write_entry("1-1", args_of("precious"));
    b.checkInbox();
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    e.write_entry("1-1", args_of("precious"));    // as if the app had died before it could remove the entry
    b.checkInbox();                               // the next start finds it again
    PB_CHECK_EQ(e.store->drafts().size(), size_t(2));
}

PB_TEST(when_the_draft_cannot_be_saved_the_entry_stays_and_nothing_is_shown) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    e.write_entry("1-1", args_of("must not be lost"));
    PB_CHECK_EQ(e.store->debug_exec("ALTER TABLE draft RENAME TO draft_hidden"), std::string(""));   // saving a draft now fails
    b.checkInbox();
    PB_CHECK(e.entry_exists("1-1"));
    PB_CHECK_EQ(shown.count(), 0);
    PB_CHECK_EQ(e.store->debug_exec("ALTER TABLE draft_hidden RENAME TO draft"), std::string(""));
    b.checkInbox();                               // the next look succeeds
    PB_CHECK(!e.entry_exists("1-1"));
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    PB_CHECK_EQ(e.store->drafts()[0].body, std::string("must not be lost"));
}

PB_TEST(the_bridge_needs_no_sync_and_makes_no_outbox_entry_so_a_handoff_sends_nothing) {
    Env e;
    HandoffBridge b(e.config());                   // built without any SyncBridge: a hand-over cannot start a sync (FR-004)
    b.deliver(args_of("hello", "Hi", "bob@example.org"), "listener");
    pump();
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM outbox"), std::string("0"));
}

// ---- User Story 3: nothing a hand-over carries is logged, shown in a notice or kept anywhere but in the draft (spec 003 FR-015, FR-016) ----

namespace {

// every window of `n` characters of each value must be absent from `haystack`
bool holds_any_part(const std::string& haystack, const std::vector<std::string>& values, size_t n = 8) {
    for (const std::string& v : values)
        for (size_t i = 0; i + n <= v.size(); ++i)
            if (haystack.find(v.substr(i, n)) != std::string::npos) return true;
    return false;
}

}  // namespace

PB_TEST(no_log_line_and_no_notice_holds_the_text_the_subject_or_the_recipient_or_a_part_of_them) {
    const std::string text = "ZQXJ-PRIVATE-TEXT-4711 and a secret link https://cloud.example.org/s/QWERTZ987";
    const std::string subject = "ZQXJ-PRIVATE-SUBJECT-4711";
    const std::string to = "zqxj.private.rcpt.4711@example.org";
    const std::vector<std::string> values = {text, subject, to};
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    Env e;
    {
        HandoffBridge b(e.config());
        QSignalSpy notices(&b, &HandoffBridge::notice);
        b.deliver(args_of(text, subject, to), "listener");                                           // a good one
        b.deliver({"pbo-mail-v1", "text=" + text, "textlen=1", "subject=" + subject, "to=" + to}, "listener");   // incomplete
        b.deliver({"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size()), "text=" + text}, "listener");   // malformed (a key twice)
        b.deliver(args_of(std::string(9000, 'q') + text, subject, to), "listener");                  // too long
        b.deliver(args_of(text, std::string(400, 'z') + subject, "list@example.org, " + to), "listener");   // subject too long, recipient ignored
        e.write_entry("1-1", args_of(text, subject, to));
        b.checkInbox();
        e.dir.write("handoff/2-2", "");                                                              // not a folder: not an entry
        pump();
        for (int i = 0; i < notices.count(); ++i)
            for (const QVariant& arg : notices.at(i)) PB_CHECK(!holds_any_part(arg.toString().toStdString(), values));
    }
    PB_CHECK(e.store->drafts().size() >= 2);                                                         // the good ones were saved ...
    PB_CHECK_EQ(e.store->debug_exec("ALTER TABLE draft RENAME TO draft_hidden"), std::string(""));   // ... and a failing store is logged without the values too
    {
        HandoffBridge b(e.config());
        b.deliver(args_of(text, subject, to), "listener");
        pump();
    }
    pbo::log::reset_sink();
    PB_CHECK(!lines.empty());
    for (const std::string& line : lines) PB_CHECK(!holds_any_part(line, values));
}

PB_TEST(only_the_four_codes_with_counts_and_keys_of_the_contract_are_logged_by_a_handoff) {
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    Env e;
    HandoffBridge b(e.config());
    b.deliver(args_of("a text", "a subject", "bob@example.org"), "listener");
    b.deliver({"pbo-mail-v1", "text=abc", "textlen=1"}, "listener");
    e.write_entry("1-1", args_of("from a file"));
    b.checkInbox();
    pump();
    pbo::log::reset_sink();
    int received = 0, drafted = 0, refused = 0;
    for (const std::string& line : lines) {
        if (line.find("handoff.received") != std::string::npos) {
            ++received;
            PB_CHECK(line.find("text_chars=") != std::string::npos && line.find("fields=") != std::string::npos && line.find("source=") != std::string::npos);
        } else if (line.find("handoff.drafted") != std::string::npos) {
            ++drafted;
            PB_CHECK(line.find("count=1") != std::string::npos);
        } else if (line.find("handoff.refused") != std::string::npos) {
            ++refused;
            PB_CHECK(line.find("reason=incomplete") != std::string::npos);
        } else {
            PB_FAIL_AT("an unexpected log line: " + line);
        }
    }
    PB_CHECK_EQ(received, 2);
    PB_CHECK_EQ(drafted, 2);
    PB_CHECK_EQ(refused, 1);
    int masks = 0;
    for (const std::string& line : lines)
        if (line.find("fields=7") != std::string::npos) ++masks;                                    // text + subject + recipient = 1 + 2 + 4
    PB_CHECK_EQ(masks, 1);
}

// ---- User Story 4: odd, large or unwanted hand-overs (spec 003 FR-008, FR-009, FR-012..014) ----

namespace {

std::string notice_keys(const QSignalSpy& spy) {
    std::string keys;
    for (int i = 0; i < spy.count(); ++i) keys += (keys.empty() ? "" : ",") + spy.at(i).at(0).toString().toStdString();
    return keys;
}

// true when any file below `dir` holds `needle`
bool file_tree_holds(const std::string& dir, const std::string& needle) {
    std::string cmd = "grep -rqF -- '" + needle + "' '" + dir + "' 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

}  // namespace

PB_TEST(a_text_that_is_too_long_or_incomplete_or_malformed_is_refused_with_its_notice_and_its_entry_is_removed) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    QSignalSpy notices(&b, &HandoffBridge::notice);
    b.deliver(args_of(std::string(8193, 'x')), "listener");
    b.deliver({"pbo-mail-v1", "text=abc", "textlen=99"}, "listener");
    b.deliver({"pbo-mail-v1", "to=a@example.org", "to=b@example.org"}, "listener");
    pump();
    PB_CHECK_EQ(notice_keys(notices), std::string("too_long,incomplete,malformed"));
    PB_CHECK_EQ(shown.count(), 0);
    PB_CHECK(e.store->drafts().empty());
    e.write_entry("1-1", args_of(std::string(8193, 'x')));
    e.write_entry("2-2", {"pbo-mail-v1", "text=abc", "textlen=99"});
    notices.clear();
    b.checkInbox();
    PB_CHECK_EQ(notice_keys(notices), std::string("too_long,incomplete"));
    PB_CHECK(!e.entry_exists("1-1"));
    PB_CHECK(!e.entry_exists("2-2"));
    PB_CHECK(e.store->drafts().empty());
}

PB_TEST(an_entry_the_reader_refuses_because_it_is_not_what_the_launcher_writes_is_malformed_and_removed) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy notices(&b, &HandoffBridge::notice);
    e.write_entry("1-1", {"pbo-mail-v1", "a", "b", "c", "d", "e", "f", "g", "h", "i"});   // nine files
    b.checkInbox();
    PB_CHECK_EQ(notice_keys(notices), std::string("malformed"));
    PB_CHECK(!e.entry_exists("1-1"));
    PB_CHECK(e.store->drafts().empty());
}

PB_TEST(a_handoff_without_content_makes_no_draft_and_no_notice_and_its_entry_is_removed) {
    Env e;
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    QSignalSpy notices(&b, &HandoffBridge::notice);
    b.deliver(args_of("   \n\t "), "listener");
    b.deliver({"pbo-mail-v1"}, "listener");
    b.deliver({"pbo-mail-v1", "subject= ", "to="}, "listener");
    e.write_entry("1-1", args_of("  "));
    b.checkInbox();
    pump();
    PB_CHECK(e.store->drafts().empty());
    PB_CHECK_EQ(shown.count(), 0);
    PB_CHECK_EQ(notices.count(), 0);
    PB_CHECK(!e.entry_exists("1-1"));
}

PB_TEST(control_characters_and_invalid_utf8_become_a_draft_with_visible_replacements) {
    Env e;
    HandoffBridge b(e.config());
    const std::string raw = fixture("invalid_utf8.bin");
    b.deliver(args_of(raw + "\x01\x1b[31m"), "listener");
    pump();
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(1));
    PB_CHECK(drafts[0].body.find("\xef\xbf\xbd") != std::string::npos);
    PB_CHECK(drafts[0].body.find('\x01') == std::string::npos);
    PB_CHECK(drafts[0].body.find('\x1b') == std::string::npos);
    PB_CHECK(drafts[0].body.find("ok ") == 0);   // what was readable is kept
}

PB_TEST(a_subject_that_is_too_long_and_a_recipient_that_is_not_one_address_are_left_out_with_a_notice_after_the_editor) {
    Env e;
    HandoffBridge b(e.config());
    QStringList order;
    QObject::connect(&b, &HandoffBridge::show, &b, [&](qlonglong, int) { order << "show"; });
    QObject::connect(&b, &HandoffBridge::notice, &b, [&](const QString& key, int) { order << key; });
    b.deliver(args_of("the text", std::string(301, 'a'), "a@example.org, b@example.org"), "listener");
    pump();
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(1));
    PB_CHECK_EQ(drafts[0].body, std::string("the text"));
    PB_CHECK_EQ(drafts[0].subject, std::string(""));
    PB_CHECK_EQ(drafts[0].to_text, std::string(""));
    PB_CHECK_EQ(order.join(",").toStdString(), std::string("show,subject_too_long,recipient_ignored"));   // the editor first, then what was left out
}

PB_TEST(without_a_mail_account_a_handoff_is_held_in_memory_only_and_becomes_a_draft_when_the_account_is_saved) {
    Env e(false);
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    QSignalSpy notices(&b, &HandoffBridge::notice);
    b.setupActive(true);                                       // the account set-up page is the current page
    const std::string text = "HELD-ONLY-IN-MEMORY-4711 secret link";
    b.deliver(args_of(text, "Held subject", "bob@example.org"), "listener");
    e.write_entry("1-1", args_of("HELD-FROM-A-FILE-4711"));
    b.checkInbox();
    pump();
    PB_CHECK(e.store->drafts().empty());
    PB_CHECK_EQ(shown.count(), 0);
    PB_CHECK_EQ(notices.count(), 0);
    PB_CHECK(!e.entry_exists("1-1"));                          // the entry is gone: nothing of the text waits on the card
    PB_CHECK(!file_tree_holds(e.dir.path(), "HELD-ONLY-IN-MEMORY-4711"));
    PB_CHECK(!file_tree_holds(e.dir.path(), "HELD-FROM-A-FILE-4711"));
    pbo::MailAccount a;
    a.imap_host = "mail.example.org";
    a.smtp_host = "smtp.example.org";
    a.username = "alice";
    a.address = "alice@example.org";
    PB_CHECK(e.store->save_mail_account(a).ok());
    b.accountReady();
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(2));                    // each hand-over is its own draft
    PB_CHECK_EQ(shown.count(), 2);
    PB_CHECK_EQ(shown.at(1).at(1).toInt(), 1);                // the newest is shown, one waits in the drafts
    bool found = false;
    for (const auto& d : drafts)
        if (d.body == text) { found = true; PB_CHECK_EQ(d.subject, std::string("Held subject")); PB_CHECK_EQ(d.to_text, std::string("bob@example.org")); }
    PB_CHECK(found);
    b.accountReady();                                          // nothing is held any more: no further draft
    PB_CHECK_EQ(e.store->drafts().size(), size_t(2));
}

PB_TEST(when_the_set_up_is_left_a_held_handoff_is_discarded_with_the_notice_and_nothing_is_made_later) {
    Env e(false);
    HandoffBridge b(e.config());
    QSignalSpy shown(&b, &HandoffBridge::show);
    QSignalSpy notices(&b, &HandoffBridge::notice);
    b.setupActive(true);
    b.deliver(args_of("one"), "listener");
    b.deliver(args_of("two"), "listener");
    pump();
    PB_CHECK_EQ(notices.count(), 0);
    b.setupActive(false);
    b.setupLeft();
    PB_CHECK_EQ(notices.count(), 1);
    PB_CHECK_EQ(notices.at(0).at(0).toString().toStdString(), std::string("no_account"));
    PB_CHECK_EQ(notices.at(0).at(1).toInt(), 2);              // two hand-overs were not kept
    pbo::MailAccount a;
    a.imap_host = "mail.example.org";
    a.smtp_host = "smtp.example.org";
    a.username = "alice";
    a.address = "alice@example.org";
    e.store->save_mail_account(a);
    b.accountReady();
    PB_CHECK(e.store->drafts().empty());                       // the discarded text does not come back
    PB_CHECK_EQ(shown.count(), 0);
    b.setupLeft();                                             // nothing is held: no second notice
    PB_CHECK_EQ(notices.count(), 1);
}

PB_TEST(without_a_mail_account_and_without_the_set_up_page_the_user_is_told_at_once_and_nothing_is_held) {
    Env e(false);
    HandoffBridge b(e.config());
    QSignalSpy notices(&b, &HandoffBridge::notice);
    b.deliver(args_of("calendar-only account, no mail"), "listener");
    pump();
    PB_CHECK_EQ(notice_keys(notices), std::string("no_account"));
    PB_CHECK(e.store->drafts().empty());
    b.setupActive(true);                                       // opening the set-up afterwards finds nothing held
    pbo::MailAccount a;
    a.imap_host = "mail.example.org";
    a.smtp_host = "smtp.example.org";
    a.username = "alice";
    a.address = "alice@example.org";
    e.store->save_mail_account(a);
    b.accountReady();
    PB_CHECK(e.store->drafts().empty());
}

PB_TEST_MAIN()
