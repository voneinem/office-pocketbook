#include "handoff_listener.h"

#ifdef PLATFORM_POCKETBOOK
#include <inkview.h>
#include <pthread.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

std::string g_probe_log;
bool g_free_argv = false;

void note(const std::string& line) {
    if (FILE* f = std::fopen(g_probe_log.c_str(), "a")) {
        std::fprintf(f, "listener-probe: t=%ld %s\n", static_cast<long>(std::time(nullptr)), line.c_str());
        std::fflush(f);
        std::fclose(f);
    }
}

// What Qt sees, asked on the Qt thread: is the app the active one, does it have a focus window?
void note_qt_view(const char* when) {
    QMetaObject::invokeMethod(
        qApp,
        [when]() {
            note(std::string(when) + " (Qt thread): applicationState=" + std::to_string(static_cast<int>(QGuiApplication::applicationState())) +
                 " focusWindow=" + (QGuiApplication::focusWindow() ? "yes" : "no"));
        },
        Qt::QueuedConnection);
}

// Called by the system, on a thread of its own or not: that is one of the things to find out. Only counts and lengths are written.
int probe_listener(int request, void* data, int inlen, int outlen) {
    (void)outlen;
    int argc = -1;
    char** argv = nullptr;
    if (data && inlen > 0) UnpackParameters(data, inlen, &argc, &argv);
    std::string lens;
    for (int i = 0; i < argc && argv; ++i) lens += (lens.empty() ? "" : ",") + std::to_string(argv[i] ? std::strlen(argv[i]) : 0);
    note("LISTENER called request=" + std::to_string(request) + " thread=" + std::to_string(static_cast<unsigned long>(pthread_self())) +
         " inlen=" + std::to_string(inlen) + " argc=" + std::to_string(argc) + " lens=" + lens + " taskActive=" + std::to_string(IsTaskActive()));
    if (g_free_argv && argv) {
        note("freeing argv now (if the next line is missing, the free crashed)");
        for (int i = 0; i < argc; ++i) std::free(argv[i]);
        std::free(argv);
        note("freed argv");
    }
    note_qt_view("after delivery");
    return 0;
}

}  // namespace

void startListenerProbe(const std::string& data_dir, bool free_argv) {
    g_probe_log = data_dir + "/listener-probe.txt";
    g_free_argv = free_argv;
    SetRequestListener(REQ_OPENBOOK2, 0, probe_listener);
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged, qApp, [](Qt::ApplicationState s) {
        note("Qt applicationState changed to " + std::to_string(static_cast<int>(s)));
    });
    note(std::string("registered; task=") + std::to_string(GetCurrentTask()) + " free_argv=" + (free_argv ? "yes" : "no"));
    note_qt_view("at start");
}

#else

void startListenerProbe(const std::string&, bool) {}

#endif
