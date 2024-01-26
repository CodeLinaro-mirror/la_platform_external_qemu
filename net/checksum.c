/*
 *  IP checksumming functions.
 *  (c) 2008 Gerd Hoffmann <kraxel@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 or later of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <stddef.h>

#include "qemu/osdep.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "qemu/log.h"

uint32_t net_checksum_add_cont(int len, uint8_t *buf, int seq)
{
    uint32_t sum1 = 0, sum2 = 0;
    int i;

    for (i = 0; i < len - 1; i += 2) {
        sum1 += (uint32_t)buf[i];
        sum2 += (uint32_t)buf[i + 1];
    }
    if (i < len) {
        sum1 += (uint32_t)buf[i];
    }

    if (seq & 1) {
        return sum1 + (sum2 << 8);
    } else {
        return sum2 + (sum1 << 8);
    }
}

uint16_t net_checksum_finish(uint32_t sum)
{
    while (sum>>16)
        sum = (sum & 0xFFFF)+(sum >> 16);
    return ~sum;
}

uint16_t net_checksum_tcpudp(uint16_t length, uint16_t proto,
                             uint8_t *addrs, uint8_t *buf,
                             uint8_t addr_len)
{
    uint32_t sum = 0;

    sum += net_checksum_add(length, buf);         /* payload */
    sum += net_checksum_add(addr_len << 1, addrs);/* src + dst address */
    sum += proto + length;                        /* protocol & length */
    return net_checksum_finish(sum);
}

/* Takes the base address to avoid unaligned address accessing */
static bool is_ipv4_fragment_by_base_address(uint8_t *base)
{
    return (be16_to_cpu((ldub_p(base + offsetof(struct ip_header, ip_off))) & \
            (IP_OFFMASK | IP_MF)) != 0);
}

static void net_checksum_ipv4(uint8_t *ip, int csum_flag)
{
    uint16_t csum;

    /* Calculate IP checksum */
    if (csum_flag & CSUM_IP) {
        stw_he_p(ip + offsetof(struct ip_header, ip_sum), 0);
        csum = net_raw_checksum((uint8_t *)ip, IP_HDR_GET_LEN(ip));
        stw_he_p(ip + offsetof(struct ip_header, ip_sum), csum);
    }

    if (is_ipv4_fragment_by_base_address(ip)) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: fragmented IP packet!", __func__);
    }
}

/* returns payload length. */
static uint16_t net_payload_length_ipv4(uint8_t *ip)
{
    return lduw_be_p(ip + offsetof(struct ip_header, ip_len)) - \
                     sizeof(struct ip_header);
}

/* returns payload length. */
static uint16_t net_payload_length_ipv6(uint8_t *ip)
{
    return lduw_be_p(ip + offsetof(struct ip6_header, ip6_plen));
}

void net_checksum_calculate(void *data, int length, int csum_flag)
{
    int mac_hdr_len, ip_version;
    uint16_t ip_len;
    uint16_t csum;
    uint8_t ip_p, addr_len;
    uint8_t *ip_src;
    uint8_t *ip_nxt;
    uint8_t *ip_base_addr;

    /*
     * Note: We cannot assume "data" is aligned, so the all code uses
     * some macros that take care of possible unaligned access for
     * struct members (just in case).
     * We can't access the member directly neither. Thus the access wll be
     * in the format of base_address + offsetof(struct, member).
     */

    /* Ensure we have at least an Eth header */
    if (length < sizeof(struct eth_header)) {
        return;
    }

    /* Handle the optional VLAN headers */
    switch (lduw_be_p(&PKT_GET_ETH_HDR(data)->h_proto)) {
    case ETH_P_VLAN:
        mac_hdr_len = sizeof(struct eth_header) +
                     sizeof(struct vlan_header);
        break;
    case ETH_P_DVLAN:
        if (lduw_be_p(&PKT_GET_VLAN_HDR(data)->h_proto) == ETH_P_VLAN) {
            mac_hdr_len = sizeof(struct eth_header) +
                         2 * sizeof(struct vlan_header);
        } else {
            mac_hdr_len = sizeof(struct eth_header) +
                         sizeof(struct vlan_header);
        }
        break;
    default:
        mac_hdr_len = sizeof(struct eth_header);
        break;
    }

    length -= mac_hdr_len;

    /* Now check we have an IP header (with an optional VLAN header) */
    if (length < sizeof(struct ip_header)) {
        return;
    }

    ip_base_addr = data + mac_hdr_len;
    ip_version = (ldub_p(ip_base_addr + offsetof(struct ip_header, ip_ver_len)) \
                 >> 4) & 0xf;

    switch (ip_version) {
    case IP_HEADER_VERSION_4:
        net_checksum_ipv4(ip_base_addr, csum_flag);
        ip_len = net_payload_length_ipv4(ip_base_addr);
        ip_p = ldub_p(ip_base_addr + offsetof(struct ip_header, ip_p));
        ip_src = ip_base_addr + offsetof(struct ip_header, ip_src);
        ip_nxt = ip_base_addr + sizeof(struct ip_header);
        addr_len = 4;
        break;
    case IP_HEADER_VERSION_6:
        ip_len = net_payload_length_ipv6(ip_base_addr);
        ip_p = ldub_p(ip_base_addr + offsetof(struct ip6_header, ip6_nxt));
        ip_src = ip_base_addr + offsetof(struct ip6_header, ip6_src);
        ip_nxt = ip_base_addr + sizeof(struct ip6_header);
        addr_len = 16;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "%s: Unknown IP version: %d", __func__, ip_version);
        return;
    }

    switch (ip_p) {
    case IP_PROTO_TCP:
    {
        if (!(csum_flag & CSUM_TCP)) {
            return;
        }

        /* ip_nxt is basically the offset of tcp_header */
        uint8_t *tcp_base_addr = ip_nxt;

        if (ip_len < sizeof(tcp_header)) {
            return;
        }

        /* Set csum to 0 */
        stw_he_p(tcp_base_addr + offsetof(tcp_header, th_sum), 0);

        csum = net_checksum_tcpudp(ip_len, ip_p, ip_src,
                                   tcp_base_addr, addr_len);

        /* Store computed csum */
        stw_be_p(tcp_base_addr + offsetof(tcp_header, th_sum), csum);

        break;
    }
    case IP_PROTO_UDP:
    {
        if (!(csum_flag & CSUM_UDP)) {
            return;
        }

        /* ip_nxt is the offset of udp_header */
        uint8_t *udp_base_addr = ip_nxt;

        if (ip_len < sizeof(udp_header)) {
            return;
        }

        /* Set csum to 0 */
        stw_he_p(udp_base_addr + offsetof(udp_header, uh_sum), 0);

        csum = net_checksum_tcpudp(ip_len, ip_p, ip_src,
                                   udp_base_addr, addr_len);

        /* Store computed csum */
        stw_be_p(udp_base_addr + offsetof(udp_header, uh_sum), csum);

        break;
    }
    default:
        /* Can't handle any other protocol */
        break;
    }
}

uint32_t
net_checksum_add_iov(const struct iovec *iov, const unsigned int iov_cnt,
                     uint32_t iov_off, uint32_t size, uint32_t csum_offset)
{
    size_t iovec_off;
    unsigned int i;
    uint32_t res = 0;

    iovec_off = 0;
    for (i = 0; i < iov_cnt && size; i++) {
        if (iov_off < (iovec_off + iov[i].iov_len)) {
            size_t len = MIN((iovec_off + iov[i].iov_len) - iov_off , size);
            void *chunk_buf = iov[i].iov_base + (iov_off - iovec_off);

            res += net_checksum_add_cont(len, chunk_buf, csum_offset);
            csum_offset += len;

            iov_off += len;
            size -= len;
        }
        iovec_off += iov[i].iov_len;
    }
    return res;
}
