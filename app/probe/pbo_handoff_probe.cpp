// pbo-handoff-probe: a small InkView program for the experiments of spec 003 (research "Device experiments", E1 to E8): what happens when one app starts PB Office
// (or a stand-in for it) with a text as the parameter, also when it is already running. It has no network, no Qt and no personal data: the texts handed over are
// fixed test strings with a unique marker word, and the output file holds only return values, events, counts and lengths, never the text itself.
// Adapted from the hand-over probe of the sibling project nextcloud-pocketbook (same author, MIT; see NOTICE).
//
//   pbo-handoff-probe [data-dir]            the test SENDER (default data-dir /mnt/ext1/system/pboffice)
//   pbo-handoff-probe target [data-dir]     the STAND-IN target of experiment E1: stays open, listens for requests of other apps, records them
//
// Sender: tap a box to start the target once. The top boxes choose the target (PB Office or the stand-in, pbo-handoff-target.app) and the flags of OpenBook2
// (0, OB_NONEWTASK, OB_WITHRETURN). A = marker, text, length, subject and recipient; B = text only; C = 1500 bytes with umlauts and line breaks; D = the marker
// alone; E = the next size of the sweep (E4); F = two calls one second apart (E6). Every tap appends one line to <data-dir>/handoff-probe.txt.
#include <inkview.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

const char* kOffice = "/mnt/ext1/applications/pboffice.app";
const char* kStandIn = "/mnt/ext1/applications/pbo-handoff-target.app";
const char* kMarkerWord = "HANDOFF-PROBE-7f3a9c";    // unique: experiment E3 searches for it everywhere
const int kSweep[] = {1500, 4000, 6000, 8192, 16000, 32000, 65000};
const int kSweepCount = sizeof(kSweep) / sizeof(kSweep[0]);

bool g_target_mode = false;
std::string g_log;
ifont* g_font = nullptr;
bool g_standin = false;       // sender: the target is the stand-in
int g_flag_index = 0;         // sender: 0 -> 0, 1 -> OB_NONEWTASK, 2 -> OB_WITHRETURN
int g_sweep_index = 0;
bool g_started = false;       // a hand-over was tried: every later event is written down (the sender may be hidden, suspended or closed)
int g_logged_after = 0;
int g_second_pending = 0;     // F: the second call is waiting for its timer

void note(const std::string& line) {
    if (FILE* f = std::fopen(g_log.c_str(), "a")) {
        std::fprintf(f, "%s: t=%ld %s\n", g_target_mode ? "target" : "sender", static_cast<long>(std::time(nullptr)), line.c_str());
        std::fflush(f);
        std::fclose(f);
    }
}

int flags_value() { return g_flag_index == 0 ? 0 : (g_flag_index == 1 ? OB_NONEWTASK : OB_WITHRETURN); }
const char* flags_name() { return g_flag_index == 0 ? "0" : (g_flag_index == 1 ? "OB_NONEWTASK" : "OB_WITHRETURN"); }

// ---- sender -------------------------------------------------------------------------------------------------------------------------------------------

struct Box { int x, y, w, h; std::string text; };

std::vector<Box> boxes() {
    const int w = ScreenWidth(), h = ScreenHeight();
    const int top = h / 8, rows = 8, each = (h - top - h / 20) / rows;
    const std::string labels[8] = {
        std::string("Target: ") + (g_standin ? "stand-in (tap to change)" : "PB Office (tap to change)"),
        std::string("Flags: ") + flags_name() + " (tap to change)",
        "A: marker, text, subject, recipient",
        "B: text only",
        "C: 1500 bytes, umlauts, line breaks",
        "D: the marker alone",
        std::string("E: next size of the sweep (") + std::to_string(kSweep[g_sweep_index]) + " bytes)",
        "F: two calls one second apart",
    };
    std::vector<Box> v;
    for (int i = 0; i < rows; ++i) v.push_back({w / 20, top + i * each, w - w / 10, each - h / 60, labels[i]});
    return v;
}

void draw() {
    ClearScreen();
    if (g_font) SetFont(g_font, BLACK);
    if (g_target_mode) {
        DrawTextRect(0, ScreenHeight() / 4, ScreenWidth(), ScreenHeight() / 2, "Stand-in target: listening. Read handoff-probe.txt. Back closes.", ALIGN_CENTER);
        FullUpdate();
        return;
    }
    DrawTextRect(0, ScreenHeight() / 40, ScreenWidth(), ScreenHeight() / 12, "Hand-over probe: tap a box, read system/pboffice/handoff-probe.txt", ALIGN_CENTER);
    for (const Box& b : boxes()) {
        FillArea(b.x, b.y, b.w, b.h, BLACK);
        FillArea(b.x + 3, b.y + 3, b.w - 6, b.h - 6, WHITE);
        DrawTextRect(b.x, b.y, b.w, b.h, b.text.c_str(), ALIGN_CENTER);
    }
    FullUpdate();
}

std::string repeat_text(const std::string& head, size_t total, const std::string& tail) {
    std::string t = head;
    if (total > head.size() + tail.size()) t.append(total - head.size() - tail.size(), 'x');
    return t + tail;
}

// The call. The line written says how it was made (target, flags, number of arguments, their lengths) and what came back; the arguments themselves stay out.
void call(const char* what, const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    std::string lens;
    for (const std::string& a : args) {
        argv.push_back(a.c_str());
        lens += (lens.empty() ? "" : ",") + std::to_string(a.size());
    }
    const char* target = g_standin ? kStandIn : kOffice;
    g_started = true;
    note(std::string("calling ") + what + " target=" + (g_standin ? "standin" : "office") + " flags=" + flags_name() + " argc=" + std::to_string(args.size()) + " lens=" + lens);
    const int rv = OpenBook2(target, static_cast<int>(argv.size()), argv.data(), flags_value());
    note(std::string("the call returned ") + std::to_string(rv));
}

void second_call_timer() {
    if (!g_second_pending) return;
    g_second_pending = 0;
    const std::string text = std::string(kMarkerWord) + "-F2 second call";
    call("F2", {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size())});
}

void start(int index) {
    switch (index) {
        case 0: g_standin = !g_standin; draw(); return;
        case 1: g_flag_index = (g_flag_index + 1) % 3; draw(); return;
        case 2: {  // A
            const std::string text = std::string(kMarkerWord) + "-A line one\nline two: Gr\xc3\xbc\xc3\x9f" "e \xe2\x82\xac 'q' \"d\" 123";
            call("A", {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size()), std::string("subject=") + kMarkerWord + "-subject", std::string("to=") + "probe@example.org"});
            return;
        }
        case 3: {  // B
            const std::string text = std::string(kMarkerWord) + "-B text only";
            call("B", {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size())});
            return;
        }
        case 4: {  // C: 1500 bytes with umlauts and line breaks
            std::string text = std::string(kMarkerWord) + "-C \xc3\xa4\xc3\xb6\xc3\xbc\xc3\x9f \xe2\x82\xac\nline two\n";
            while (text.size() < 1496) text += "abcdefghij";
            text.resize(1496);
            text += "-END";
            call("C", {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size())});
            return;
        }
        case 5:  // D
            call("D", {"pbo-mail-v1"});
            return;
        case 6: {  // E: the next size of the sweep
            const int size = kSweep[g_sweep_index];
            const std::string text = repeat_text(std::string(kMarkerWord) + "-E" + std::to_string(size) + "-", static_cast<size_t>(size), "-END");
            g_sweep_index = (g_sweep_index + 1) % kSweepCount;
            call("E", {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size())});
            draw();
            return;
        }
        case 7: {  // F: two calls one second apart (the first now, the second by timer)
            const std::string text = std::string(kMarkerWord) + "-F1 first call";
            call("F1", {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size())});
            g_second_pending = 1;
            SetHardTimer("pbo-handoff-f2", second_call_timer, 1000);
            return;
        }
        default: return;
    }
}

int hit(int x, int y) {
    const std::vector<Box> v = boxes();
    for (size_t i = 0; i < v.size(); ++i)
        if (x >= v[i].x && x < v[i].x + v[i].w && y >= v[i].y && y < v[i].y + v[i].h) return static_cast<int>(i);
    return -1;
}

// ---- stand-in target (experiment E1) ------------------------------------------------------------------------------------------------------------------

// The request listener of a running app: called by the system when another app starts this one with OpenBook2 while it runs (the SDK strings suggest that, research
// R-01). Records the thread, the sizes and the number of arguments; not the arguments. The memory of `argv` is not freed: who owns it is open item O-2.
int listener(int request, void* data, int inlen, int outlen) {
    (void)outlen;
    int argc = -1;
    char** argv = nullptr;
    if (data && inlen > 0) UnpackParameters(data, inlen, &argc, &argv);
    std::string lens;
    for (int i = 0; i < argc && argv; ++i) lens += (lens.empty() ? "" : ",") + std::to_string(argv[i] ? std::strlen(argv[i]) : 0);
    note("LISTENER called request=" + std::to_string(request) + " thread=" + std::to_string(static_cast<unsigned long>(pthread_self())) + " inlen=" + std::to_string(inlen) +
         " argc=" + std::to_string(argc) + " lens=" + lens);
    return 0;
}

// ---- events -------------------------------------------------------------------------------------------------------------------------------------------

int handler(int type, int p1, int p2) {
    if ((g_started || g_target_mode) && g_logged_after < 400 && type != EVT_POINTERUP && type != EVT_POINTERDOWN && type != EVT_POINTERMOVE && type != EVT_POINTERDRAG) {
        ++g_logged_after;
        note("event " + std::to_string(type) + " p1=" + std::to_string(p1));
    }
    switch (type) {
        case EVT_INIT:
            g_font = OpenFont("LiberationSans", 30, 1);
            note(std::string("started (event INIT), task=") + std::to_string(GetCurrentTask()));
            if (g_target_mode) SetRequestListener(REQ_OPENBOOK2, 0, listener);
            break;
        case EVT_SHOW:
            draw();
            break;
        case EVT_POINTERUP: {
            if (g_target_mode) break;
            const int i = hit(p1, p2);
            if (i >= 0) start(i);
            break;
        }
        case EVT_KEYPRESS:
            if (p1 == IV_KEY_BACK || p1 == IV_KEY_HOME || p1 == IV_KEY_MENU) {
                note("closing (key)");
                CloseApp();
            }
            break;
        case EVT_EXIT:
            note("event EXIT");
            break;
        default:
            break;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int next = 1;
    if (argc > 1 && std::strcmp(argv[1], "target") == 0) {
        g_target_mode = true;
        next = 2;
    }
    const std::string dir = argc > next ? argv[next] : "/mnt/ext1/system/pboffice";
    g_log = dir + "/handoff-probe.txt";
    InkViewMain(handler);
    return 0;
}
