#include "platform/http_client.h"

#include "platform/path.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <system_error>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace mscharged::platform
{
namespace
{
// Receives the body; throws to stop the transfer.
using Sink = std::function<void(const char* data, std::size_t size)>;

bool HttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }
bool HttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0; }

#ifndef _WIN32
// libcurl's stable easy interface (libcurl.so.4 / libcurl.4.dylib). The
// option numbers are part of its ABI and are checked against curl.h.
constexpr int kCurlUrl = 10002, kCurlWriteFunction = 20011, kCurlWriteData = 10001, kCurlUserAgent = 10018,
              kCurlHttpHeader = 10023, kCurlFollowLocation = 52, kCurlMaxRedirects = 68,
              kCurlConnectTimeout = 78, kCurlLowSpeedLimit = 19, kCurlLowSpeedTime = 20, kCurlNoProgress = 43,
              kCurlProgressFunction = 20219, kCurlProgressData = 10057, kCurlNoSignal = 99,
              kCurlAcceptEncoding = 10102, kCurlErrorBuffer = 10010, kCurlProtocols = 10318,
              kCurlRedirectProtocols = 10319, kCurlProtocolMask = 181, kCurlRedirectProtocolMask = 182,
              kCurlPostFields = 10015, kCurlPostFieldSize = 60, kCurlCustomRequest = 10036;
constexpr long kCurlHttpsOnly = 2, kCurlHttpAndHttps = 3; // CURLPROTO_HTTPS, | CURLPROTO_HTTP
constexpr int kCurlResponseCode = 0x200002;
constexpr int kCurlAborted = 42, kCurlWriteError = 23, kCurlTimedOut = 28;

struct CurlApi
{
    int (*global_init)(long) = nullptr;
    void* (*easy_init)() = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
    bool ready = false;
    std::string error;
};

const CurlApi& Curl()
{
    static CurlApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* const names[] = {
#ifdef __APPLE__
            "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib",
#else
            "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so",
#endif
        };
        void* library = nullptr;
        for (const char* name : names)
            if ((library = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!library)
        {
            api.error = "the system's libcurl was not found";
            return;
        }
        // Kept loaded for the rest of the run.
        bool complete = true;
        auto load = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
            complete = complete && function;
        };
        load(api.global_init, "curl_global_init");
        load(api.easy_init, "curl_easy_init");
        load(api.easy_setopt, "curl_easy_setopt");
        load(api.easy_perform, "curl_easy_perform");
        load(api.easy_getinfo, "curl_easy_getinfo");
        load(api.easy_cleanup, "curl_easy_cleanup");
        load(api.easy_strerror, "curl_easy_strerror");
        load(api.slist_append, "curl_slist_append");
        load(api.slist_free_all, "curl_slist_free_all");
        if (!complete) api.error = "the system's libcurl is incomplete";
        else if (api.global_init(3 /* CURL_GLOBAL_DEFAULT */) != 0) api.error = "the system's libcurl failed to start";
        else api.ready = true;
    });
    return api;
}
#else
std::wstring Wide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(std::size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
}

std::string Narrow(const wchar_t* text, int length)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string narrow(std::size_t(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

std::string WinHttpMessage(DWORD error)
{
    switch (error)
    {
    case ERROR_WINHTTP_TIMEOUT: return "the connection timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the server name could not be resolved (no internet connection?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect to the server";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection failed";
    default: break;
    }
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"winhttp.dll"), error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message = length ? Narrow(buffer, int(length)) : std::string();
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '
        || message.back() == '.'))
        message.pop_back();
    return message.empty() ? "network error " + std::to_string(error) : message;
}

struct InternetHandle
{
    HINTERNET handle = nullptr;
    ~InternetHandle() { if (handle) WinHttpCloseHandle(handle); }
};
#endif
} // namespace

class HttpTransfer
{
public:
    HttpTransfer(const HttpRequest& request, const HttpProgress& progress) : request_(request), progress_(progress) {}

    long Run(const Sink& sink)
    {
        if (!HttpsUrl(request_.url) && !(request_.allow_http && HttpUrl(request_.url)))
            throw HttpError("Only https addresses can be requested: " + request_.url);
        // A GET whose network failure came before any data is tried once more
        // (a dropped or stale connection); a partial answer is never repeated,
        // nor a request that may have changed something on the server.
        const Sink counted = [&](const char* data, std::size_t size) {
            delivered_ = true;
            sink(data, size);
        };
        const bool repeatable = request_.method == "GET";
        for (int attempt = 0;; ++attempt)
        {
            if (Cancelled()) throw HttpError("Cancelled", true);
            try
            {
#ifdef _WIN32
                return RunWinHttp(counted);
#else
                return RunCurl(counted);
#endif
            }
            catch (const HttpError& error)
            {
                if (error.Cancelled() || delivered_ || !repeatable || attempt > 0) throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

private:
    bool Cancelled() const { return request_.cancel && request_.cancel->Cancelled(); }

    void Report(std::uint64_t received, std::uint64_t total)
    {
        if (progress_) progress_(received, total);
    }

#ifdef _WIN32
    // The request handle is shared with HttpCancel::Cancel, which closes it to
    // end a blocking WinHTTP call at once; whoever takes it first closes it.
    void Register(HINTERNET handle)
    {
        if (!request_.cancel) return;
        std::lock_guard<std::mutex> lock(request_.cancel->mutex_);
        request_.cancel->handle_ = handle;
    }

    bool Unregister()
    {
        if (!request_.cancel) return true;
        std::lock_guard<std::mutex> lock(request_.cancel->mutex_);
        const bool owned = request_.cancel->handle_ != nullptr;
        request_.cancel->handle_ = nullptr;
        return owned;
    }

    [[noreturn]] void Fail(DWORD error)
    {
        if (Cancelled() || error == ERROR_WINHTTP_OPERATION_CANCELLED) throw HttpError("Cancelled", true);
        throw HttpError(WinHttpMessage(error));
    }

    long RunWinHttp(const Sink& sink)
    {
        const std::wstring url = Wide(request_.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts)
            || !(parts.nScheme == INTERNET_SCHEME_HTTPS || (request_.allow_http && parts.nScheme == INTERNET_SCHEME_HTTP)))
            throw HttpError("Invalid address: " + request_.url);
        const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        const std::wstring agent = Wide(request_.user_agent);
        InternetHandle session;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
        // The system's proxy settings (Windows 8.1 and newer).
        session.handle = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
#endif
        if (!session.handle)
            session.handle = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.handle) Fail(GetLastError());
        const int connect = request_.connect_timeout * 1000, stall = request_.stall_timeout * 1000;
        WinHttpSetTimeouts(session.handle, connect, connect, stall, stall);
        InternetHandle connection;
        connection.handle = WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0);
        if (!connection.handle) Fail(GetLastError());
        const std::wstring method = Wide(request_.method);
        HINTERNET handle = WinHttpOpenRequest(connection.handle, method.c_str(), path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              secure ? WINHTTP_FLAG_SECURE : 0);
        if (!handle) Fail(GetLastError());
        Register(handle);
        // A fresh connection per request: a kept-alive connection the server
        // has closed meanwhile fails the next request (seen under Wine).
        DWORD features = WINHTTP_DISABLE_KEEP_ALIVE;
        WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
        struct Close
        {
            HttpTransfer& transfer;
            HINTERNET handle;
            ~Close() { if (transfer.Unregister()) WinHttpCloseHandle(handle); }
        } close{*this, handle};
        if (Cancelled()) throw HttpError("Cancelled", true);
#if defined(WINHTTP_OPTION_DECOMPRESSION) && defined(WINHTTP_DECOMPRESSION_FLAG_ALL)
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL; // Windows 8.1 and newer; optional
        WinHttpSetOption(handle, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof decompression);
#endif
        std::wstring headers;
        for (const auto& header : request_.headers) headers += Wide(header) + L"\r\n";
        // A body for every method but GET, even an empty one (Content-Length: 0).
        const bool body = request_.method != "GET";
        void* data = body && !request_.body.empty() ? const_cast<char*>(request_.body.data()) : WINHTTP_NO_REQUEST_DATA;
        const DWORD length = body ? DWORD(request_.body.size()) : 0;
        if (!WinHttpSendRequest(handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : DWORD(-1), data, length, length, 0)
            || !WinHttpReceiveResponse(handle, nullptr))
            Fail(GetLastError());
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            Fail(GetLastError());
        DWORD content = 0;
        size = sizeof content;
        const std::uint64_t total = WinHttpQueryHeaders(handle, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                                        WINHTTP_HEADER_NAME_BY_INDEX, &content, &size,
                                                        WINHTTP_NO_HEADER_INDEX) ? content : 0;
        std::vector<char> buffer(1 << 16);
        std::uint64_t received = 0;
        Report(0, total);
        for (;;)
        {
            DWORD read = 0;
            if (!WinHttpReadData(handle, buffer.data(), DWORD(buffer.size()), &read)) Fail(GetLastError());
            if (read == 0) break;
            received += read;
            sink(buffer.data(), read);
            Report(received, total);
            if (Cancelled()) throw HttpError("Cancelled", true);
        }
        return long(status);
    }
#else
    struct CurlContext
    {
        HttpTransfer* transfer;
        const Sink* sink;
        std::exception_ptr failure;
    };

    static std::size_t Write(char* data, std::size_t size, std::size_t count, void* context)
    {
        auto& state = *static_cast<CurlContext*>(context);
        try
        {
            (*state.sink)(data, size * count);
            return size * count;
        }
        catch (...)
        {
            state.failure = std::current_exception();
            return 0;
        }
    }

    static int Progress(void* context, std::int64_t total, std::int64_t now, std::int64_t, std::int64_t)
    {
        auto& state = *static_cast<CurlContext*>(context);
        if (state.transfer->Cancelled()) return 1;
        state.transfer->Report(std::uint64_t(std::max<std::int64_t>(now, 0)),
                               std::uint64_t(std::max<std::int64_t>(total, 0)));
        return 0;
    }

    long RunCurl(const Sink& sink)
    {
        const auto& curl = Curl();
        if (!curl.ready) throw HttpError("Cannot connect: " + curl.error);
        void* handle = curl.easy_init();
        if (!handle) throw HttpError("Cannot connect: libcurl could not start a request");
        void* headers = nullptr;
        struct Cleanup
        {
            const CurlApi& curl;
            void*& handle;
            void*& headers;
            ~Cleanup()
            {
                if (headers) curl.slist_free_all(headers);
                curl.easy_cleanup(handle);
            }
        } cleanup{curl, handle, headers};
        for (const auto& header : request_.headers)
            if (void* list = curl.slist_append(headers, header.c_str())) headers = list;
        char error[256] = ""; // CURL_ERROR_SIZE
        CurlContext context{this, &sink, nullptr};
        curl.easy_setopt(handle, kCurlUrl, request_.url.c_str());
        // HTTPS only (HTTP too with allow_http), redirects HTTPS only; libcurl
        // before 7.85 knows only the (since deprecated) protocol masks.
        if (curl.easy_setopt(handle, kCurlProtocols, request_.allow_http ? "http,https" : "https") != 0)
            curl.easy_setopt(handle, kCurlProtocolMask, request_.allow_http ? kCurlHttpAndHttps : kCurlHttpsOnly);
        if (curl.easy_setopt(handle, kCurlRedirectProtocols, "https") != 0)
            curl.easy_setopt(handle, kCurlRedirectProtocolMask, kCurlHttpsOnly);
        if (request_.method != "GET")
        {
            // POSTFIELDS makes it a POST, even with an empty body; another
            // method keeps the body and only renames the request.
            curl.easy_setopt(handle, kCurlPostFieldSize, long(request_.body.size()));
            curl.easy_setopt(handle, kCurlPostFields, request_.body.c_str());
            if (request_.method != "POST") curl.easy_setopt(handle, kCurlCustomRequest, request_.method.c_str());
        }
        curl.easy_setopt(handle, kCurlFollowLocation, 1L);
        curl.easy_setopt(handle, kCurlMaxRedirects, 10L);
        curl.easy_setopt(handle, kCurlUserAgent, request_.user_agent.c_str());
        if (headers) curl.easy_setopt(handle, kCurlHttpHeader, headers);
        curl.easy_setopt(handle, kCurlAcceptEncoding, "");
        curl.easy_setopt(handle, kCurlNoSignal, 1L);
        curl.easy_setopt(handle, kCurlConnectTimeout, long(request_.connect_timeout));
        curl.easy_setopt(handle, kCurlLowSpeedLimit, 1L);
        curl.easy_setopt(handle, kCurlLowSpeedTime, long(request_.stall_timeout));
        curl.easy_setopt(handle, kCurlErrorBuffer, error);
        curl.easy_setopt(handle, kCurlWriteFunction, &Write);
        curl.easy_setopt(handle, kCurlWriteData, &context);
        curl.easy_setopt(handle, kCurlNoProgress, 0L);
        curl.easy_setopt(handle, kCurlProgressFunction, &Progress);
        curl.easy_setopt(handle, kCurlProgressData, &context);
        Report(0, 0);
        const int result = curl.easy_perform(handle);
        if (context.failure) std::rethrow_exception(context.failure);
        if (result == kCurlAborted || Cancelled()) throw HttpError("Cancelled", true);
        if (result == kCurlTimedOut) throw HttpError("the connection timed out");
        if (result != 0 && result != kCurlWriteError)
            throw HttpError(*error ? std::string(error) : std::string(curl.easy_strerror(result)));
        if (result != 0) throw HttpError("the download could not be stored");
        long status = 0;
        curl.easy_getinfo(handle, kCurlResponseCode, &status);
        return status;
    }
#endif

    const HttpRequest& request_;
    const HttpProgress& progress_;
    bool delivered_ = false;
};

void HttpCancel::Cancel()
{
    cancelled_ = true;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) WinHttpCloseHandle(static_cast<HINTERNET>(handle_));
    handle_ = nullptr;
#endif
}

std::string HttpUnavailable()
{
#ifdef _WIN32
    return {};
#else
    const auto& curl = Curl();
    return curl.ready ? std::string() : curl.error;
#endif
}

HttpResponse HttpGet(const HttpRequest& request, const HttpProgress& progress)
{
    HttpResponse response;
    response.status = HttpTransfer(request, progress).Run([&](const char* data, std::size_t size) {
        if (response.body.size() + size > request.max_size) throw HttpError("The answer is too large");
        response.body.append(data, size);
    });
    return response;
}

long HttpDownload(const HttpRequest& request, const std::filesystem::path& file, const HttpProgress& progress)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) throw HttpError("Cannot create " + PathUtf8(file));
    long status = 0;
    try
    {
        status = HttpTransfer(request, progress).Run([&](const char* data, std::size_t size) {
            if (!stream.write(data, std::streamsize(size))) throw HttpError("Cannot write " + PathUtf8(file));
        });
        stream.close();
        if (!stream) throw HttpError("Cannot write " + PathUtf8(file));
    }
    catch (...)
    {
        stream.close();
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        throw;
    }
    if (status != 200)
    {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
    return status;
}
} // namespace mscharged::platform
