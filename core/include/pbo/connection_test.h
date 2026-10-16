#pragma once
// The connection test of the account form (US1 scenario 1): tries the IMAP and the SMTP server with the values of the form and
// reports success or ONE cause key together with the side it belongs to. It stores nothing; the caller saves the account only
// after a success. Both sides are always tried (unless the form or the network rules the test out), so the user learns about
// both problems at once.

#include <set>
#include <string>

#include <vector>

#include "pbo/account_input.h"
#include "pbo/caldav.h"
#include "pbo/calendar_account_input.h"
#include "pbo/http_transport.h"
#include "pbo/imap_transport.h"
#include "pbo/smtp_transport.h"
#include "pbo/types.h"

namespace pbo {

struct ConnectionReport {
    bool imap_ok = false;
    bool smtp_ok = false;
    Error imap_error, smtp_error;
    std::string failed_side;                  // "imap" or "smtp" (the first failure); for an invalid form the field name; "" on success
    std::set<std::string> imap_capabilities;  // lowercase tokens, informational
};

// `network_up` false: no_network at once, no transport is touched. The Result's error is the first failure (IMAP before SMTP);
// its value always holds the report.
Result<ConnectionReport> test_mail_connection(ImapTransport& imap, SmtpTransport& smtp, const MailAccountInput& input, bool network_up = true);

struct CalendarConnectionReport {
    bool server_tested = false, feed_tested = false;
    bool server_ok = false, feed_ok = false;
    Error server_error, feed_error;
    std::vector<CalendarInfo> calendars;      // what the server offers (for the calendar selection)
    std::string failed_side;                  // "caldav" or "feed" (the first failure); for an invalid form the field name; "" on success
};

// The same for the calendar tab: the CalDAV server (discovery) and the feed (one download) are tried with the values of the form, both when
// both are given. Stores nothing. The Result's error is the first failure (server before feed); its value always holds the report.
Result<CalendarConnectionReport> test_calendar_connection(HttpTransport& http, const CalendarAccountInput& input, bool network_up = true);

}  // namespace pbo
