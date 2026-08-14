/*
 * QTests for Remote PCI Device
 *
 * Copyright (c) 2022 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_regs.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "qemu/bitops.h"

#include <sys/socket.h>

#define QEMU_CMD_CHR                                                           \
    " -chardev socket,id=pci0,host=localhost,port=%d,reconnect=10"

#define RESP_CODE_OK                0
#define RESP_CODE_MASK              BIT(7)

#define TEST_VENDOR_ID              0x1234
#define TEST_DEVICE_ID              0xabcd
#define TEST_SUBSYSTEM_VENDOR_ID    0x7823
#define TEST_SUBSYSTEM_DEVICE_ID    0x4d21
#define TEST_CLASS_REV              0x02800405
#define TEST_BAR_SIZE               1024
#define TEST_BAR_NO                 5
#define TEST_OFFSET                 1000
#define TEST_DATA                   0x1a2b3c4d
#define TEST_NUM_MSIX_VECTORS       32

#define TEST_MSIX_TABLE_BAR         2
#define TEST_MSIX_PBA_BAR           1
#define TEST_MSIX_VECTOR            13

#define CODE_READ_DATA              0x01
#define CODE_WRITE_DATA             0x02
#define CODE_MSIX                   0x05
#define CODE_READ_CONFIG            0x06
#define CODE_WRITE_CONFIG           0x07

typedef struct QRemotePCI {
    QOSGraphObject obj;
    QPCIDevice dev;
} QRemotePCI;

static in_port_t open_socket(int *sock)
{
    struct sockaddr_in myaddr;
    socklen_t addrlen;

    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    myaddr.sin_port = 0;

    *sock = socket(AF_INET, SOCK_STREAM, 0);
    g_assert(*sock != -1);
    g_assert(bind(*sock, (struct sockaddr *)&myaddr, sizeof(myaddr)) != -1);

    addrlen = sizeof(myaddr);
    g_assert(getsockname(*sock, (struct sockaddr *)&myaddr, &addrlen) != -1);
    g_assert(listen(*sock, 1) != -1);

    return ntohs(myaddr.sin_port);
}

static int setup_fd(int sock)
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

static void remote_pci_respond_read(int fd, uint8_t code, uint32_t data,
                                    int size)
{
    ssize_t rv;

    code |= RESP_CODE_MASK;
    rv = write(fd, &code, sizeof(uint8_t));
    g_assert_cmpint(rv, ==, sizeof(uint8_t));
    rv = write(fd, &data, size);
    g_assert_cmpint(rv, ==, size);
}

static void remote_pci_respond_write(int fd, uint8_t code)
{
    ssize_t rv;

    code |= RESP_CODE_MASK;
    rv = write(fd, &code, sizeof(uint8_t));
    g_assert_cmpint(rv, ==, sizeof(uint8_t));
}

typedef struct {
    uint8_t code;
    uint64_t offset;
    uint8_t size;
} __attribute__ ((__packed__)) RemotePCIConfigRequestHead;

static void remote_pci_check_config_request_head(int fd, uint64_t offset,
                                                 uint8_t size, uint8_t code)
{
    ssize_t rv;
    RemotePCIConfigRequestHead hdr;

    rv = read(fd, &hdr, sizeof(RemotePCIConfigRequestHead));
    g_assert_cmpint(rv, ==, sizeof(RemotePCIConfigRequestHead));
    g_assert_cmphex(hdr.code, ==, code);
    g_assert_cmpuint(hdr.offset, ==, offset);
    g_assert_cmpuint(hdr.size, ==, size);
}

static void remote_pci_check_config_read_request(int fd, uint64_t offset,
                                                 uint8_t size)
{
    remote_pci_check_config_request_head(fd, offset, size, CODE_READ_CONFIG);
}

static void remote_pci_check_config_write_request(int fd, uint64_t offset,
                                                  uint8_t size, uint64_t data)
{
    ssize_t rv;
    uint64_t req_data = 0;

    remote_pci_check_config_request_head(fd, offset, size, CODE_WRITE_CONFIG);
    rv = read(fd, &req_data, size);
    g_assert_cmpint(rv, ==, size);
    g_assert_cmphex(req_data, ==, data);
}

typedef struct {
    uint8_t code;
    uint8_t bar_no;
    uint64_t offset;
    uint8_t size;
} __attribute__ ((__packed__)) RemotePCIBarRequestHead;

static void remote_pci_check_bar_request_head(int fd, uint8_t bar_no,
                                              uint64_t offset, uint8_t size,
                                              uint8_t code)
{
    ssize_t rv;
    RemotePCIBarRequestHead hdr;

    rv = read(fd, &hdr, sizeof(RemotePCIBarRequestHead));
    g_assert_cmpint(rv, ==, sizeof(RemotePCIBarRequestHead));
    g_assert_cmphex(hdr.code, ==, code);
    g_assert_cmpuint(hdr.bar_no, ==, bar_no);
    g_assert_cmpuint(hdr.offset, ==, offset);
    g_assert_cmpuint(hdr.size, ==, size);
}

static void remote_pci_check_bar_read_request(int fd, uint8_t bar_no,
                                              uint64_t offset, uint8_t size)
{
    remote_pci_check_bar_request_head(fd, bar_no, offset, size, CODE_READ_DATA);
}

static void remote_pci_check_bar_write_request(int fd, uint8_t bar_no,
                                               uint64_t offset, uint8_t size,
                                               uint64_t data)
{
    ssize_t rv;
    uint64_t req_data = 0;

    remote_pci_check_bar_request_head(fd, bar_no, offset, size,
                                      CODE_WRITE_DATA);
    rv = read(fd, &req_data, size);
    g_assert_cmpint(rv, ==, size);
    g_assert_cmphex(req_data, ==, data);
}

typedef struct {
    uint8_t code;
    uint32_t vector_no;
} __attribute__ ((__packed__)) RemotePCIMsixRequest;

static void remote_pci_send_msix(int fd, uint32_t vector_no)
{
    ssize_t rv;
    RemotePCIMsixRequest req = {
        .code = CODE_MSIX,
        .vector_no = vector_no,
    };

    rv = write(fd, &req, sizeof(req));
    g_assert_cmpint(rv, ==, sizeof(req));
}

static void remote_pci_check_response(int fd, uint8_t code)
{
    ssize_t rv;
    uint8_t data;

    rv = read(fd, &data, sizeof(uint8_t));
    g_assert_cmpint(rv, ==, sizeof(uint8_t));
    g_assert_cmphex(data, ==, code | RESP_CODE_MASK);
}

static void remote_pci_test_cleanup(void *socket)
{
    int *s = socket;

    close(*s);
    qos_invalidate_command_line();
    g_free(s);
}

static void *remote_pci_setup_chardev(GString *cmd_line, void *arg)
{
    int *sock;
    in_port_t port;

    sock = g_new(int, 1);
    port = open_socket(sock);
    g_string_append_printf(cmd_line, QEMU_CMD_CHR, port);
    g_test_queue_destroy(remote_pci_test_cleanup, sock);

    return sock;
}

static void *remote_pci_get_driver(void *obj, const char *interface)
{
    QRemotePCI *pci = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &pci->dev;
    }

    fprintf(stderr, "%s not present in remote PCI\n", interface);
    g_assert_not_reached();
}

static void *remote_pci_create(void *pci_bus, QGuestAllocator *alloc,
                               void *addr)
{
    QRemotePCI *remote_pci = g_new0(QRemotePCI, 1);
    QPCIBus *bus = pci_bus;
    QPCIAddress *address = addr;

    /*
     * For remote PCI device, we can't call config_read at this point
     * as the connection can't be established. So we just set the bus
     * and devfn when creating the driver without checking the config
     * read as in qpci_device_init.
     */
    qpci_device_set(&remote_pci->dev, bus, address->devfn);
    remote_pci->obj.get_driver = remote_pci_get_driver;

    return &remote_pci->obj;
}

typedef struct {
    uint8_t cmd;
    uint8_t bar_no;
    uint64_t offset;
    uint8_t size;
    uint64_t data;
    uint8_t resp_code;
} RemoteRequest;

typedef struct {
    GThread *thread;
    RemoteRequest *requests;
    int request_size;
    int sock;
    GCond data_cond;
    GMutex data_mutex;
    bool data_ready;
} RemotePCIServerThread;

static void check_and_respond(int fd, RemoteRequest *req,
                              RemotePCIServerThread *t)
{
    switch (req->cmd) {
    case CODE_READ_DATA:
        remote_pci_check_bar_read_request(fd, req->bar_no, req->offset,
                                          req->size);
        remote_pci_respond_read(fd, req->resp_code, req->data, req->size);
        break;

    case CODE_WRITE_DATA:
        remote_pci_check_bar_write_request(fd, req->bar_no, req->offset,
                                           req->size, req->data);
        remote_pci_respond_write(fd, req->resp_code);
        break;

    case CODE_MSIX:
        remote_pci_send_msix(fd, req->data);
        remote_pci_check_response(fd, RESP_CODE_OK);
        g_mutex_lock(&t->data_mutex);
        t->data_ready = true;
        g_mutex_unlock(&t->data_mutex);
        g_cond_signal(&t->data_cond);
        break;

    case CODE_READ_CONFIG:
        remote_pci_check_config_read_request(fd, req->offset, req->size);
        remote_pci_respond_read(fd, req->resp_code, req->data, req->size);
        break;

    case CODE_WRITE_CONFIG:
        remote_pci_check_config_write_request(fd, req->offset,
                                              req->size, req->data);
        remote_pci_respond_write(fd, req->resp_code);
        break;

    default:
        g_assert_not_reached();
    }
}

static void *server_thread(void *data)
{
    RemotePCIServerThread *t = data;
    int fd = setup_fd(t->sock);
    int i;

    for (i = 0; i < t->request_size; ++i) {
        check_and_respond(fd, &t->requests[i], t);
    }
    close(fd);

    return NULL;
}

static void remote_pci_check_config_test(void *obj, void *data,
                                          QGuestAllocator *alloc)
{
    QRemotePCI *remote_pci = obj;
    QPCIDevice *pdev = &remote_pci->dev;

    qpci_device_enable(pdev);
    g_assert_cmphex(qpci_config_readw(pdev, PCI_VENDOR_ID), ==, TEST_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(pdev, PCI_DEVICE_ID), ==, TEST_DEVICE_ID);
    g_assert_cmphex(qpci_config_readw(pdev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                                      TEST_SUBSYSTEM_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(pdev, PCI_SUBSYSTEM_ID), ==,
                                      TEST_SUBSYSTEM_DEVICE_ID);
    g_assert_cmphex(qpci_config_readl(pdev, PCI_CLASS_REVISION), ==,
                                      TEST_CLASS_REV);
}

static void remote_pci_check_remote_config_test(void *obj, void *data,
                                                QGuestAllocator *alloc)
{
    QRemotePCI *remote_pci = obj;
    QPCIDevice *pdev = &remote_pci->dev;
    int *sock = data;
    uint16_t val;
    RemotePCIServerThread thread;
    RemoteRequest reqs[] = {
        {
            .cmd = CODE_WRITE_CONFIG,
            .offset = PCI_COMMAND,
            .size = 2,
            .data = PCI_COMMAND_IO,
            .resp_code = RESP_CODE_OK,
        },
        {
            .cmd = CODE_READ_CONFIG,
            .offset = PCI_COMMAND,
            .size = 2,
            .data = PCI_COMMAND_MEMORY,
            .resp_code = RESP_CODE_OK,
        },
    };

    thread.sock = *sock;
    thread.request_size = 2;
    thread.requests = reqs;
    thread.thread = g_thread_new("remote-pci-check-remote-config-server",
                                 server_thread, &thread);
    g_assert(thread.thread != NULL);

    /* Test remote config write */
    qpci_config_writew(pdev, PCI_COMMAND, PCI_COMMAND_IO);
    /* Test remote config read */
    val = qpci_config_readw(pdev, PCI_COMMAND);
    /*
     * The value should equal to the response over chardev,
     * not the value we wrote before.
     */
    g_assert_cmphex(val, ==, PCI_COMMAND_MEMORY);

    g_thread_join(thread.thread);
}

static void remote_pci_check_bar_read(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    QRemotePCI *remote_pci = obj;
    QPCIDevice *pdev = &remote_pci->dev;
    QPCIBar bar;
    int *sock = data;
    uint64_t bar_size;
    uint32_t val;
    RemotePCIServerThread thread;
    RemoteRequest req = {
        .cmd = CODE_READ_DATA,
        .bar_no = TEST_BAR_NO,
        .offset = TEST_OFFSET,
        .size = 4,
        .data = TEST_DATA,
        .resp_code = RESP_CODE_OK,
    };

    thread.sock = *sock;
    thread.request_size = 1;
    thread.requests = &req;
    thread.thread = g_thread_new("remote-pci-check-bar-read-server",
                                 server_thread, &thread);
    g_assert(thread.thread != NULL);

    qpci_device_enable(pdev);
    bar = qpci_iomap(pdev, TEST_BAR_NO, &bar_size);
    g_assert_cmpuint(bar_size, ==, TEST_BAR_SIZE);
    val = qpci_io_readl(pdev, bar, TEST_OFFSET);
    g_assert_cmphex(val, ==, TEST_DATA);

    g_thread_join(thread.thread);
}

static void remote_pci_check_bar_write(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    QRemotePCI *remote_pci = obj;
    QPCIDevice *pdev = &remote_pci->dev;
    QPCIBar bar;
    int *sock = data;
    uint64_t bar_size;
    RemotePCIServerThread thread;
    RemoteRequest req = {
        .cmd = CODE_WRITE_DATA,
        .bar_no = TEST_BAR_NO,
        .offset = TEST_OFFSET,
        .size = 4,
        .data = TEST_DATA,
        .resp_code = RESP_CODE_OK,
    };

    thread.sock = *sock;
    thread.request_size = 1;
    thread.requests = &req;
    thread.thread = g_thread_new("remote-pci-check-bar-write-server",
                                 server_thread, &thread);
    g_assert(thread.thread != NULL);

    qpci_device_enable(pdev);
    bar = qpci_iomap(pdev, TEST_BAR_NO, &bar_size);
    g_assert_cmpuint(bar_size, ==, TEST_BAR_SIZE);
    qpci_io_writel(pdev, bar, TEST_OFFSET, TEST_DATA);

    g_thread_join(thread.thread);
}

static void remote_pci_check_msix(void *obj, void *data, QGuestAllocator *alloc)
{
    QRemotePCI *remote_pci = obj;
    QPCIDevice *pdev = &remote_pci->dev;
    int *sock = data;
    RemotePCIServerThread thread;
    RemoteRequest reqs[] = {
        {
            .cmd = CODE_MSIX,
            .data = TEST_MSIX_VECTOR,
        },
    };

    /* Must enable MSIX before starting the server. */
    qpci_msix_enable(pdev);
    thread.sock = *sock;
    thread.request_size = ARRAY_SIZE(reqs);
    thread.requests = reqs;
    thread.data_ready = false;
    g_mutex_init(&thread.data_mutex);
    g_cond_init(&thread.data_cond);
    thread.thread = g_thread_new("remote-pci-check-bar-write-server",
                                 server_thread, &thread);
    g_assert(thread.thread != NULL);
    qpci_device_enable(pdev);
    /* check number of MSIX vectors */
    g_assert_cmpuint(qpci_msix_table_size(pdev), ==, TEST_NUM_MSIX_VECTORS);
    /* Wait until the MSIX request is sent. */
    g_mutex_lock(&thread.data_mutex);
    while (!thread.data_ready) {
        g_cond_wait(&thread.data_cond, NULL);
    }
    thread.data_ready = false;
    g_mutex_unlock(&thread.data_mutex);
    /* Check we can see the MSIX request issued. */
    g_assert_true(qpci_msix_pending(pdev, TEST_MSIX_VECTOR));

    g_thread_join(thread.thread);
    qpci_msix_disable(pdev);
}

static void remote_pci_register_nodes(void)
{
    g_autofree char *opts_str = g_strdup_printf(
        "addr=04.0,vendor-id=%u,device-id=%u,subsystem-vendor-id=%u,"
        "subsystem-device-id=%u,class-revision=%u",
        TEST_VENDOR_ID, TEST_DEVICE_ID, TEST_SUBSYSTEM_VENDOR_ID,
        TEST_SUBSYSTEM_DEVICE_ID, TEST_CLASS_REV);

    QOSGraphEdgeOptions opts = {
        .extra_device_opts = opts_str,
    };

    add_qpci_address(&opts, &(QPCIAddress) {
        .devfn = QPCI_DEVFN(4, 0)
    });

    qos_node_create_driver("remote-pci", remote_pci_create);
    qos_node_consumes("remote-pci", "pci-bus", &opts);
    qos_node_produces("remote-pci", "pci-device");

    qos_add_test("check-config", "remote-pci", remote_pci_check_config_test,
                 NULL);
    qos_add_test("check-remote-config", "remote-pci",
                 remote_pci_check_remote_config_test, &(QOSGraphTestOptions) {
        .before = remote_pci_setup_chardev,
        .edge.extra_device_opts = "use-external-cfg=on,chardev=pci0",
    });
    qos_add_test("check-bar-read", "remote-pci",
                 remote_pci_check_bar_read, &(QOSGraphTestOptions) {
        .before = remote_pci_setup_chardev,
        .edge.extra_device_opts = "bar-size[5]=1024,chardev=pci0",
    });
    qos_add_test("check-bar-write", "remote-pci",
                 remote_pci_check_bar_write, &(QOSGraphTestOptions) {
        .before = remote_pci_setup_chardev,
        .edge.extra_device_opts = "bar-size[5]=1024,chardev=pci0",
    });
    qos_add_test("check-msix", "remote-pci", remote_pci_check_msix,
                 &(QOSGraphTestOptions) {
        .before = remote_pci_setup_chardev,
        .edge.extra_device_opts = "bar-size[2]=4096,bar-size[1]=1024,"
            "chardev=pci0,msix-vector-count=32,msix-table-bar-number=2,"
            "msix-table-bar-offset=0,msix-pba-bar-number=2,"
            "msix-pba-bar-offset=1000",
    });
}
libqos_init(remote_pci_register_nodes);
