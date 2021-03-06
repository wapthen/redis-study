/* Synchronous socket and file I/O operations useful across the core.
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"

/* ----------------- Blocking sockets I/O with timeouts --------------------- */

/* Redis performs most of the I/O in a nonblocking way, with the exception
 * of the SYNC command where the slave does it in a blocking way, and
 * the MIGRATE command that must be blocking in order to be atomic from the
 * point of view of the two instances (one migrating the key and one receiving
 * the key). This is why need the following blocking I/O functions.
 *
 * All the functions take the timeout in milliseconds. */

// 阻塞式等待套接字可用的最小时长
#define SYNCIO__RESOLUTION 10 /* Resolution in milliseconds */

/* Write the specified payload to 'fd'. If writing the whole payload will be
 * done within 'timeout' milliseconds the operation succeeds and 'size' is
 * returned. Otherwise the operation fails, -1 is returned, and an unspecified
 * partial write could be performed against the file descriptor. */
// 同步发送指定长度的数据到套接字里
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nwritten, ret = size;
    long long start = mstime();
    long long remaining = timeout;

    while(1) {
        long long wait = (remaining > SYNCIO__RESOLUTION) ?
                          remaining : SYNCIO__RESOLUTION;
        long long elapsed;

        /* Optimistically try to write before checking if the file descriptor
         * is actually writable. At worst we get EAGAIN. */
        // 优先尝试直接发送数据
        nwritten = write(fd,ptr,size);
        if (nwritten == -1) {
            if (errno != EAGAIN) return -1;
        } else {
            ptr += nwritten;
            size -= nwritten;
        }
        // 全部发送完毕的场景
        if (size == 0) return ret;

        // 部分发送成功 or errno为EAGAIN的场景,则需要如下阻塞式等待该套接字为可写状态
        /* Wait */
        aeWait(fd,AE_WRITABLE,wait);
        // 校验是否超时
        elapsed = mstime() - start;
        if (elapsed >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining = timeout - elapsed;
    }
}

/* Read the specified amount of bytes from 'fd'. If all the bytes are read
 * within 'timeout' milliseconds the operation succeed and 'size' is returned.
 * Otherwise the operation fails, -1 is returned, and an unspecified amount of
 * data could be read from the file descriptor. */
// 同步阻塞式等待timeout时间来读取指定句柄里的数据
// 注意除非发生短读情况 或者 读取到指定长度的数据,否则此函数会一直等待timeout时长
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nread, totread = 0;
    long long start = mstime();
    long long remaining = timeout;

    if (size == 0) return 0;
    while(1) {
        long long wait = (remaining > SYNCIO__RESOLUTION) ?
                          remaining : SYNCIO__RESOLUTION;
        long long elapsed;

        /* Optimistically try to read before checking if the file descriptor
         * is actually readable. At worst we get EAGAIN. */
        nread = read(fd,ptr,size);
        if (nread == 0) return -1; /* short read. */
        if (nread == -1) {
            if (errno != EAGAIN) return -1;
        } else {
            ptr += nread;
            size -= nread;
            totread += nread;
        }
        if (size == 0) return totread;

        /* Wait */
        aeWait(fd,AE_READABLE,wait);
        elapsed = mstime() - start;
        if (elapsed >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining = timeout - elapsed;
    }
}

/* Read a line making sure that every char will not require more than 'timeout'
 * milliseconds to be read.
 *
 * On success the number of bytes read is returned, otherwise -1.
 * On success the string is always correctly terminated with a 0 byte. */
// 类阻塞式的读取套接字里的数据,直到返回的数据第一次遇到\n,同时最终的返回数据里会自动抹掉末尾的\r\n
// 注意此函数是从套接字里逐个字符读取,而且每个读取时均是以完整的timeout计时,也就是说实际超时时长会是N*timeout
// 此处是为了区分:回复数据里可能有对psync的响应数据+rdb数据流,所以需要精确的从套接字缓冲区取出psync的响应数据,
// 而将可能的rdb数据留在套接字缓冲区里便于后续专一读取rdb数据流
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nread = 0;

    size--;
    while(size) {
        char c;

        if (syncRead(fd,&c,1,timeout) == -1) return -1;
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            return nread;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
        size--;
    }
    return nread;
}
