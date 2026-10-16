#include "pbo/connection_test.h"

#include "pbo/log.h"

namespace pbo {

Result<ConnectionReport> test_mail_connection(ImapTransport& imap, SmtpTransport& smtp, const MailAccountInput& input, bool network_up) {
    Result<ConnectionReport> result;
    ConnectionReport& report = result.value;

    const std::vector<FieldError> invalid = validate_mail_account(input);
    if (!invalid.empty()) {   // refused before any connection
        report.failed_side = invalid.front().field;
        result.error = make_error(ErrorClass::Unsupported, invalid.front().key, "a value of the form is not valid");
        return result;
    }
    if (!network_up) {
        result.error = make_error(ErrorClass::Network, "no_network", "there is no network connection");
        return result;
    }

    ImapEndpoint ie;
    ie.host = input.account.imap_host;
    ie.port = input.account.imap_port;
    ie.security = input.account.imap_security;
    ie.username = input.account.username;
    report.imap_error = imap.connect(ie, input.imap_password);
    report.imap_ok = report.imap_error.ok();
    if (report.imap_ok) report.imap_capabilities = imap.capabilities();
    imap.disconnect();

    SmtpEndpoint se;
    se.host = input.account.smtp_host;
    se.port = input.account.smtp_port;
    se.security = input.account.smtp_security;
    se.username = input.account.smtp_username.empty() ? input.account.username : input.account.smtp_username;
    const std::string smtp_password = input.smtp_password.empty() ? input.imap_password : input.smtp_password;
    report.smtp_error = smtp.test_connection(se, smtp_password);
    report.smtp_ok = report.smtp_error.ok();

    if (!report.imap_ok) {
        result.error = report.imap_error;
        report.failed_side = "imap";
    } else if (!report.smtp_ok) {
        result.error = report.smtp_error;
        report.failed_side = "smtp";
    }
    log::event("connection.tested", {log::text("imap", report.imap_ok ? "ok" : report.imap_error.key), log::text("smtp", report.smtp_ok ? "ok" : report.smtp_error.key)});
    return result;
}

Result<CalendarConnectionReport> test_calendar_connection(HttpTransport& http, const CalendarAccountInput& raw, bool network_up) {
    Result<CalendarConnectionReport> result;
    CalendarConnectionReport& report = result.value;
    const CalendarAccountInput input = normalize_calendar_account(raw);

    const std::vector<FieldError> invalid = validate_calendar_account(input);
    if (!invalid.empty()) {   // refused before any connection
        report.failed_side = invalid.front().field;
        result.error = make_error(ErrorClass::Unsupported, invalid.front().key, "a value of the form is not valid");
        return result;
    }
    if (!network_up) {
        result.error = make_error(ErrorClass::Network, "no_network", "there is no network connection");
        return result;
    }
    if (!input.caldav_url.empty()) {
        report.server_tested = true;
        CalDavClient client(http, input.caldav_url, input.username, input.password);
        auto found = client.discover();
        report.server_error = found.error;
        report.server_ok = found.ok();
        if (found.ok()) report.calendars = std::move(found.value);
    }
    if (!input.feed_url.empty()) {
        report.feed_tested = true;
        const FeedResult f = fetch_feed(http, input.feed_url, std::string(), std::string());
        report.feed_ok = f.state == FeedResult::State::Changed;
        report.feed_error = f.error;
    }
    if (report.server_tested && !report.server_ok) {
        result.error = report.server_error;
        report.failed_side = "caldav";
    } else if (report.feed_tested && !report.feed_ok) {
        result.error = report.feed_error;
        report.failed_side = "feed";
    }
    log::event("connection.tested", {log::text("caldav", !report.server_tested ? "none" : report.server_ok ? "ok" : report.server_error.key),
                                     log::text("feed", !report.feed_tested ? "none" : report.feed_ok ? "ok" : report.feed_error.key)});
    return result;
}

}  // namespace pbo
