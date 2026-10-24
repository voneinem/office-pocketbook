#pragma once
// Device glue for a hand-over to a RUNNING PB Office (spec 003, research R-05). The SDK strings suggest that the system forwards the arguments of a second start of
// a running app to it as a request (REQ_OPENBOOK2), which a request listener receives; the experiments E1 and E2 decide whether and how. On the host everything here
// is a no-op.

#include <string>

// Experiment E2: registers a listener that only RECORDS what arrives (the thread, the sizes, the number of arguments and their lengths, whether Qt sees the app
// in front), never the arguments, in <data-dir>/listener-probe.txt. `free_argv` also frees what UnpackParameters returned, to find out whether that is safe
// (open item O-2); a crash is the answer. Started only when the launcher's run-listener-probe flag was used.
void startListenerProbe(const std::string& data_dir, bool free_argv);
