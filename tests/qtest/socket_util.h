/*
 * Socket Utilities for QTests
 *
 * Copyright (C) 2024 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef SOCKET_UTIL_H
#define SOCKET_UTIL_H

/**
 * socket_util_open_socket:
 * @sock: Socket pointer.
 * @recv_timeout: Socket receive timeout.
 * Opens a local TCP socket.
 */
static inline in_port_t socket_util_open_socket(int *sock,
                                                struct timeval *recv_timeout,
                                                struct timeval *send_timeout)
{
    struct sockaddr_in myaddr;
    socklen_t addrlen;

    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    myaddr.sin_port = 0;

    *sock = socket(AF_INET, SOCK_STREAM, 0);
    g_assert(*sock != -1);
    g_assert(bind(*sock, (struct sockaddr *)&myaddr, sizeof(myaddr)) != -1);

    if (recv_timeout != NULL) {
        setsockopt(*sock, SOL_SOCKET, SO_RCVTIMEO, recv_timeout,
                   sizeof(struct timeval));
    }

    if (send_timeout != NULL) {
        setsockopt(*sock, SOL_SOCKET, SO_SNDTIMEO, send_timeout,
                   sizeof(struct timeval));
    }

    addrlen = sizeof(myaddr);
    g_assert(getsockname(*sock, (struct sockaddr *)&myaddr, &addrlen) != -1);
    g_assert(listen(*sock, 1) != -1);

    return ntohs(myaddr.sin_port);
}

/**
 * socket_util_setup_fd:
 * @sock: Socket.
 * Create a filedescriptor connected to the socket.
 */
static inline int socket_util_setup_fd(int sock)
{
    int fd;
    fd_set readfds;
    fd_set writefds;

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(sock, &readfds);
    FD_SET(sock, &writefds);
    g_assert(select(sock + 1, &readfds, &writefds, NULL, NULL) == 1);

    fd = accept(sock, NULL, 0);
    g_assert(fd >= 0);

    return fd;
}

#endif /* SOCKET_UTIL_H */
