/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)
// 字典节点
typedef struct dictEntry {
    void *key; // 键
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v; //值
    struct dictEntry *next; // 指针指向下一个节点
} dictEntry;

// 字典节点的操作函数
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);
    void *(*keyDup)(void *privdata, const void *key);
    void *(*valDup)(void *privdata, const void *obj);
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);
    void (*keyDestructor)(void *privdata, void *key);
    void (*valDestructor)(void *privdata, void *obj);
} dictType;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
// hash句柄
typedef struct dictht {
    dictEntry **table;// 节点链表
    unsigned long size;// 当前表的数组大小,即桶的个数
    unsigned long sizemask; // 掩码：size-1
    unsigned long used;// 已存节点数
} dictht;
// 字典句柄
typedef struct dict {
    dictType *type;// 字典节点的操作函数方法
    void *privdata; // 创建字典时引入的私有数据
    dictht ht[2];// 两个字典句柄, 除非目前处于渐进式hash中，所有的节点元素都处在ht[0]中
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */ //渐进式hash即将要处理的ht[0]桶的下标位置，-1表示当前无渐进中
    unsigned long iterators; /* number of iterators currently running */// 当前字典有安全迭代器的个数, 当此值非0时会暂停渐进式hash
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
// 字典迭代器
// 迭代器分为安全迭代器与非安全迭代器
/** 
 * 1. 需要安全迭代器的原因如下:
 * 场景: 一边遍历一边增加/删除字典里的元素
 * 上述场景在字典处于渐进式数据迁移时,如不暂停数据迁移会导致遍历过程出现元素重复or丢失异常
 * 在遍历时需要对字典进行增删的场景下只能使用安全迭代器
**/
/**
 * 2. 既然已经有了安全迭代器,那为什么还要有一个非安全迭代器呢?
 * 原因: 这个跟多进程内存cow机制有关. 
 *       为了支持安全迭代器可以暂停当前字典的渐进式数据迁移过程,内部实现是在当前字典dict句柄里维护一个安全迭代器计数器,
 *       在第一次使用迭代器遍历时,对此计数器+1,这样在调用字典的增删改查接口时判断此计数器是否为0以决定是否暂停数据迁移.
 *       在释放安全迭代器时,又会对当前字典dict句柄里的安全迭代器计数器-1,以允许数据迁移.
 *       上述过程, 本质上是对字典dict结构体一个元素的修改.
 * 
 *       而redis在进行内存数据rdb持久化 或者 aof-rewrite时,是通过fork子进程, 由子进程将自身"静止的"内存数据进行持久化,
 *       *inux系统为提升fork性能,普遍采用内存cow写时拷贝机制,即fork出的子进程共享父进程的内存空间,
 *       只有当出现修改此共享内存空间时,才会拷贝出相关涉及到的内存页,对其进行修改.
 * 
 *      基于上述情况, 如果在子进程的数据持久化时使用安全迭代器,必然会导致dict里的计数器字段改动,进行导致不必要的内存页cow.
 *      所以对于只读式的遍历场景,可以使用非安全迭代器,以避免不必要的内存写时拷贝.
 * 
 */ 
typedef struct dictIterator {
    dict *d; // 迭代器当前操作的字典对象
    long index; // 单张hash表里的桶下标;
    int table; // 表示当前dictht数组下标,即具体是使用到哪张hash表
    int safe; // 当前迭代器是否是安全迭代器.对于安全迭代器会在第一次使用时会增加dict->iterators计数器,进而暂停渐进式数据迁移.
    dictEntry *entry;// 指向桶中链表里的当前某个节点
    dictEntry *nextEntry; // 指向桶中链表里的下一个节点,以避免迭代器用户删除当前返回的entry节点,进而中断后续迭代.
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint; // 字典指纹, 用于非安全迭代器在遍历前后侦测字典是否发生扩容伸缩调整;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

/* This is the initial size of every hash table */
// 初始字典大小, 其值必须是2的幂
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
#define dictIsRehashing(d) ((d)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *dictAddOrFind(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
int dictDelete(dict *d, const void *key);
dictEntry *dictUnlink(dict *ht, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, int len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
