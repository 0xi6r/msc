#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

#define SERVER_PORT "8080"
#define BACKLOG 10
#define BUFFER_SIZE 4096

static int send_all(SOCKET client_socket, const char *buffer, int length)
{
    int total_sent = 0;

    while (total_sent < length) {
        int sent = send(client_socket, buffer + total_sent, length - total_sent, 0);
        if (sent == SOCKET_ERROR) {
            return 0;
        }
        total_sent += sent;
    }

    return 1;
}

static void handle_client(SOCKET client_socket)
{
    char request[BUFFER_SIZE];
    const char *body =
        "<html>\r\n"
        "<head><title>Simple C HTTP Server</title></head>\r\n"
        "<body>\r\n"
        "<h1>Simple C HTTP Server</h1>\r\n"
        "<p>The server is running on Windows.</p>\r\n"
        "</body>\r\n"
        "</html>\r\n";
    char response[BUFFER_SIZE];
    int received;
    int body_length = (int)strlen(body);
    int response_length;

    received = recv(client_socket, request, sizeof(request) - 1, 0);
    if (received == SOCKET_ERROR || received == 0) {
        return;
    }

    request[received] = '\0';
    printf("Request:\n%s\n", request);

    response_length = _snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        body_length,
        body
    );

    if (response_length < 0 || response_length >= (int)sizeof(response)) {
        const char *error_response =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        send_all(client_socket, error_response, (int)strlen(error_response));
        return;
    }

    send_all(client_socket, response, response_length);
}

int main(void)
{
    WSADATA wsa_data;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    SOCKET listen_socket = INVALID_SOCKET;
    int status;

    status = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (status != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", status);
        return 1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    status = getaddrinfo(NULL, SERVER_PORT, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo failed: %d\n", status);
        WSACleanup();
        return 1;
    }

    listen_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    status = bind(listen_socket, result->ai_addr, (int)result->ai_addrlen);
    freeaddrinfo(result);
    if (status == SOCKET_ERROR) {
        fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
        closesocket(listen_socket);
        WSACleanup();
        return 1;
    }

    status = listen(listen_socket, BACKLOG);
    if (status == SOCKET_ERROR) {
        fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
        closesocket(listen_socket);
        WSACleanup();
        return 1;
    }

    printf("HTTP server listening on http://localhost:%s\n", SERVER_PORT);

    for (;;) {
        SOCKET client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) {
            fprintf(stderr, "accept failed: %d\n", WSAGetLastError());
            break;
        }

        handle_client(client_socket);
        closesocket(client_socket);
    }

    closesocket(listen_socket);
    WSACleanup();
    return 0;
}
