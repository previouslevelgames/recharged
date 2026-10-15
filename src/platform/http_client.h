#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace mscharged::platform
{
// Lets another thread stop a running request: the transfer ends with an
// HttpError whose Cancelled() is true.
class HttpCancel
{
public:
    void Cancel();
    bool Cancelled() const { return cancelled_; }

private:
    friend class HttpTransfer;
    std::atomic<bool> cancelled_{false};
#ifdef _WIN32
    std::mutex mutex_;
    void* handle_ = nullptr; // the WinHTTP request, closed by Cancel()
#endif
};

// HTTPS GET requests for the launcher's update check and downloads, through
// what the system provides: WinHTTP on Windows, elsewhere the system's
// libcurl, loaded at run time so that a system without it only loses updates.
struct HttpRequest
{
    std::string url; // https only, also for redirects
    std::string user_agent = "mscharged";
    std::vector<std::string> headers; // "Name: value"
    int connect_timeout = 15;          // seconds
    int stall_timeout = 30;            // seconds without data before the transfer fails
    std::uint64_t max_size = 16u << 20; // HttpGet refuses larger answers
    std::shared_ptr<HttpCancel> cancel;
};

struct HttpResponse
{
    long status = 0;
    std::string body;
};

// Bytes received so far and the expected total (0 when unknown).
using HttpProgress = std::function<void(std::uint64_t received, std::uint64_t total)>;

class HttpError : public std::runtime_error
{
public:
    explicit HttpError(const std::string& message, bool cancelled = false)
        : std::runtime_error(message), cancelled_(cancelled) {}
    bool Cancelled() const { return cancelled_; }

private:
    bool cancelled_;
};

// Empty when HTTPS requests work here, otherwise the reason they do not.
std::string HttpUnavailable();

// Network failures and cancellation throw HttpError; every HTTP status,
// including errors, is returned.
HttpResponse HttpGet(const HttpRequest& request, const HttpProgress& progress = {});

// Saves the answer's body to `file` (replacing it) and returns the status. The
// file remains only for status 200; it is removed after any other status or error.
long HttpDownload(const HttpRequest& request, const std::filesystem::path& file, const HttpProgress& progress = {});
} // namespace mscharged::platform
