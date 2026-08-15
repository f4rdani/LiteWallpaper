#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace litewp {

// Pipe name: \\.\pipe\LiteWallpaper
constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\LiteWallpaper";

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
    void* m_pipe_handle = nullptr; // Server pipe handle (HANDLE)
    
    void ServerLoop();
};

// Client-side IPC helper (used by Settings UI)
class IpcClient {
public:
    IpcClient();
    ~IpcClient();

    // Connect to daemon pipe
    bool Connect();
    void Disconnect();
    bool IsConnected() const;
    
    // Send request, wait for response (synchronous)
    std::string SendRequest(const std::string& request_json);

private:
    void* m_pipe_handle = nullptr; // Client pipe handle (HANDLE)
};

} // namespace litewp
