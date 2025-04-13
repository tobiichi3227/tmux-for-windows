// EchoCon.cpp : Entry point for the EchoCon Pseudo-Console sample application.
// Copyright © 2018, Microsoft

// TODO: support window resize event
#include <winsock2.h>
#include <windows.h>
#include <WS2tcpip.h>
#include <iostream>
#include <process.h>
#include <SDKDDKVer.h>
#include <cstdio>
#include <tchar.h>
#include <assert.h>

#pragma comment(lib, "ws2_32.lib")

// Forward declarations
void __cdecl InputConsoleListener(LPVOID);
void __cdecl OutputConsoleListener(LPVOID);

HANDLE exitEvent;

void StartClient(const char* sessionId, const char* ipAddress = "127.0.0.1", int port = 12345) {

    HANDLE hConsoleInput = { GetStdHandle(STD_INPUT_HANDLE) };
    HANDLE hConsoleOutput = { GetStdHandle(STD_OUTPUT_HANDLE) };

    // Enable Console VT Processing
    DWORD consoleMode{};
    GetConsoleMode(hConsoleInput, &consoleMode);
    consoleMode &= ~ENABLE_ECHO_INPUT;
    consoleMode &= ~ENABLE_PROCESSED_INPUT;
    SetConsoleMode(hConsoleInput, consoleMode | ENABLE_WINDOW_INPUT);
    GetConsoleMode(hConsoleOutput, &consoleMode);
    SetConsoleMode(hConsoleOutput, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

	SetConsoleCP(CP_UTF8);
	SetConsoleOutputCP(CP_UTF8);

    WSADATA wsaData;
    SOCKET clientSocket;
    struct sockaddr_in serverAddr;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return;
    }

    clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    ZeroMemory(&serverAddr, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, ipAddress, &serverAddr.sin_addr) <= 0) {
        closesocket(clientSocket);
        WSACleanup();
        return;
    }

    if (connect(clientSocket, static_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(clientSocket);
        WSACleanup();
        return;
    }

    char buffer[128];
    memset(buffer, '\0', sizeof(buffer));
    sprintf_s(buffer, "%s", sessionId);
    send(clientSocket, buffer, (int)strlen(buffer), 0);
    memset(buffer, '\0', sizeof(buffer));
    recv(clientSocket, buffer, 2, 0);
    if (strncmp(buffer, "ok", 2) != 0) {
        exit(-1);
    }

    exitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	HANDLE hPipeListenerThread{ reinterpret_cast<HANDLE>(_beginthread(InputConsoleListener, 0, (LPVOID)clientSocket)) };
	HANDLE hPipeListenerThread2{ reinterpret_cast<HANDLE>(_beginthread(OutputConsoleListener, 0, (LPVOID)clientSocket)) };
    WaitForSingleObject(exitEvent, INFINITE);

    CloseHandle(hPipeListenerThread);
    CloseHandle(hPipeListenerThread2);
    CloseHandle(exitEvent);
    closesocket(clientSocket);
    WSACleanup();
}

void __cdecl InputConsoleListener(LPVOID socket) {
    SOCKET sSocket = (SOCKET)socket;
    HANDLE hConsole{ GetStdHandle(STD_OUTPUT_HANDLE) };

    const DWORD BUFF_SIZE{ 512 };
    char szBuffer[BUFF_SIZE]{};

    DWORD dwBytesWritten{};
    int dwBytesRead{};

    do {
        dwBytesRead = recv(sSocket, szBuffer, BUFF_SIZE - 1, 0);
        if (dwBytesRead <= 0) {
            SetEvent(exitEvent);
            break;
        }

        szBuffer[dwBytesRead] = '\0';
        WriteFile(hConsole, szBuffer, dwBytesRead, &dwBytesWritten, NULL);
    } while (dwBytesRead > 0);
}
void __cdecl OutputConsoleListener(LPVOID socket) {
    SOCKET sSocket = (SOCKET)socket;
    HANDLE hConsole{ GetStdHandle(STD_INPUT_HANDLE) };

    INPUT_RECORD eventRecord;
    DWORD res{};

    const DWORD BUFF_SIZE{ 512 };
    char szBuffer[BUFF_SIZE]{};

    wchar_t unicodeBuffer[3]{};
    wchar_t utf16_high_surrogate;

    DWORD dwBytesWritten{};
    DWORD dwBytesRead{};
    BOOL fRead{ FALSE };

    do {
        fRead = ReadConsoleInputW(hConsole, &eventRecord, 1, &res);
        if (eventRecord.EventType == KEY_EVENT) {
            const KEY_EVENT_RECORD& keyEvent = eventRecord.Event.KeyEvent;
            if (!keyEvent.bKeyDown && !(((keyEvent.dwControlKeyState & LEFT_ALT_PRESSED) ||
                keyEvent.wVirtualKeyCode == VK_MENU) && keyEvent.uChar.UnicodeChar != 0)) {
                continue;
            }

            if ((keyEvent.dwControlKeyState & LEFT_ALT_PRESSED) &&
                !(keyEvent.dwControlKeyState & ENHANCED_KEY) &&
                (keyEvent.wVirtualKeyCode == VK_INSERT ||
                    keyEvent.wVirtualKeyCode == VK_END ||
                    keyEvent.wVirtualKeyCode == VK_DOWN ||
                    keyEvent.wVirtualKeyCode == VK_NEXT ||
                    keyEvent.wVirtualKeyCode == VK_LEFT ||
                    keyEvent.wVirtualKeyCode == VK_CLEAR ||
                    keyEvent.wVirtualKeyCode == VK_RIGHT ||
                    keyEvent.wVirtualKeyCode == VK_HOME ||
                    keyEvent.wVirtualKeyCode == VK_UP ||
                    keyEvent.wVirtualKeyCode == VK_PRIOR ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD0 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD1 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD2 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD3 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD4 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD5 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD6 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD7 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD8 ||
                    keyEvent.wVirtualKeyCode == VK_NUMPAD9)) {
                continue;
            }

            if (keyEvent.bKeyDown) {
                if (keyEvent.wVirtualKeyCode == VK_SHIFT) continue;
                if (keyEvent.wVirtualKeyCode == VK_CAPITAL) continue;
                if (keyEvent.wVirtualKeyCode == VK_CONTROL) continue;
                if (keyEvent.wVirtualKeyCode == VK_LCONTROL) continue;
                if (keyEvent.wVirtualKeyCode == VK_RCONTROL) continue;
                if (keyEvent.wVirtualKeyCode == VK_MENU) continue;
                if (keyEvent.wVirtualKeyCode == 'B' && (keyEvent.dwControlKeyState & LEFT_CTRL_PRESSED)) {
                    SetEvent(exitEvent);
                    break;
                } else {
                    if (keyEvent.uChar.UnicodeChar != 0) {
                        if (keyEvent.uChar.UnicodeChar >= 0xD800 && keyEvent.uChar.UnicodeChar < 0xDC00) {
                            utf16_high_surrogate = keyEvent.uChar.UnicodeChar;
                            continue;
                        }

						char buffer[6]{};
                        DWORD dBufSize{};
                        if (keyEvent.uChar.UnicodeChar >= 0xDC00 && keyEvent.uChar.UnicodeChar < 0xE000) {
                            wchar_t utf16_buffer[2] = { utf16_high_surrogate, keyEvent.uChar.UnicodeChar };
                            dBufSize = WideCharToMultiByte(CP_UTF8, 0, utf16_buffer, 2, buffer, sizeof(buffer), NULL, NULL);
                        } else {
                            dBufSize = WideCharToMultiByte(CP_UTF8, 0, &keyEvent.uChar.UnicodeChar, 1, buffer, sizeof(buffer), NULL, NULL);
                        }

                        send(sSocket, buffer, (int)dBufSize, 0);
                    }
				}
            } 
        } else if (eventRecord.EventType == WINDOW_BUFFER_SIZE_EVENT) {
 //           //const WINDOW_BUFFER_SIZE_RECORD& resizeEvent = eventRecord.Event.WindowBufferSizeEvent;

 //           //ResizePseudoConsole(params->hPC, resizeEvent.dwSize);
        }
    } while (true);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cout << "Please input session id" << std::endl;
        return -1;
    }
    StartClient(argv[1]);
    return 0;
}
