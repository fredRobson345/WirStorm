// CTMP Proxy Server - C++ Implementation
//
// Build:
//   g++ -std=c++17 -o ctmp_proxy ctmp_proxy.cpp
//
// Run:
//   ./ctmp_proxy

#include <iostream>
#include <vector>
#include <list>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>

constexpr int SOURCE_PORT = 33333;
constexpr int DEST_PORT = 44444;
constexpr int MAX_CLIENTS = 100;
constexpr int HEADER_SIZE = 8;
constexpr uint8_t MAGIC_BYTE = 0xCC;

struct DestClient {
    int fd;
};

std::list<DestClient> destClients;

void addDestClient(int fd) {
    destClients.push_back({fd});
}

void removeDestClient(int fd) {
    destClients.remove_if([fd](const DestClient &client) {
        close(client.fd);
        return client.fd == fd;
    });
}

void broadcastToDestClients(const std::vector<uint8_t> &buffer) {
    for (auto it = destClients.begin(); it != destClients.end();) {
        ssize_t sent = send(it->fd, buffer.data(), buffer.size(), 0);
        if (sent <= 0) {
            int fd = it->fd;
            it = destClients.erase(it);
            close(fd);
        } else {
            ++it;
        }
    }
}

int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int setupListener(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(sock, 10) < 0) {
        perror("bind/listen");
        exit(1);
    }

    setNonBlocking(sock);
    return sock;
}

int readFull(int sock, uint8_t *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

void handleSource(int sourceFd) {
    uint8_t header[HEADER_SIZE];

    while (true) {
        if (readFull(sourceFd, header, HEADER_SIZE) < 0) break;

        if (header[0] != MAGIC_BYTE || header[1] != 0x00 ||
            header[4] != 0x00 || header[5] != 0x00 || header[6] != 0x00 || header[7] != 0x00) {
            std::cerr << "Invalid header, dropping message\n";
            break;
        }

        uint16_t len;
        std::memcpy(&len, &header[2], 2);
        len = ntohs(len);

        std::vector<uint8_t> payload(HEADER_SIZE + len);
        std::memcpy(payload.data(), header, HEADER_SIZE);

        if (readFull(sourceFd, payload.data() + HEADER_SIZE, len) < 0) break;

        broadcastToDestClients(payload);
    }

    close(sourceFd);
}

int main() {
    int srcListener = setupListener(SOURCE_PORT);
    int dstListener = setupListener(DEST_PORT);
    int sourceFd = -1;

    pollfd fds[MAX_CLIENTS + 2];

    while (true) {
        int nfds = 0;
        fds[nfds++] = {srcListener, POLLIN};
        fds[nfds++] = {dstListener, POLLIN};

        for (const auto &client : destClients) {
            if (nfds >= MAX_CLIENTS + 2) break;
            fds[nfds++] = {client.fd, POLLOUT};
        }

        poll(fds, nfds, 100);

        if (fds[0].revents & POLLIN && sourceFd < 0) {
            sourceFd = accept(srcListener, nullptr, nullptr);
            if (sourceFd >= 0) {
                std::cout << "Source client connected\n";
                if (!fork()) {
                    close(srcListener);
                    close(dstListener);
                    handleSource(sourceFd);
                    exit(0);
                }
                close(sourceFd);
                sourceFd = -1;
            }
        }

        if (fds[1].revents & POLLIN) {
            int newFd = accept(dstListener, nullptr, nullptr);
            if (newFd >= 0) {
                setNonBlocking(newFd);
                addDestClient(newFd);
                std::cout << "New destination client connected\n";
            }
        }
    }

    return 0;
}
