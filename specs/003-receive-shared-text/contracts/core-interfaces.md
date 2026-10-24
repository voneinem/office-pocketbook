# Contract: new and changed C++ interfaces and the launcher

**Feature**: [spec.md](../spec.md) | **Plan**: [plan.md](../plan.md) | **Research**: [R-02, R-04, R-06, R-07](../research.md)

Sketches of the seams the tests are written against. Names may change in the first red test; the behaviour is the contract.
`core/` stays Qt-free and InkView-free (constitution II).

## core/include/pbo/handoff.h (new)

```cpp
namespace pbo {

constexpr size_t kHandoffMaxTextBytes = 8192;       // lowered to the measured limit by experiment E4, never below 6000
constexpr size_t kHandoffMaxSubjectChars = 300;
constexpr size_t kHandoffMaxAddressBytes = 254;
extern const char* const kHandoffMarker;            // "pbo-mail-v1"

enum class HandoffNotice { SubjectTooLong, RecipientIgnored };
enum class HandoffRefusal { None, TooLong, Incomplete, Malformed };

struct Handoff {
    std::string text, subject, to;                  // cleaned
    std::vector<HandoffNotice> notices;
    bool empty() const;                             // text, subject and to all empty or only white space
};

struct HandoffResult {
    bool is_handoff = false;                        // false: no marker, an ordinary start
    HandoffRefusal refusal = HandoffRefusal::None;  // not None: no Handoff
    Handoff handoff;                                // valid when is_handoff and refusal == None
};

// The arguments exactly as received (the system may put the app path first; everything before the marker is skipped).
HandoffResult parse_handoff(const std::vector<std::string>& args);

// The cleaning rules alone, for the tests: invalid bytes and controls -> U+FFFD, line ends -> "\n"; the subject variant makes one line.
std::string clean_handoff_text(std::string_view text);
std::string clean_handoff_subject(std::string_view text);

}  // namespace pbo
```

Rules: [research R-07](../research.md). `parse_handoff` never throws, never logs, never reads a file, and treats its input as
untrusted (bounded work: it stops at the limits; a 1 MB argument costs a length check).

## core/include/pbo/handoff_inbox.h (new)

```cpp
namespace pbo {

struct PendingHandoff {
    std::string id;                                 // the folder name
    std::vector<std::string> args;                  // one entry per file, raw bytes, in file order
    bool refused = false;                           // too many or too large files
};

class HandoffInbox {
public:
    HandoffInbox(std::string dir, Clock& clock);
    // Entries that are complete (not *.tmp), oldest first. Entries older than 10 minutes are removed unread (counted in `expired`).
    std::vector<PendingHandoff> take_ready(int* expired = nullptr);
    // Removes one entry; only called after the draft was saved or the hand-over was refused.
    bool remove(const std::string& id);
    static constexpr int kMaxFiles = 8;
    static constexpr size_t kMaxFileBytes = 16384;
    static constexpr int kExpirySeconds = 600;
};

}  // namespace pbo
```

`take_ready` reads; it does not remove (so a crash between draft and removal repeats the draft rather than losing it, data-model).
A missing directory is an empty result, not an error.

## bridge/include/handoff_bridge.h (new)

```cpp
class HandoffBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(int waiting READ waiting NOTIFY waitingChanged)     // drafts made by hand-overs not shown yet
public:
    struct Config { pbo::Store* store; pbo::Clock* clock; pbo::Random* random; std::string handoff_dir; };

    // Thread-safe: copies the arguments and queues the work to the bridge's thread. Used by the request listener (case B1).
    void deliver(std::vector<std::string> args, const char* source);

    Q_INVOKABLE void checkInbox();          // start, window to the front, wake signal
    Q_INVOKABLE void accountReady();        // mail account configured: continue a held hand-over
    Q_INVOKABLE void setupLeft();           // the set-up page was left: discard a held hand-over with the notice
    Q_INVOKABLE void shown();               // QML shows one waiting draft: the counter goes down

signals:
    void show(qlonglong draftId, int moreWaiting);         // open ComposePage { draftId }
    void notice(const QString& key, int n);                // key of research R-08
    void waitingChanged();
};
```

Behaviour (tested by `test_handoff_bridge`): the draft is saved *before* `show` is emitted; one draft per hand-over; `show` carries the
newest draft and the number of older ones that wait in the Drafts list; the bridge never calls `SyncBridge`; nothing it
logs or emits contains a handed-over value.

## app/ (changed): the three ways in, device only

| File | Change |
|------|--------|
| `app/handoff_listener.{h,cpp}` (new) | `#ifdef PLATFORM_POCKETBOOK`: registers `SetRequestListener(REQ_OPENBOOK2, ...)` after the window exists; the listener copies `data` through `UnpackParameters` into `std::vector<std::string>` and calls `HandoffBridge::deliver(..., "listener")`; on the host a no-op. Registers a `SIGUSR1` handler that only writes a byte to a self-pipe; a `QSocketNotifier` then calls `checkInbox()`. Connects `QGuiApplication::applicationStateChanged(Active)` to `checkInbox()`. Built only after experiment E1/E2 selects it (research R-05); the file reader works without it |
| `app/main.cpp` | creates `HandoffBridge`, exposes it to QML as `handoff`, calls `checkInbox()` once after the QML is loaded |
| `app/probe/pbo_handoff_probe.cpp` (new) | the test sender and the stand-in target of experiments E1 to E8; fixed test strings; writes counts only |

## platform/pocketbook/pboffice.app (changed): the capture block

```sh
# handoff-capture-begin  (the only lines that may expand "$@", "$1".."$9"; scripts/check-constitution.sh rule R11)
if [ "$#" -gt 0 ]; then
    if [ "$1" = "pbo-mail-v1" ]; then
        umask 077
        id="$(date +%s 2>/dev/null || echo 0)-$$"
        mkdir -p "$DATADIR/handoff/$id.tmp" 2>/dev/null || true
        shift
        n=0
        for a in "$@"; do printf '%s' "$a" > "$DATADIR/handoff/$id.tmp/$n"; n=$((n + 1)); done
        mv "$DATADIR/handoff/$id.tmp" "$DATADIR/handoff/$id" 2>/dev/null || true
    fi
    exec sh "$0"
fi
# handoff-capture-end
```

Placed after `DATADIR=...` and before `mkdir -p "$DATADIR"`'s log handling, `exec >log`, any `echo` and the diagnostics. The
`exec` runs for every start with arguments, marker or not, so that no argument stays in the long-lived process. A sketch: the
real block also limits the number of arguments to 8 and caps each at the reader's limit (`head -c` is not on every reader, so the app
enforces the limit when reading; the launcher bounds only the count).

Case B2 adds, after the block and before the app starts: if a PB Office instance is running, `kill -USR1 <its pid>` (or, on the
fallback, `kill -TERM` and a bounded wait), then `exit 0`. Not written before experiment E1 selects it.

## Checks added

| Check | Where |
|-------|-------|
| rule R11 (launcher expands arguments only inside the capture block) with self-test | `scripts/check-constitution.sh` |
| `test_handoff_launcher.sh` | `tests/launcher/`, registered in `tests/CMakeLists.txt` like the other launcher tests |
| `check-fixtures.sh` | the hand-over fixtures carry the `.unverified` marker where they imitate the system's arguments |
