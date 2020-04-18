/* See endianconv.c top comments for more information
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2011-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ENDIANCONV_H
#define __ENDIANCONV_H

#include "config.h"
#include <stdint.h>

// 内存级颠倒字节(参数是16位)
void memrev16(void *p);
// 内存级颠倒字节(参数是32位)
void memrev32(void *p);
// 内存级颠倒字节(参数是64位)
void memrev64(void *p);
// 整数级颠倒字节(参数是16位)
uint16_t intrev16(uint16_t v);
// 整数级颠倒字节(参数是32位)
uint32_t intrev32(uint32_t v);
// 整数级颠倒字节(参数是64位)
uint64_t intrev64(uint64_t v);

/* variants of the function doing the actual conversion only if the target
 * host is big endian */
// 如果宿主机是小尾架构，则如下转换函数全部空转
#if (BYTE_ORDER == LITTLE_ENDIAN)
#define memrev16ifbe(p) ((void)(0))
#define memrev32ifbe(p) ((void)(0))
#define memrev64ifbe(p) ((void)(0))
#define intrev16ifbe(v) (v)
#define intrev32ifbe(v) (v)
#define intrev64ifbe(v) (v)
#else
// 如果宿主机是大尾架构，则需要按字节头尾颠倒转为小尾方式
#define memrev16ifbe(p) memrev16(p)
#define memrev32ifbe(p) memrev32(p)
#define memrev64ifbe(p) memrev64(p)
#define intrev16ifbe(v) intrev16(v)
#define intrev32ifbe(v) intrev32(v)
#define intrev64ifbe(v) intrev64(v)
#endif

/* The functions htonu64() and ntohu64() convert the specified value to
 * network byte ordering and back. In big endian systems they are no-ops. */
#if (BYTE_ORDER == BIG_ENDIAN)
#define htonu64(v) (v)
#define ntohu64(v) (v)
#else
// 本机为小尾体系
// 网络传输数据统一为大尾,所以对于本机小尾数据发送到网络上,需要转位;收到网络的大尾数据需要转为本机的小尾数据使用
#define htonu64(v) intrev64(v)
#define ntohu64(v) intrev64(v)
#endif

#ifdef REDIS_TEST
int endianconvTest(int argc, char *argv[]);
#endif

#endif
