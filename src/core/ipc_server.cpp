#include "ipc_server.h"
#include <windows.h>
#include <vector>

namespace miniwp {

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

    // Unblock ConnectNamedPipe by opening a short dummy client connection
    HANDLE hPipe = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
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
    std::vector<char> buffer(8192, 0);

    while (m_running.load()) {
        HANDLE hPipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            8192,
            8192,
            0,
            nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && m_running.load()) {
            DWORD bytesRead = 0;
            BOOL readSuccess = ReadFile(hPipe, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &bytesRead, nullptr);

            if (readSuccess && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::string request(buffer.data(), bytesRead);

                std::string response = "{\"ok\":false,\"error\":\"no callback\"}";
                if (m_callback) {
                    response = m_callback(request);
                }

                DWORD bytesWritten = 0;
                WriteFile(hPipe, response.c_str(), static_cast<DWORD>(response.size()), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

IpcClient::IpcClient() = default;

IpcClient::~IpcClient() {
    Disconnect();
}

bool IpcClient::Connect() {
    if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
        return true;
    }

    if (!WaitNamedPipeW(PIPE_NAME, 1000)) {
        return false;
    }

    HANDLE hPipe = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    m_pipe_handle = hPipe;
    return true;
}

void IpcClient::Disconnect() {
    if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(m_pipe_handle));
        m_pipe_handle = nullptr;
    }
}

bool IpcClient::IsConnected() const {
    return m_pipe_handle != nullptr && m_pipe_handle != INVALID_HANDLE_VALUE;
}

std::string IpcClient::SendRequest(const std::string& request_json) {
    if (!Connect()) {
        return "";
    }

    HANDLE hPipe = static_cast<HANDLE>(m_pipe_handle);
    DWORD bytesWritten = 0;
    BOOL writeSuccess = WriteFile(hPipe, request_json.c_str(), static_cast<DWORD>(request_json.size()), &bytesWritten, nullptr);
    if (!writeSuccess) {
        Disconnect();
        return "";
    }

    char buffer[8192] = {};
    DWORD bytesRead = 0;
    BOOL readSuccess = ReadFile(hPipe, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &bytesRead, nullptr);
    Disconnect();
    if (!readSuccess || bytesRead == 0) {
        return "";
    }

    return std::string(buffer, bytesRead);
}

} // namespace miniwp
