#include "ipc_server.h"
#include <windows.h>
#include <vector>

namespace litewp {

IpcServer::IpcServer() = default;

IpcServer::~IpcServer() {
    Stop();
}

void IpcServer::Start(IpcCallback callback) {
    if (m_running.load()) return;
    m_callback = std::move(callback);
    m_running.store(true);
    m_thread = std::thread(&IpcServer::ServerLoop, this);
}

void IpcServer::Stop() {
    if (!m_running.load()) return;
    m_running.store(false);

    // Unblock ConnectNamedPipe by opening a client connection
    HANDLE hPipe = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );
    if (hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool IpcServer::IsRunning() const {
    return m_running.load();
}

void IpcServer::ServerLoop() {
    while (m_running.load()) {
        HANDLE hPipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,              // Max 1 instance
            8192,           // Output buffer size
            8192,           // Input buffer size
            0,              // Default timeout
            NULL            // Security attributes
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }

        m_pipe_handle = hPipe;

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && m_running.load()) {
            std::vector<char> buffer(8192, 0);
            DWORD bytesRead = 0;
            BOOL readSuccess = ReadFile(hPipe, buffer.data(), (DWORD)buffer.size() - 1, &bytesRead, NULL);
            
            if (readSuccess && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::string request(buffer.data(), bytesRead);
                
                std::string response = "{\"ok\":false,\"error\":\"no callback\"}";
                if (m_callback) {
                    response = m_callback(request);
                }

                DWORD bytesWritten = 0;
                WriteFile(hPipe, response.c_str(), (DWORD)response.size(), &bytesWritten, NULL);
                FlushFileBuffers(hPipe);
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        m_pipe_handle = nullptr;
    }
}

// === Client Implementation ===

IpcClient::IpcClient() = default;

IpcClient::~IpcClient() {
    Disconnect();
}

bool IpcClient::Connect() {
    if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
        return true;
    }

    // Check if pipe exists, wait up to 1 second
    if (!WaitNamedPipeW(PIPE_NAME, 1000)) {
        return false;
    }

    HANDLE hPipe = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    m_pipe_handle = hPipe;
    return true;
}

void IpcClient::Disconnect() {
    if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle((HANDLE)m_pipe_handle);
        m_pipe_handle = nullptr;
    }
}

bool IpcClient::IsConnected() const {
    return m_pipe_handle != nullptr && m_pipe_handle != INVALID_HANDLE_VALUE;
}

std::string IpcClient::SendRequest(const std::string& request_json) {
    if (!Connect()) {
        return "{\"ok\":false,\"error\":\"failed to connect to daemon\"}";
    }

    HANDLE hPipe = (HANDLE)m_pipe_handle;
    DWORD bytesWritten = 0;
    BOOL writeSuccess = WriteFile(hPipe, request_json.c_str(), (DWORD)request_json.size(), &bytesWritten, NULL);
    if (!writeSuccess) {
        Disconnect();
        return "{\"ok\":false,\"error\":\"failed to write to pipe\"}";
    }

    std::vector<char> buffer(8192, 0);
    DWORD bytesRead = 0;
    BOOL readSuccess = ReadFile(hPipe, buffer.data(), (DWORD)buffer.size() - 1, &bytesRead, NULL);
    if (!readSuccess || bytesRead == 0) {
        Disconnect();
        return "{\"ok\":false,\"error\":\"failed to read from pipe\"}";
    }

    buffer[bytesRead] = '\0';
    Disconnect(); // Close connection per request
    return std::string(buffer.data(), bytesRead);
}

} // namespace litewp
