#pragma once
// Scripted HttpTransport for contract and integration tests (T016). Records every request
// including credentials and headers. Accepts any URL scheme.

#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "pbo/http_transport.h"

namespace pbtest {

class FakeTransport : public pbo::HttpTransport {
public:
    struct Expectation {
        std::string method;
        std::string url_contains;  // substring of the full URL
        pbo::Response response;
    };

    // Queue of expectations, consumed in order. A request that does not match the next
    // expectation is recorded as a mismatch and answered with a Server error.
    FakeTransport& expect(const std::string& method, const std::string& url_contains, pbo::Response response) {
        queue_.push_back({method, url_contains, std::move(response)});
        return *this;
    }

    // Alternative to the queue: a handler decides every answer (fake server logic).
    void set_handler(std::function<pbo::Response(const pbo::Request&)> handler) { handler_ = std::move(handler); }

    // Failure injection: the n-th request (0-based) fails with a transport error instead.
    void fail_request(size_t index, pbo::ErrorClass error) { failures_.push_back({index, error}); }

    // Simulates a full device for the next file downloads (Response::storage_full, nothing written).
    bool simulate_storage_full = false;

    pbo::Response perform(const pbo::Request& request) override {
        pbo::Response r = perform_scripted(request);
        apply_file_semantics(request, &r);
        return r;
    }

    const std::vector<pbo::Request>& requests() const { return requests_; }
    void clear_requests() { requests_.clear(); }
    const std::vector<std::string>& mismatches() const { return mismatches_; }
    bool all_consumed() const { return next_ == queue_.size(); }

private:
    // What the real transport does with save_body_to / max_body_bytes: the body of a 2xx answer goes into the
    // file (Response::body stays empty); a Content-Length or a body over the cap stops the transfer.
    void apply_file_semantics(const pbo::Request& request, pbo::Response* r) {
        if (request.save_body_to.empty() || r->status < 200 || r->status >= 300 || r->error != pbo::ErrorClass::None) return;
        if (r->body_too_large || r->storage_full) return;
        if (request.method == "HEAD") return;
        const std::string length = pbo::header_value(r->headers, "Content-Length");
        if (request.max_body_bytes > 0) {
            const long long declared = length.empty() ? -1 : std::atoll(length.c_str());
            if (declared > request.max_body_bytes || static_cast<long long>(r->body.size()) > request.max_body_bytes) {
                r->body_too_large = true;
                r->body.clear();
                return;
            }
        }
        if (simulate_storage_full) {
            r->storage_full = true;
            r->body.clear();
            return;
        }
        std::ofstream f(request.save_body_to, std::ios::binary | std::ios::trunc);
        f << r->body;
        r->body.clear();
    }

    pbo::Response perform_scripted(const pbo::Request& request) {
        const size_t index = requests_.size();
        requests_.push_back(request);
        for (const auto& f : failures_) {
            if (f.first == index) {
                pbo::Response r;
                r.error = f.second;
                r.error_detail = "injected failure";
                return r;
            }
        }
        if (handler_) return handler_(request);
        if (next_ < queue_.size()) {
            const Expectation& e = queue_[next_];
            if (e.method == request.method && request.url.find(e.url_contains) != std::string::npos) {
                ++next_;
                return e.response;
            }
        }
        mismatches_.push_back(request.method + " " + request.url);
        pbo::Response r;
        r.status = 599;
        r.error = pbo::ErrorClass::Server;
        r.error_detail = "unexpected request";
        return r;
    }

public:
    static pbo::Response ok(const std::string& body, pbo::Headers headers = {}) {
        pbo::Response r;
        r.status = 200;
        r.body = body;
        r.headers = std::move(headers);
        return r;
    }
    static pbo::Response status(long code, const std::string& body = "", pbo::Headers headers = {}) {
        pbo::Response r;
        r.status = code;
        r.body = body;
        r.headers = std::move(headers);
        return r;
    }

private:
    std::vector<Expectation> queue_;
    size_t next_ = 0;
    std::function<pbo::Response(const pbo::Request&)> handler_;
    std::vector<std::pair<size_t, pbo::ErrorClass>> failures_;
    std::vector<pbo::Request> requests_;
    std::vector<std::string> mismatches_;
};

}  // namespace pbtest
