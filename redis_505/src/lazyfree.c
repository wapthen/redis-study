#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "cluster.h"

// 待异步释放的对象计数器:提交任务时数值增加,在后台线程实际释放对象后数值减少
static size_t lazyfree_objects = 0;
pthread_mutex_t lazyfree_objects_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Return the number of currently pending objects to free. */
size_t lazyfreeGetPendingObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfree_objects,aux);
    return aux;
}

/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is compoesd of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actually logical composed of multiple
 * elements.
 *
 * For lists the function returns the number of elements in the quicklist
 * representing the list. */
// 评估释放一个对象的操作量级，并不是精确计算释放一个对象的操作次数，而是一定比例的呈现。
// 规则是：由当初在构造此对象时分配内存的次数来决定释放此对象的操作次数。
// 对于聚合类型的对象:例如链表，字典，只需要以节点个数代表即可
// 对于字符串，则认为是1次;
// 对于其他由一次性分配内存构成的类型，则返回1
size_t lazyfreeGetFreeEffort(robj *obj) {
    if (obj->type == OBJ_LIST) {
        quicklist *ql = obj->ptr;
        return ql->len;
    } else if (obj->type == OBJ_SET && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else if (obj->type == OBJ_ZSET && obj->encoding == OBJ_ENCODING_SKIPLIST){
        zset *zs = obj->ptr;
        return zs->zsl->length;
    } else if (obj->type == OBJ_HASH && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else {
        return 1; /* Everything else is a single allocation. */
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB.
 * If there are enough allocations to free the value object may be put into
 * a lazy free list instead of being freed synchronously. The lazy free list
 * will be reclaimed in a different bio.c thread. */
// 同步删除主字典指定的key对应节点以及节点中的key
// 内部会同步删除expire字典里的key相关数据
// 根据value的操作成本决定是同步删除value数据，还是异步删除value
// 删除成功返回1，未执行删除动作返回0
#define LAZYFREE_THRESHOLD 64 //异步释放阈值，释放时的操作次数超过此阈值则走异步释放，否则直接同步释放内存
int dbAsyncDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    // expire字典里的节点里的key指向主字典里的实际内存，value存放过期时刻
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);

    /* If the value is composed of a few allocations, to free in a lazy way
     * is actually just slower... So under a certain limit we just free
     * the object synchronously. */
    // 从主字典里找到此key，并从字典中摘除但不释放内存，只是获取节点指针
    dictEntry *de = dictUnlink(db->dict,key->ptr);
    // 已找到key对应的节点指针
    if (de) {
        robj *val = dictGetVal(de);
        // 估算要释放此value对象的操作次数
        size_t free_effort = lazyfreeGetFreeEffort(val);

        /* If releasing the object is too much work, do it in the background
         * by adding the object to the lazy free list.
         * Note that if the object is shared, to reclaim it now it is not
         * possible. This rarely happens, however sometimes the implementation
         * of parts of the Redis core may call incrRefCount() to protect
         * objects, and then call dbDelete(). In this case we'll fall
         * through and reach the dictFreeUnlinkedEntry() call, that will be
         * equivalent to just calling decrRefCount(). */
        if (free_effort > LAZYFREE_THRESHOLD && val->refcount == 1) {
            //超过阈值且当前value的引用计数为1：表示本次需要实际释放value值
            atomicIncr(lazyfree_objects,1);
            // 交由后台独立的释放线程来异步释放此value对象
            bioCreateBackgroundJob(BIO_LAZY_FREE,val,NULL,NULL);
            // 将value指针置为null，表示此value以被释放
            dictSetVal(db->dict,de,NULL);
        }
    }

    // 执行到此，de中的value字段如果太大，会走异步free，被置为NULL；如果为非NULL，则需要本次同步free
    /* Release the key-val pair, or just the key if we set the val
     * field to NULL in order to lazy free it later. */
    if (de) {
        // 主字典释放节点自身的元素
        dictFreeUnlinkedEntry(db->dict,de);
        if (server.cluster_enabled) slotToKeyDel(key);
        // 释放内存
        return 1;
    } else {
        //未释放内存
        return 0;
    }
}

/* Free an object, if the object is huge enough, free it in async way. */
// 可能采用异步方式释放对象内存
void freeObjAsync(robj *o) {
    size_t free_effort = lazyfreeGetFreeEffort(o);
    if (free_effort > LAZYFREE_THRESHOLD && o->refcount == 1) {
        // 异步释放对象
        atomicIncr(lazyfree_objects,1);
        bioCreateBackgroundJob(BIO_LAZY_FREE,o,NULL,NULL);
    } else {
        // 引用计数-1
        decrRefCount(o);
    }
}

/* Empty a Redis DB asynchronously. What the function does actually is to
 * create a new empty set of hash tables and scheduling the old ones for
 * lazy freeing. */
// 异步清空db库:主字典与过期字典
void emptyDbAsync(redisDb *db) {
    dict *oldht1 = db->dict, *oldht2 = db->expires;
    // 创建两个新的字典
    db->dict = dictCreate(&dbDictType,NULL);
    db->expires = dictCreate(&keyptrDictType,NULL);
    // 只记录主字典中的待删除元素个数,无需记录过期字典里的元素个数
    atomicIncr(lazyfree_objects,dictSize(oldht1));
    // 旧的字典进入线程异步清除
    bioCreateBackgroundJob(BIO_LAZY_FREE,NULL,oldht1,oldht2);
}

/* Empty the slots-keys map of Redis CLuster by creating a new empty one
 * and scheduiling the old for lazy freeing. */
// 异步清空slot-->key的映射关系
void slotToKeyFlushAsync(void) {
    rax *old = server.cluster->slots_to_keys;
    // 构造一个新的
    server.cluster->slots_to_keys = raxNew();
    // 同时清空槽位计数器
    memset(server.cluster->slots_keys_count,0,
           sizeof(server.cluster->slots_keys_count));
    // 记录待异步释放的对象个数
    atomicIncr(lazyfree_objects,old->numele);
    // 提交异步清除内存任务
    bioCreateBackgroundJob(BIO_LAZY_FREE,NULL,NULL,old);
}

/* Release objects from the lazyfree thread. It's just decrRefCount()
 * updating the count of objects to release. */
// 供后台线程调用,释放对象
void lazyfreeFreeObjectFromBioThread(robj *o) {
    decrRefCount(o);
    atomicDecr(lazyfree_objects,1);
}

/* Release a database from the lazyfree thread. The 'db' pointer is the
 * database which was substitutied with a fresh one in the main thread
 * when the database was logically deleted. 'sl' is a skiplist used by
 * Redis Cluster in order to take the hash slots -> keys mapping. This
 * may be NULL if Redis Cluster is disabled. */
// 供后台线程调用,释放db里的主字典与过期字典
void lazyfreeFreeDatabaseFromBioThread(dict *ht1, dict *ht2) {
    size_t numkeys = dictSize(ht1);
    dictRelease(ht1);
    dictRelease(ht2);
    atomicDecr(lazyfree_objects,numkeys);
}

/* Release the skiplist mapping Redis Cluster keys to slots in the
 * lazyfree thread. */
// 供后台线程调用,释放rax类型的数据:目前只有slot-->key的映射关系数据
void lazyfreeFreeSlotsMapFromBioThread(rax *rt) {
    size_t len = rt->numele;
    raxFree(rt);
    atomicDecr(lazyfree_objects,len);
}
