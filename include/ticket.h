/***************************************************************************
 * This file is part of Ticket Cleaner.                                    *
 * Copyright (c) 2022 V10lator <v10lator@myway.de>                         *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 3 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, If not, see <http://www.gnu.org/licenses/>.  */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct WUT_PACKED
    {
        uint32_t sig_type;
        uint8_t sig[0x100];
        WUT_UNKNOWN_BYTES(0x3C);
        char issuer[0x40];
        uint8_t ecdsa_pubkey[0x3c];
        uint8_t version;
        uint8_t ca_clr_version;
        uint8_t signer_crl_version;
        uint8_t key[0x10];
        WUT_UNKNOWN_BYTES(0x01);
        uint64_t ticket_id;
        uint32_t device_id;
        uint64_t tid;
        uint16_t sys_access;
        uint16_t title_version;
        WUT_UNKNOWN_BYTES(0x08);
        uint8_t license_type;
        uint8_t ckey_index;
        uint16_t property_mask;
        WUT_UNKNOWN_BYTES(0x28);
        uint32_t account_id;
        WUT_UNKNOWN_BYTES(0x01);
        uint8_t audit;
        WUT_UNKNOWN_BYTES(0x42);
        uint8_t limit_entries[0x40];
        uint16_t header_version; // we support version 1 only!
        uint16_t header_size;
        uint32_t total_hdr_size;
        uint32_t sect_hdr_offset;
        uint16_t num_sect_headers;
        uint16_t num_sect_header_entry_size;
        uint32_t header_flags;
    } TICKET;
    // WUT_CHECK_OFFSET(TICKET, 0x0004, sig);
    WUT_CHECK_OFFSET(TICKET, 0x0140, issuer);
    WUT_CHECK_OFFSET(TICKET, 0x0180, ecdsa_pubkey);
    WUT_CHECK_OFFSET(TICKET, 0x01BC, version);
    WUT_CHECK_OFFSET(TICKET, 0x01BD, ca_clr_version);
    WUT_CHECK_OFFSET(TICKET, 0x01BE, signer_crl_version);
    WUT_CHECK_OFFSET(TICKET, 0x01BF, key);
    WUT_CHECK_OFFSET(TICKET, 0x01D0, ticket_id);
    WUT_CHECK_OFFSET(TICKET, 0x01D8, device_id);
    WUT_CHECK_OFFSET(TICKET, 0x01DC, tid);
    WUT_CHECK_OFFSET(TICKET, 0x01E4, sys_access);
    WUT_CHECK_OFFSET(TICKET, 0x01E6, title_version);
    WUT_CHECK_OFFSET(TICKET, 0x01F0, license_type);
    WUT_CHECK_OFFSET(TICKET, 0x01F1, ckey_index);
    WUT_CHECK_OFFSET(TICKET, 0x01F2, property_mask);
    WUT_CHECK_OFFSET(TICKET, 0x021C, account_id);
    WUT_CHECK_OFFSET(TICKET, 0x0221, audit);
    WUT_CHECK_OFFSET(TICKET, 0x0264, limit_entries);
    WUT_CHECK_OFFSET(TICKET, 0x02A4, header_version);
    WUT_CHECK_OFFSET(TICKET, 0x02A6, header_size);
    WUT_CHECK_OFFSET(TICKET, 0x02A8, total_hdr_size);
    WUT_CHECK_OFFSET(TICKET, 0x02AC, sect_hdr_offset);
    WUT_CHECK_OFFSET(TICKET, 0x02B0, num_sect_headers);
    WUT_CHECK_OFFSET(TICKET, 0x02B2, num_sect_header_entry_size);
    WUT_CHECK_OFFSET(TICKET, 0x02B4, header_flags);
    WUT_CHECK_SIZE(TICKET, 0x02B8);

#ifdef __cplusplus
}
#endif
