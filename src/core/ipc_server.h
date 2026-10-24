#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace miniwp {

// Pipe name: \\.\pipe\MiniWallpaper
constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\MiniWallpaper";

// Callback receives incoming JSON request string and returns JSON response string
using IpcCallback = std::function<std::string(const std::string& request_json)>;

class IpcServer {
public:
    IpcServer();
    ~IpcServer();

    // Start listening on background thread
    void Start(IpcCallback callback);
    void Stop();
    bool IsRunning() const;

private:
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    IpcCallback m_callback;

    void ServerLoop();
};

// Client-side IPC helper (used by CLI / secondary instance forwarding)
class IpcClient {
public:
    IpcClient();
    ~IpcClient();

    bool Connect();
    void Disconnect();
    bool IsConnected() const;

    // Send request, wait for response (synchronous)
    std::string SendRequest(const std::string& request_json);

private:
    void* m_pipe_handle = nullptr;
};

} // namespace miniwp
