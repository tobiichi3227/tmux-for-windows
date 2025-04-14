#include <winsock2.h>
#include <windows.h>
#include <WS2tcpip.h>
#include <iostream>
#include <process.h>
#include <SDKDDKVer.h>
#include <cstdio>
#include <tchar.h>
#include <assert.h>
#include <vector>
#include <string>
#include <map>
#include <set>

#pragma comment(lib, "ws2_32.lib")

HRESULT CreatePseudoConsoleAndPipes(HPCON*, HANDLE*, HANDLE*);
HRESULT InitializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEX*, HPCON);
void __cdecl ReadBufferThreadFunc(LPVOID);
void __cdecl WriteBufferThreadFunc(LPVOID);
void __cdecl WriteConsoleThreadFunc(LPVOID);
void __cdecl PTYCleanupThreadFunc(LPVOID);
void __cdecl ClientCleanupThreadFunc(LPVOID);

struct Client;

struct Process {
    PROCESS_INFORMATION pi;
    STARTUPINFOEX si;
};

struct PTY {
    HPCON pty;
    Process process;
    HANDLE pipeIn;
    HANDLE pipeOut;
    HANDLE writeBufferThread;
    HANDLE cleanupThread;
    bool isExit = false;
    std::vector<char> buffer;
    std::set<Client*> clients;
    int sessionID;
};


struct Client {
    PTY* pty;
    SOCKET clientSocket;
    HANDLE readBufferThread;
    HANDLE writeConsoleThread;
    HANDLE cleanupThread;
    HANDLE needReadBufferCount;
    HANDLE cleanEvent;
    int i = 0;
    int clientID;
    bool connectionClosed = false;
};

static int clientID = 0;
HANDLE processExitedEvent;
bool serverClosed = false;
std::map<int, PTY*> ptys;
std::map<int, Client*> clients;

HRESULT InitializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEX* pStartupInfo, HPCON hPC)
{
    HRESULT hr{ E_UNEXPECTED };

    if (pStartupInfo)
    {
        size_t attrListSize{};

        pStartupInfo->StartupInfo.cb = sizeof(STARTUPINFOEX);

        // Get the size of the thread attribute list.
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

        // Allocate a thread attribute list of the correct size
        pStartupInfo->lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrListSize));

        // Initialize thread attribute list
        if (pStartupInfo->lpAttributeList
            && InitializeProcThreadAttributeList(pStartupInfo->lpAttributeList, 1, 0, &attrListSize))
        {
            // Set Pseudo Console attribute
            hr = UpdateProcThreadAttribute(
                pStartupInfo->lpAttributeList,
                0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                hPC,
                sizeof(HPCON),
                NULL,
                NULL)
                ? S_OK
                : HRESULT_FROM_WIN32(GetLastError());
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    return hr;
}

HRESULT CreatePseudoConsoleAndPipes(HPCON* phPC, HANDLE* phPipeIn, HANDLE* phPipeOut)
{
    HRESULT hr{ E_UNEXPECTED };
    HANDLE hPipePTYIn{ INVALID_HANDLE_VALUE };
    HANDLE hPipePTYOut{ INVALID_HANDLE_VALUE };

    // Create the pipes to which the ConPTY will connect
    if (CreatePipe(&hPipePTYIn, phPipeOut, NULL, 0) &&
        CreatePipe(phPipeIn, &hPipePTYOut, NULL, 0))
    {
        // Determine required size of Pseudo Console
        COORD consoleSize{};
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        HANDLE hConsole{ GetStdHandle(STD_OUTPUT_HANDLE) };
        if (GetConsoleScreenBufferInfo(hConsole, &csbi))
        {
            consoleSize.X = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            consoleSize.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }

        // Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
        hr = CreatePseudoConsole(consoleSize, hPipePTYIn, hPipePTYOut, 0, phPC);

        // Note: We can close the handles to the PTY-end of the pipes here
        // because the handles are dup'ed into the ConHost and will be released
        // when the ConPTY is destroyed.
        if (INVALID_HANDLE_VALUE != hPipePTYOut) CloseHandle(hPipePTYOut);
        if (INVALID_HANDLE_VALUE != hPipePTYIn) CloseHandle(hPipePTYIn);
    }

    return hr;
}

void __cdecl PTYCleanupThreadFunc(LPVOID pPty) {
    PTY* pty = static_cast<PTY*>(pPty);
    WaitForSingleObject(pty->process.pi.hThread, INFINITE);

    printf("pty %d: cleanup\n", pty->sessionID);
    CloseHandle(pty->process.pi.hThread);
    CloseHandle(pty->process.pi.hProcess);
    CloseHandle(pty->writeBufferThread);
    for (auto client : pty->clients) {
        client->connectionClosed = true;
        SetEvent(client->cleanEvent);
    }
    DeleteProcThreadAttributeList(pty->process.si.lpAttributeList);
    free(pty->process.si.lpAttributeList);
    ClosePseudoConsole(pty->pty);
    if (pty->pipeIn != INVALID_HANDLE_VALUE) CloseHandle(pty->pipeIn);
    if (pty->pipeOut != INVALID_HANDLE_VALUE) CloseHandle(pty->pipeOut);
    ptys.erase(ptys.find(pty->sessionID));
    SetEvent(processExitedEvent);
    CloseHandle(pty->cleanupThread);
    delete pty;
}

void __cdecl ClientCleanupThreadFunc(LPVOID pClient) {
    Client* client = static_cast<Client*>(pClient);
    WaitForSingleObject(client->cleanEvent, INFINITE);
    printf("client %d: cleanup\n", client->clientID);
    client->connectionClosed = true;
    closesocket(client->clientSocket);
    CloseHandle(client->readBufferThread);
    CloseHandle(client->writeConsoleThread);
    CloseHandle(client->needReadBufferCount);
    CloseHandle(client->cleanEvent);
    clients.erase(clients.find(client->clientID));
    client->pty->clients.erase(client->pty->clients.find(client));
    CloseHandle(client->cleanupThread);
    delete client;
}

PTY* CreatePTY() {
    HRESULT hr{ S_OK };
    PTY* pty = new PTY();

	HPCON hPC{ INVALID_HANDLE_VALUE };

	//  Create the Pseudo Console and pipes to it
	HANDLE hPipeIn{ INVALID_HANDLE_VALUE };
	HANDLE hPipeOut{ INVALID_HANDLE_VALUE };
	hr = CreatePseudoConsoleAndPipes(&hPC, &hPipeIn, &hPipeOut);
	if (S_OK == hr) {
		pty->pipeIn = hPipeIn;
		pty->pipeOut = hPipeOut;
		pty->buffer.reserve(1024 * 64);
		pty->writeBufferThread = { reinterpret_cast<HANDLE>(_beginthread(WriteBufferThreadFunc, 0, (LPVOID)pty)) };
        pty->pty = hPC;

		STARTUPINFOEX startupInfo{};
		if (S_OK == InitializeStartupInfoAttachedToPseudoConsole(&startupInfo, hPC))
		{
			// Launch ping to emit some text back via the pipe
			PROCESS_INFORMATION piClient{};
            PROCESS_TERMINATE;
			hr = CreateProcess(
				NULL,                           // No module name - use Command Line
				,                      // Command Line
				NULL,                           // Process handle not inheritable
				NULL,                           // Thread handle not inheritable
				TRUE,                          // Inherit handles
				EXTENDED_STARTUPINFO_PRESENT,   // Creation flags
				NULL,                           // Use parent's environment block
				NULL,                           // Use parent's starting directory 
				&startupInfo.StartupInfo,       // Pointer to STARTUPINFO
				&piClient)                      // Pointer to PROCESS_INFORMATION
				? S_OK
				: GetLastError();

			if (S_OK != hr)
			{
				CloseHandle(piClient.hThread);
				CloseHandle(piClient.hProcess);
				DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
				free(startupInfo.lpAttributeList);
				delete pty;
				ClosePseudoConsole(hPC);
				if (INVALID_HANDLE_VALUE != hPipeOut) CloseHandle(hPipeOut);
				if (INVALID_HANDLE_VALUE != hPipeIn) CloseHandle(hPipeIn);
				return nullptr;
            } else {
                pty->process = Process();
                pty->process.pi = piClient;
                pty->process.si = startupInfo;
                pty->cleanupThread = { reinterpret_cast<HANDLE>(_beginthread(PTYCleanupThreadFunc, 0, (LPVOID)pty)) };
				return pty;
            }
		}
	}
    return nullptr;
}

BOOL WINAPI CtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        for (auto [id, pty] : ptys) {
            
        }
        return TRUE;
    }
}

void __cdecl AcceptThreadFunc(LPVOID pServerSocket) {
    SOCKET serverSocket = (SOCKET)pServerSocket;
    SOCKET clientSocket;
    struct sockaddr_in clientAddr;
    int clientAddrSize = sizeof(clientAddr);

    while (!serverClosed) {
        clientSocket = accept(serverSocket, static<struct sockaddr*>(&clientAddr), &clientAddrSize);
        if (serverClosed) break;
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "Accept failed with error: " << WSAGetLastError() << std::endl;
            continue;
        }

        std::cout << "Client connected!" << std::endl;

        char buffer[128];
        char *endptr;
        long num;
        int readBytes;
        readBytes = recv(clientSocket, buffer, 127, 0);
        if (readBytes <= 0) continue;
        buffer[readBytes] = '\0';
        num = strtol(buffer, &endptr, 10);
        if (*endptr != '\0') continue;
        int sessionId = (int)num;

        PTY* pty = nullptr;
        if (ptys.find(sessionId) == ptys.end()) {
			pty = CreatePTY();
            if (pty == nullptr) continue;
            pty->sessionID = sessionId;
            ptys[pty->sessionID] = pty;
            std::cout << "Session" << sessionId << ": Not Exist" << std::endl;
        } else {
            std::cout << "Session" << sessionId << ": Exist" << std::endl;
        }
		pty = ptys[sessionId];

        std::cout << "Session" << sessionId << ": OK" << std::endl;

        sprintf_s(buffer, "ok");
        buffer[3] = '\0';
        send(clientSocket, buffer, 2, 0);

        Client* client = new Client();
        client->clientID = clientID++;
        clients[client->clientID] = client;
        pty->clients.insert(client);

        client->pty = pty;
        client->clientSocket = clientSocket;
        client->needReadBufferCount = CreateSemaphore(NULL, 0, 1024, NULL);
        client->cleanEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        client->readBufferThread = { reinterpret_cast<HANDLE>(_beginthread(ReadBufferThreadFunc, 0, (LPVOID)client)) };
        client->writeConsoleThread = { reinterpret_cast<HANDLE>(_beginthread(WriteConsoleThreadFunc, 0, (LPVOID)client)) };
        client->cleanupThread = { reinterpret_cast<HANDLE>(_beginthread(ClientCleanupThreadFunc, 0, (LPVOID)client)) };

        printf("New client %d create\n", client->clientID);
    }
}

void StartEchoServer(const char* ipAddress = "127.0.0.1", int port = 12345) {
    WSADATA wsaData;
    SOCKET serverSocket;
    struct sockaddr_in serverAddr;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed with error: " << WSAGetLastError() << std::endl;
        return;
    }

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed with error: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return;
    }

    ZeroMemory(&serverAddr, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, ipAddress, &serverAddr.sin_addr) <= 0) {
        std::cerr << "Invalid address/Address not supported" << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    if (bind(serverSocket, static_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed with error: " << WSAGetLastError() << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed with error: " << WSAGetLastError() << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    std::cout << "Echo server started on " << ipAddress << ":" << port << std::endl;
    HANDLE acceptThread{ reinterpret_cast<HANDLE>(_beginthread(AcceptThreadFunc, 0, (LPVOID)serverSocket)) };

    processExitedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    while (true) {
        WaitForSingleObject(processExitedEvent, INFINITE);

        if (ptys.size() == 0) {
            serverClosed = true;
            break;
        }
        ResetEvent(processExitedEvent);
    }

    std::cout << "Echo server closed" << std::endl;
    CloseHandle(acceptThread);
    CloseHandle(processExitedEvent);
    closesocket(serverSocket);
    WSACleanup();
}

void __cdecl WriteBufferThreadFunc(LPVOID pPty) {
    PTY* pty = static_cast<PTY*>(pPty);
    const DWORD BUFF_SIZE{ 512 };
    char szBuffer[BUFF_SIZE]{};

    DWORD dwBytesWritten{};
    DWORD dwBytesRead{};
    BOOL fRead{ FALSE };
    do {
        fRead = ReadFile(pty->pipeIn, szBuffer, BUFF_SIZE - 1, &dwBytesRead, NULL);
        if (!fRead) break;

        szBuffer[dwBytesRead] = '\0';
        if (pty->buffer.capacity() <= pty->buffer.size() + dwBytesRead) {
            pty->buffer.reserve(pty->buffer.capacity() + BUFF_SIZE * 64);
        }
        //pty->buffer.insert(pty->buffer.end(), szBuffer, szBuffer + dwBytesRead);
        std::copy(szBuffer, szBuffer + dwBytesRead, std::back_inserter(pty->buffer)); // TODO: bad performance
        for (auto client : pty->clients) {
            ReleaseSemaphore(client->needReadBufferCount, 1, NULL);
        }

    } while (!pty->isExit && dwBytesRead >= 0);
}

void __cdecl ReadBufferThreadFunc(LPVOID pClient) {
    Client* client = static_cast<Client*>(pClient);
    PTY* pty = client->pty;
    int dwBytesWritten{};
    

    int i = client->i;
	for (; i + 512 < (int)pty->buffer.size(); i += 512) {
		send(client->clientSocket, pty->buffer.data() + i, 512, 0);
	}
    send(client->clientSocket, pty->buffer.data() + i, (int)pty->buffer.size() - i, 0);
    //send(client->clientSocket, pty->buffer.data(), (int)pty->buffer.size(), 0);
    client->i = (int)pty->buffer.size();

    do {
        WaitForSingleObject(client->needReadBufferCount, INFINITE);
        if (client->connectionClosed) break;
        if (client->i == pty->buffer.size()) continue;
        // TODO: chunk send
        i = client->i;
        for (; i + 512 < (int)pty->buffer.size(); i += 512) {
            send(client->clientSocket, pty->buffer.data() + i, 512, 0);
        }
        send(client->clientSocket, pty->buffer.data() + i, (int)pty->buffer.size() - i, 0);
        //dwBytesWritten = send(client->clientSocket, pty->buffer.data() + client->i, (int)pty->buffer.size() - client->i, 0);
        client->i = (int)pty->buffer.size();

    } while (!client->connectionClosed);
}

void __cdecl WriteConsoleThreadFunc(LPVOID pClient) {
    Client* client = static_cast<Client*>(pClient);
    PTY* pty = client->pty;

    const DWORD BUFF_SIZE{ 512 };
    char szBuffer[BUFF_SIZE]{};

    DWORD dwBytesWritten{};
    int dwBytesRead{};
    BOOL fRead{ FALSE };

    do {
        dwBytesRead = recv(client->clientSocket, szBuffer, BUFF_SIZE - 1, 0);
        if (dwBytesRead <= 0) {
            client->connectionClosed = true;
            SetEvent(client->cleanEvent);
            break;
        }
        szBuffer[dwBytesRead] = '\0';
        WriteFile(pty->pipeOut, szBuffer, dwBytesRead, &dwBytesWritten, NULL);
    } while (!client->connectionClosed && dwBytesRead > 0);

}

int main(int argc, char** argv) {
    StartEchoServer();
    return 0;
}
