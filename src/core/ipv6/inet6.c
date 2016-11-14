/**
 * @file
 * Functions common to all TCP/IPv6 modules, such as the Internet checksum and the
 * byte order functions.
 *
 */

/*
 * Copyright (c) 2010 Inico Technologies Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Ivan Delamer <delamer@inicotech.com>
 *
 */

#include "lwip/opt.h"

#include "lwip/def.h"
#include "lwip/inet.h"

/** This variable is initialized by the system to contain the wildcard IPv6 address.
 */
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;

u16_t
inet_chksum_pbuf(struct pbuf *p)
{
  u32_t acc;
  struct pbuf *q;
  u8_t swapped;
  
  acc = 0;
  swapped = 0;
  for(q = p; q != NULL; q = q->next) {
    acc += chksum(q->payload, q->len);
    while (acc >> 16) {
      acc = (acc & 0xffff) + (acc >> 16);
    }    
    if (q->len % 2 != 0) {
      swapped = 1 - swapped;
      acc = (acc & 0xff << 8) | (acc & 0xff00 >> 8);
    }
  }
 
  if (swapped) {
    acc = ((acc & 0xff) << 8) | ((acc & 0xff00) >> 8);
  }
  return ~(acc & 0xffff);
}
