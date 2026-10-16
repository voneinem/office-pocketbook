#pragma once
// pbo-probe --imap-selftest <host> <port> <ca.pem> [--bad-login] [--append <mailbox>]
// Runs the checks of tests/contract/test_imap_transport.cpp that matter for the device against any IMAP server over TLS
// with the real transport (libcurl as installed on the device) and prints one line per check:
//   PASS <name> | FAIL <name> <cause key> | SKIP <name> <reason> | INFO <name> <figures>
// It never prints message data, addresses or credentials. User name and password come from the environment
// (PBO_SELFTEST_USER, PBO_SELFTEST_PASSWORD) and default to the demo server's values. Returns 0 when no check failed.
int run_imap_selftest(int argc, char** argv);   // argv[0] is the first argument after --imap-selftest
