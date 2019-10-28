/*
* Author: Christian Huitema
* Copyright (c) 2019, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "util.h"
#include "qinqproto.h"

uint8_t* picoquic_frames_fixed_skip(uint8_t* bytes, const uint8_t* bytes_max, size_t size);
uint8_t* picoquic_frames_varint_decode(uint8_t* bytes, const uint8_t* bytes_max, uint64_t* n64);
uint8_t* picoquic_frames_varlen_decode(uint8_t* bytes, const uint8_t* bytes_max, size_t* n);
uint8_t* picoquic_frames_uint8_decode(uint8_t* bytes, const uint8_t* bytes_max, uint8_t* n);
uint8_t* picoquic_frames_uint16_decode(uint8_t* bytes, const uint8_t* bytes_max, uint16_t* n);
uint8_t* picoquic_frames_uint32_decode(uint8_t* bytes, const uint8_t* bytes_max, uint32_t* n);
uint8_t* picoquic_frames_uint64_decode(uint8_t* bytes, const uint8_t* bytes_max, uint64_t* n);
uint8_t* picoquic_frames_cid_decode(uint8_t* bytes, const uint8_t* bytes_max, picoquic_connection_id_t* n);

static int qinq_copy_address(struct sockaddr_storage* addr_s, size_t address_length, const uint8_t* address, uint16_t port) {
    int ret = 0;

    if (address_length == 4) {
        struct sockaddr_in* addr4 = (struct sockaddr_in*)addr_s;
        addr4->sin_family = AF_INET;
        memcpy(&addr4->sin_addr, address, 4);
        addr4->sin_port = port;
    }
    else if (address_length == 16) {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)addr_s;
        addr6->sin6_family = AF_INET6;
        memcpy(&addr6->sin6_addr, address, 16);
        addr6->sin6_port = port;
    }
    else {
        ret = -1;
    }

    return ret;
}


/* The datagram frames start with:
 *  - if using h3, varint describing the QINQ stream. Omitted in QINQ native, because there is only one stream.
 *       This will be already decoded by the HTTP specific code before calling the parser
 *  - Header compression index, varint.
 *       integer value N if this is a place holder for the IP address and the CID
 * The structure of the datagram would thus be:
 *    <0><length of address><address><16 bit port number><first byte><reminder of packet including DCID>
 *    <N(1rTT)><reminder of 1-RTT packet with DCID bytes removed>
 * Compression of Initial or handshake packet is for further study.
 */

uint8_t * picoqinq_decode_datagram_header(uint8_t * bytes, uint8_t * bytes_max,
    struct sockaddr_storage* addr_s,
    picoquic_connection_id_t** cid, picoqinq_header_compression_t ** p_receive_hc)
{
    uint64_t hcid;
    size_t address_length = 0; 
    uint8_t* address = NULL;
    uint16_t port = 0;

    *cid = NULL;
    memset(addr_s, 0, sizeof(struct sockaddr_storage));

    bytes = picoquic_frames_varint_decode(bytes, bytes_max, &hcid);
    if (bytes != NULL) {
        if (hcid == 0) {
            if ((bytes = picoquic_frames_varlen_decode(bytes, bytes_max, &address_length)) != NULL) {
                address = bytes;
                if ((bytes = picoquic_frames_fixed_skip(bytes, bytes_max, address_length)) != NULL &&
                    (bytes = picoquic_frames_uint16_decode(bytes, bytes_max, &port)) != NULL){
                    if (qinq_copy_address(addr_s, address_length, address, port) != 0) {
                        bytes = NULL;
                    }
                }
            }
        }
        else {
            picoqinq_header_compression_t* hc = picoqinq_find_reserve_header_by_id(p_receive_hc, hcid);

            if (hc == NULL) {
                bytes = NULL;
            }
            else if (picoquic_store_addr(addr_s, (struct sockaddr*) & hc->addr_s) == 0) {
                bytes = NULL;
            } else {
                *cid = &hc->cid;
            }
        }
    }

    return bytes;
}

/* To reserve a header, the node send a reserve header message on a new bidir
 * stream (either client or server depending of direction). The message has the
 * format: 
 *     - Op code "reserve header" -- varint.
 *     - Direction: 0 to server, 1 to client,
 *     - address length -- varint
 *     - N bytes of address
 *     - 16 bits port number
 *     - cid length -- varint
 *     - cid content bytes
 * The other node replies with a message composed of a single varint:
 *     - header compression code -- varint
 * A reply of max int (0x3FFFFFFFFFFFFFFF) indicates that no code is available.
 * Other replies indicate that the compression code can now be used.
 */

uint8_t* picoqinq_encode_reserve_header(uint8_t* bytes, uint8_t* bytes_max,
    uint64_t direction, uint64_t hcid, const struct sockaddr* addr, const picoquic_connection_id_t* cid)
{

    uint16_t port = 0;
    uint8_t* addr_bytes = NULL;
    size_t address_length = 0;

    if (addr->sa_family == AF_INET) {
        struct sockaddr_in* addr4 = (struct sockaddr_in*)addr;
        address_length = 4;
        addr_bytes = (uint8_t*)& addr4->sin_addr;
        port = addr4->sin_port;
    }
    else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)addr;
        address_length = 16;
        addr_bytes = (uint8_t*)& addr6->sin6_addr;
        port = addr6->sin6_port;
    }
    else {
        return NULL;
    }

    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, QINQ_PROTO_RESERVE_HEADER)) != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max, direction)) != NULL &&
        (bytes = picoquic_frames_varint_encode(bytes, bytes_max, hcid)) != NULL &&
        (bytes = picoquic_frames_l_v_encode(bytes, bytes_max, address_length, addr_bytes)) != NULL &&
        (bytes = picoquic_frames_uint16_encode(bytes, bytes_max, port)) != NULL) {
        bytes = picoquic_frames_cid_encode(bytes, bytes_max, cid);
    }

    return bytes;
}

/* Assume that the operation code is already parsed */
uint8_t* picoqinq_decode_reserve_header(uint8_t* bytes, uint8_t* bytes_max,
    uint64_t* direction, uint64_t* hcid, struct sockaddr_storage* addr_s, picoquic_connection_id_t* cid)
{
    size_t address_length = 0;
    uint8_t* address = NULL;
    uint16_t port = 0;

    if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, direction)) != NULL &&
        (bytes = picoquic_frames_varint_decode(bytes, bytes_max, hcid)) != NULL &&
        (bytes = picoquic_frames_varlen_decode(bytes, bytes_max, &address_length)) != NULL) {
        address = bytes;
        if ((bytes = picoquic_frames_fixed_skip(bytes, bytes_max, address_length)) != NULL &&
            (bytes = picoquic_frames_uint16_decode(bytes, bytes_max, &port)) != NULL &&
            (bytes = picoquic_frames_cid_decode(bytes, bytes_max, cid)) != NULL){
            if (qinq_copy_address(addr_s, address_length, address, port) != 0) {
                bytes = NULL;
            }
        }
    }

    return bytes;
}

picoqinq_header_compression_t* picoqinq_create_header(uint64_t hcid, struct sockaddr * addr, const picoquic_connection_id_t* cid)
{
    picoqinq_header_compression_t* hc = (picoqinq_header_compression_t*)malloc(sizeof(picoqinq_header_compression_t));
    if (hc != NULL) {
        hc->next_hc = NULL;
        hc->hcid = hcid;
        picoquic_store_addr(&hc->addr_s, addr);
        memcpy(hc->cid.id, cid->id, cid->id_len);
        hc->cid.id_len = cid->id_len;
    }
    return hc;
}

void picoqinq_reserve_header(picoqinq_header_compression_t* hc, picoqinq_header_compression_t** phc_head)
{
    if (hc != NULL && phc_head != NULL) {
        picoqinq_header_compression_t** phc_next = &hc->next_hc;
        hc->next_hc = *phc_head;
        *phc_head = hc;

        while (*phc_next) {          
            if ((*phc_next)->hcid == hc->hcid) {
                picoqinq_header_compression_t* to_delete = *phc_next;
                *phc_next = to_delete->next_hc;
                free(to_delete);
            }
            else {
                phc_next = &(*phc_next)->next_hc;
            }
        }
    }
}

uint64_t picoqinq_find_reserve_header_id_by_address(picoqinq_header_compression_t** phc_head, struct sockaddr* addr, const picoquic_connection_id_t* cid)
{
    uint64_t hcid = 0;
    picoqinq_header_compression_t** pnext = phc_head;
    picoqinq_header_compression_t* next;

    while ((next = *pnext) != NULL) {
        if (picoquic_compare_addr((struct sockaddr*)&next->addr_s, addr) == 0 &&
            next->cid.id_len == cid->id_len &&
            memcmp(next->cid.id, cid->id, cid->id_len) == 0) {
            hcid = next->hcid;

            if (next != *phc_head) {
                /* Bring LRU on top of list */
                *pnext = next->next_hc;
                next->next_hc = *phc_head;
                *phc_head = next;
            }
            break;
        }
        else {
            pnext = &next->next_hc;
        }
    }
    return hcid;
}

picoqinq_header_compression_t* picoqinq_find_reserve_header_by_id(picoqinq_header_compression_t** phc_head, uint64_t hcid)
{
    picoqinq_header_compression_t** pnext = phc_head;
    picoqinq_header_compression_t* next;

    while ((next = *pnext) != NULL) {
        if (next->hcid == hcid) {
            if (next != *phc_head) {
                /* Bring LRU on top of list */
                *pnext = next->next_hc;
                next->next_hc = *phc_head;
                *phc_head = next;
            }
            break;
        }
        else {
            pnext = &next->next_hc;
        }
    }
    return next;
}

/* To reserve a CID to handle incoming packets, the client sends a CID reservation
 * message on a new bidir client stream. The message has the format:
 *     - Op code "reserve header" -- varint.
 *     - cid length -- varint
 *     - cid content bytes 
 * There is no reply necessary -- the server just closes the stream.  
 * There is no synchronization requirement -- this is best effort.
 *
 * The CID is pushed when it is created.
 * We may consider dropping when it is retired.
 *
 * The hash table managed by the server lists the first N bytes of the CID.
 * This delivers a list of matching connection. A secondary filter per
 * connection checks the match for the full CID. 
 */

uint8_t* picoqinq_encode_reserve_cid(uint8_t* bytes, uint8_t* bytes_max, const picoquic_connection_id_t* cid)
{
    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, QINQ_PROTO_RESERVE_CID)) != NULL) {
        bytes = picoquic_frames_cid_encode(bytes, bytes_max, cid);
    }
    return bytes;
}

uint8_t* picoqinq_decode_reserve_cid(uint8_t* bytes, uint8_t* bytes_max, picoquic_connection_id_t* cid)
{
    return picoquic_frames_cid_decode(bytes, bytes_max, cid);
}