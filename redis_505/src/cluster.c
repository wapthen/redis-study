/* Redis Cluster implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "cluster.h"
#include "endianconv.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <math.h>

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. */
// 注意:如果配置文件中没有cluster_announce_ip的话,则myself里的ip为null,
// 会在收到第一个meet消息时由tcp套接字获取到自身的ip数据
clusterNode *myself = NULL;

clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode *n, int slot);
sds clusterGenNodesDescription(int filter);
clusterNode *clusterLookupNode(const char *name);
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);
int clusterNodeSetSlotBit(clusterNode *n, int slot);
void clusterSetMaster(clusterNode *n);
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsMaster(clusterNode *n);
void clusterDelNode(clusterNode *delnode);
sds representClusterNodeFlags(sds ci, uint16_t flags);
uint64_t clusterGetMaxEpoch(void);
int clusterBumpConfigEpochWithoutConsensus(void);
void moduleCallClusterReceivers(const char *sender_id, uint64_t module_id, uint8_t type, const unsigned char *payload, uint32_t len);

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), C_ERR is returned.
 * If the configuration was loaded from the file, C_OK is returned. */
// 从磁盘上的node.cnf加载集群配置文件
// 如无此文件或者文件内容为空,则返回错误
// node-name ip:port@cluster-port slave master-name ping-sent pong-receive 
// 如果master-name这一列是-，表示当前节点为主节点，
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    struct stat sb;
    char *line;
    int maxline, j;

    if (fp == NULL) {
        if (errno == ENOENT) {
            return C_ERR;
        } else {
            serverLog(LL_WARNING,
                "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
            exit(1);
        }
    }

    /* Check if the file is zero-length: if so return C_ERR to signal
     * we have to write the config. */
    // 文件为空时返回错误
    if (fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        fclose(fp);
        return C_ERR;
    }

    /* Parse the file. Note that single lines of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case, half of the Redis slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * To simplify we allocate 1024+CLUSTER_SLOTS*128 bytes per line. */
    // 简化处理,每一行表示一个节点的信息以及对应负责处理的槽位,最坏的情况是半数的槽位信息出现在同一行
    maxline = 1024+CLUSTER_SLOTS*128;
    line = zmalloc(maxline);
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n, *master;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        // 跳过空行
        if (line[0] == '\n' || line[0] == '\0') continue;

        /* Split the line into arguments for processing. */
        // 以空格分割当前行数据
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by Redis. */
        // 开头为 vars 的行数据为特殊数据,关键字段为:currentEpoch与lastVoteEpoch
        //例如 vars currentEpoch 111111 lastVoteEpoch 22222
        if (strcasecmp(argv[0],"vars") == 0) {
            for (j = 1; j < argc; j += 2) {
                if (strcasecmp(argv[j],"currentEpoch") == 0) {
                    server.cluster->currentEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else if (strcasecmp(argv[j],"lastVoteEpoch") == 0) {
                    server.cluster->lastVoteEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else {
                    serverLog(LL_WARNING,
                        "Skipping unknown cluster config variable '%s'",
                        argv[j]);
                }
            }
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Regular config lines have at least eight fields */
        if (argc < 8) goto fmterr;

        /* Create this node if it does not exist */
        n = clusterLookupNode(argv[0]);
        if (!n) {
            n = createClusterNode(argv[0],0);
            clusterAddNode(n);
        }
        /* Address and port */
        if ((p = strrchr(argv[1],':')) == NULL) goto fmterr;
        *p = '\0';
        // 确保将argv[1]里的末尾空字符也拷贝到目标缓冲区里
        memcpy(n->ip,argv[1],strlen(argv[1])+1);
        // 开始切割端口号字段
        char *port = p+1;
        char *busp = strchr(port,'@');
        if (busp) {
            *busp = '\0';
            busp++;
        }
        n->port = atoi(port);
        /* In older versions of nodes.conf the "@busport" part is missing.
         * In this case we set it to the default offset of 10000 from the
         * base port. */
        // 获取集群bus通信端口
        n->cport = busp ? atoi(busp) : n->port + CLUSTER_PORT_INCR;

        /* Parse flags */
        p = s = argv[2];
        while(p) {
            p = strchr(s,',');
            if (p) *p = '\0';
            if (!strcasecmp(s,"myself")) {
                serverAssert(server.cluster->myself == NULL);
                myself = server.cluster->myself = n;
                n->flags |= CLUSTER_NODE_MYSELF;
            } else if (!strcasecmp(s,"master")) {
                n->flags |= CLUSTER_NODE_MASTER;
            } else if (!strcasecmp(s,"slave")) {
                n->flags |= CLUSTER_NODE_SLAVE;
            } else if (!strcasecmp(s,"fail?")) {
                n->flags |= CLUSTER_NODE_PFAIL;
            } else if (!strcasecmp(s,"fail")) {
                n->flags |= CLUSTER_NODE_FAIL;
                n->fail_time = mstime();
            } else if (!strcasecmp(s,"handshake")) {
                n->flags |= CLUSTER_NODE_HANDSHAKE;
            } else if (!strcasecmp(s,"noaddr")) {
                n->flags |= CLUSTER_NODE_NOADDR;
            } else if (!strcasecmp(s,"nofailover")) {
                n->flags |= CLUSTER_NODE_NOFAILOVER;
            } else if (!strcasecmp(s,"noflags")) {
                /* nothing to do */
            } else {
                serverPanic("Unknown flag in redis cluster config file");
            }
            if (p) s = p+1;
        }

        /* Get master if any. Set the master and populate master's
         * slave list. */
        if (argv[3][0] != '-') {
            // 此位置存放的是当前节点对应的主节点name名称
            master = clusterLookupNode(argv[3]);
            if (!master) {
                master = createClusterNode(argv[3],0);
                clusterAddNode(master);
            }

            // 主备节点建立互指关系
            n->slaveof = master;
            clusterNodeAddSlave(master,n);
        }

        /* Set ping sent / pong received timestamps */
        if (atoi(argv[4])) n->ping_sent = mstime();
        if (atoi(argv[5])) n->pong_received = mstime();

        /* Set configEpoch for this node. */
        n->configEpoch = strtoull(argv[6],NULL,10);

        /* Populate hash slots served by this instance. */
        for (j = 8; j < argc; j++) {
            int start, stop;

            if (argv[j][0] == '[') {
                /* Here we handle migrating / importing slots */
                int slot;
                char direction;
                clusterNode *cn;

                p = strchr(argv[j],'-');
                serverAssert(p != NULL);
                *p = '\0';
                direction = p[1]; /* Either '>' or '<' */
                slot = atoi(argv[j]+1);
                if (slot < 0 || slot >= CLUSTER_SLOTS) goto fmterr;
                p += 3;
                cn = clusterLookupNode(p);
                if (!cn) {
                    cn = createClusterNode(p,0);
                    clusterAddNode(cn);
                }
                if (direction == '>') {
                    server.cluster->migrating_slots_to[slot] = cn;
                } else {
                    server.cluster->importing_slots_from[slot] = cn;
                }
                continue;
            } else if ((p = strchr(argv[j],'-')) != NULL) {
                *p = '\0';
                start = atoi(argv[j]);
                stop = atoi(p+1);
            } else {
                start = stop = atoi(argv[j]);
            }
            if (start < 0 || start >= CLUSTER_SLOTS) goto fmterr;
            if (stop < 0 || stop >= CLUSTER_SLOTS) goto fmterr;
            while(start <= stop) clusterAddSlot(n, start++);
        }

        sdsfreesplitres(argv,argc);
    }
    /* Config sanity check */
    // 健全检查:配置文件加载完毕后,必须有一个节点是表示自身 
    if (server.cluster->myself == NULL) goto fmterr;

    zfree(line);
    fclose(fp);

    serverLog(LL_NOTICE,"Node configuration loaded, I'm %.40s", myself->name);

    /* Something that should never happen: currentEpoch smaller than
     * the max epoch found in the nodes configuration. However we handle this
     * as some form of protection against manual editing of critical files. */
    if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
        server.cluster->currentEpoch = clusterGetMaxEpoch();
    }
    return C_OK;

fmterr:
    serverLog(LL_WARNING,
        "Unrecoverable error: corrupted cluster config file.");
    zfree(line);
    if (fp) fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. */
// 入参表示是否强制刷盘,1表示强制阻塞式刷盘 0表示无需刷盘
// 注意 本函数需确保原子性写文件, 即最后配置文件要么是旧数据,要么是新数据. 
// 为避免在写文件时进程崩溃导致无法保证原子性.
// 本函数将所有内容调用一次write全部写入,如果旧数据字节数多余新数据,则先以尾部填充'/n'确保完全覆盖旧数据,之后再执行截断文件操作
int clusterSaveConfig(int do_fsync) {
    sds ci;
    size_t content_size;
    struct stat sb;
    int fd;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

    /* Get the nodes description and concatenate our "vars" directive to
     * save currentEpoch and lastVoteEpoch. */
    ci = clusterGenNodesDescription(CLUSTER_NODE_HANDSHAKE);
    ci = sdscatprintf(ci,"vars currentEpoch %llu lastVoteEpoch %llu\n",
        (unsigned long long) server.cluster->currentEpoch,
        (unsigned long long) server.cluster->lastVoteEpoch);
    // 获取实际要写入的数据的字节数
    content_size = sdslen(ci);

    if ((fd = open(server.cluster_configfile,O_WRONLY|O_CREAT,0644))
        == -1) goto err;

    /* Pad the new payload if the existing file length is greater. */
    if (fstat(fd,&sb) != -1) {
        if (sb.st_size > (off_t)content_size) {
            // 为确保单次write全部覆盖旧数据,
            ci = sdsgrowzero(ci,sb.st_size);
            // 将新数据尾部填充回车符
            memset(ci+content_size,'\n',sb.st_size-content_size);
        }
    }
    // TODO 此处可能出现部分写的情况,导致文件中的配置数据可能部分旧/部分新.
    if (write(fd,ci,sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
    if (do_fsync) {
        // 开始fsync操作,所以将该预约标记清除.
        server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
        // 阻塞式的强制刷盘:包含data与meta
        fsync(fd);
    }

    /* Truncate the file if needed to remove the final \n padding that
     * is just garbage. */
    // 此时的ci可能有尾部填充的回车符,跟实际数据长度不同,则执行文件截断操作,确保文件保存的是真实配置数据
    if (content_size != sdslen(ci) && ftruncate(fd,content_size) == -1) {
        // 此处失败也是无损的
        /* ftruncate() failing is not a critical error. */
    }
    close(fd);
    sdsfree(ci);
    return 0;

err:
    if (fd != -1) close(fd);
    sdsfree(ci);
    return -1;
}


// 保存配置文件,如果失败则进程退出
void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == -1) {
        serverLog(LL_WARNING,"Fatal: can't update cluster config file.");
        exit(1);
    }
}

/* Lock the cluster config using flock(), and leaks the file descritor used to
 * acquire the lock so that the file will be locked forever.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success C_OK is returned, otherwise an error is logged and
 * the function returns C_ERR to signal a lock was not acquired. */
/**
 *
 * There are several types of advisory locks available in Linux:
 * 
 * BSD locks (flock)
 * POSIX record locks (fcntl, lockf)
 * Open file description locks (fcntl)
 * All locks except the lockf function are reader-writer locks, i.e. support exclusive and shared modes.
 * 
 * BSD locks (flock)
 * The simplest and most common file locks are provided by flock(2).
 * 
 * Features:
 * 
 * not specified in POSIX, but widely available on various Unix systems
 * always lock the entire file
 * associated with a file object
 * do not guarantee atomic switch between the locking modes (exclusive and shared)
 * up to Linux 2.6.11, didn’t work on NFS; since Linux 2.6.12, flock() locks on NFS are emulated using fcntl() POSIX record byte-range locks on the entire file (unless the emulation is disabled in the NFS mount options)
 * The lock acquisition is associated with a file object, i.e.:
 * 
 * duplicated file descriptors, e.g. created using dup2 or fork, share the lock acquisition;
 * independent file descriptors, e.g. created using two open calls (even for the same file), don’t share the lock acquisition;
 * This means that with BSD locks, threads or processes can’t be synchronized on the same or duplicated file descriptor,
 *  but nevertheless, both can be synchronized on independent file descriptors.


 */
// 打开指定的配置文件,并使用flock函数锁住整个文件,并有意的泄露文件句柄,这样就可以一直持有文件锁直至进程终结
// 因为在close文件,那么当前进程在此文件上加的文件锁均会被系统自动释放.
// 避免同机不同的redis进程使用同一份磁盘上的配置文件
int clusterLockConfig(char *filename) {
/* flock() does not exist on Solaris
 * and a fcntl-based solution won't help, as we constantly re-open that file,
 * which will release _all_ locks anyway
 */
#if !defined(__sun)
    /* To lock it, we need to open the file in a way it is created if
     * it does not exist, otherwise there is a race condition with other
     * processes. */
    int fd = open(filename,O_WRONLY|O_CREAT,0644);
    if (fd == -1) {
        serverLog(LL_WARNING,
            "Can't open %s in order to acquire a lock: %s",
            filename, strerror(errno));
        return C_ERR;
    }

    if (flock(fd,LOCK_EX|LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            serverLog(LL_WARNING,
                 "Sorry, the cluster configuration file %s is already used "
                 "by a different Redis Cluster node. Please make sure that "
                 "different nodes use different cluster configuration "
                 "files.", filename);
        } else {
            serverLog(LL_WARNING,
                "Impossible to lock %s: %s", filename, strerror(errno));
        }
        close(fd);
        return C_ERR;
    }
    /* Lock acquired: leak the 'fd' by not closing it, so that we'll retain the
     * lock to the file as long as the process exists. */
#endif /* __sun */

    return C_OK;
}

/* Some flags (currently just the NOFAILOVER flag) may need to be updated
 * in the "myself" node based on the current configuration of the node,
 * that may change at runtime via CONFIG SET. This function changes the
 * set of flags in myself->flags accordingly. */
// 校验并更新自身节点的标记位:CLUSTER_NODE_NOFAILOVER
// 因为该配置是可以在运行期间由config set命令进行动态设置
// 所以需要周期性检查并保存到cluster的配置文件中，对应文件中的标记是nofailover
void clusterUpdateMyselfFlags(void) {
    int oldflags = myself->flags;
    int nofailover = server.cluster_slave_no_failover ?
                     CLUSTER_NODE_NOFAILOVER : 0;
    // 将原标记位清除
    myself->flags &= ~CLUSTER_NODE_NOFAILOVER;
    // 设置最新的值
    myself->flags |= nofailover;
    // 确认标记位数值是否有变化
    if (myself->flags != oldflags) {
        // 需异步更新保存配置文件
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE);
    }
}

// 初始化集群数据结构:clusterState
void clusterInit(void) {
    // 表示是否需要保存配置文件
    int saveconf = 0;

    server.cluster = zmalloc(sizeof(clusterState));
    server.cluster->myself = NULL;
    server.cluster->currentEpoch = 0;
    server.cluster->state = CLUSTER_FAIL;
    server.cluster->size = 1;
    server.cluster->todo_before_sleep = 0;
    server.cluster->nodes = dictCreate(&clusterNodesDictType,NULL);
    server.cluster->nodes_black_list =
        dictCreate(&clusterNodesBlackListDictType,NULL);
    server.cluster->failover_auth_time = 0;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_rank = 0;
    server.cluster->failover_auth_epoch = 0;
    server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
    server.cluster->lastVoteEpoch = 0;
    // 统计集群交互信息字段清零
    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
        server.cluster->stats_bus_messages_sent[i] = 0;
        server.cluster->stats_bus_messages_received[i] = 0;
    }
    server.cluster->stats_pfail_nodes = 0;
    memset(server.cluster->slots,0, sizeof(server.cluster->slots));
    clusterCloseAllSlots();

    /* Lock the cluster config file to make sure every node uses
     * its own nodes.conf. */
    // 加文件锁占用磁盘上的cluster配置文件
    if (clusterLockConfig(server.cluster_configfile) == C_ERR)
        exit(1);

    /* Load or create a new nodes configuration. */
    if (clusterLoadConfig(server.cluster_configfile) == C_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        // 加载配置文件失败,表示当前磁盘上没有对应的集群配置文件,后续需要save一份
        // 创建一个表示当前自身的节点结构
        myself = server.cluster->myself =
            createClusterNode(NULL,CLUSTER_NODE_MYSELF|CLUSTER_NODE_MASTER);
        serverLog(LL_NOTICE,"No cluster configuration found, I'm %.40s",
            myself->name);
        clusterAddNode(myself);
        saveconf = 1;
    }
    // 至此, node字典里至少有一个表示自身的node节点

    // 首次启动cluster集群模式,需要在磁盘保存集群配置文件.
    if (saveconf) clusterSaveConfigOrDie(1);

    /* We need a listening TCP port for our cluster messaging needs. */
    server.cfd_count = 0;

    /* Port sanity check II
     * The other handshake port check is triggered too late to stop
     * us from trying to use a too-high cluster port number. */
    // 校验一下cluster的端口号范围是否会超过65535,因为cluster端口号是在redis端口号基础上加上10000
    if (server.port > (65535-CLUSTER_PORT_INCR)) {
        serverLog(LL_WARNING, "Redis port number too high. "
                   "Cluster communication port is 10,000 port "
                   "numbers higher than your Redis port. "
                   "Your Redis port number must be "
                   "lower than 55535.");
        exit(1);
    }

    // 监听cluster端口号
    if (listenToPort(server.port+CLUSTER_PORT_INCR,
        server.cfd,&server.cfd_count) == C_ERR)
    {
        exit(1);
    } else {
        int j;
        // 注册cluster句柄对应的回调函数
        for (j = 0; j < server.cfd_count; j++) {
            if (aeCreateFileEvent(server.el, server.cfd[j], AE_READABLE,
                clusterAcceptHandler, NULL) == AE_ERR)
                    serverPanic("Unrecoverable error creating Redis Cluster "
                                "file event.");
        }
    }

    /* The slots -> keys map is a radix tree. Initialize it here. */
    server.cluster->slots_to_keys = raxNew();
    memset(server.cluster->slots_keys_count,0,
           sizeof(server.cluster->slots_keys_count));

    /* Set myself->port / cport to my listening ports, we'll just need to
     * discover the IP address via MEET messages. */
    myself->port = server.port;
    myself->cport = server.port+CLUSTER_PORT_INCR;
    if (server.cluster_announce_port)
        myself->port = server.cluster_announce_port;
    if (server.cluster_announce_bus_port)
        myself->cport = server.cluster_announce_bus_port;

    server.cluster->mf_end = 0;
    resetManualFailover();
    clusterUpdateMyselfFlags();
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forget.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a slave, it turns into a master.
 * 5) Only for hard reset: a new Node ID is generated.
 * 6) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 7) The new configuration is saved and the cluster state updated.
 * 8) If the node was a slave, the whole data set is flushed away. */
// 重置本节点记录的整个集群数据,如果是hard则还需清除本节点id以及纪元数据
// 如果本节点目前是备节点时,则丢弃本节点的所有既有数据
void clusterReset(int hard) {
    dictIterator *di;
    dictEntry *de;
    int j;

    /* Turn into master. */
    // 对于当前节点是备节点,重置后置为主节点
    if (nodeIsSlave(myself)) {
        clusterSetNodeAsMaster(myself);
        replicationUnsetMaster();
        // 备节点需要清除所有数据
        emptyDb(-1,EMPTYDB_NO_FLAGS,NULL);
    }

    /* Close slots, reset manual failover state. */
    // 清除所有槽位的迁入迁出信息
    clusterCloseAllSlots();
    resetManualFailover();

    /* Unassign all the slots. */
    // 删除槽位与节点的相互映射关系
    for (j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Forget all the nodes, but myself. */
    // 遍历字典删除所有节点,除了自己
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == myself) continue;
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    /* Hard reset only: set epochs to 0, change node ID. */
    if (hard) {
        // 硬重置的情况下,需将节点id与纪元清空,并且将自身节点id改名再放入集群中
        sds oldname;

        server.cluster->currentEpoch = 0;
        server.cluster->lastVoteEpoch = 0;
        myself->configEpoch = 0;
        serverLog(LL_WARNING, "configEpoch set to 0 via CLUSTER RESET HARD");

        /* To change the Node ID we need to remove the old name from the
         * nodes table, change the ID, and re-add back with new name. */
        // 从集群中删除自己
        oldname = sdsnewlen(myself->name, CLUSTER_NAMELEN);
        dictDelete(server.cluster->nodes,oldname);
        sdsfree(oldname);
        // 重新生成一个新的id名称
        getRandomHexChars(myself->name, CLUSTER_NAMELEN);
        // 加入集群字典里
        clusterAddNode(myself);
        serverLog(LL_NOTICE,"Node hard reset, now I'm %.40s", myself->name);
    }

    /* Make sure to persist the new config and update the state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE|
                         CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */
// 基于指定的节点信息创建一个对应的link结构体
clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->ctime = mstime();
    link->sndbuf = sdsempty();
    link->rcvbuf = sdsempty();
    link->node = node;
    link->fd = -1;
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
// 释放指定的link数据
void freeClusterLink(clusterLink *link) {
    if (link->fd != -1) {
        // 将本link的tcp套接字从reactor注册器中移除
        aeDeleteFileEvent(server.el, link->fd, AE_READABLE|AE_WRITABLE);
    }
    sdsfree(link->sndbuf);
    sdsfree(link->rcvbuf);
    if (link->node)
        // 将node中的link置空
        link->node->link = NULL;
    close(link->fd);
    // 释放本link
    zfree(link);
}

// client链接cluster监听端口时的回调函数
#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;//一次回调函数最多连续建立1000个新link 链接,避免消耗过多时间
    char cip[NET_IP_STR_LEN];
    clusterLink *link;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    /* If the server is starting up, don't accept cluster connections:
     * UPDATE messages may interact with the database content. */
    // 如果本机为主节点且还处于启动加载中,则暂不接受bus client链接
    if (server.masterhost == NULL && server.loading) return;

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            // 对于非阻塞fd进行的accept()如果没有合适的套接字,会立即返回并将errno置为EWOULDBLOCK或者EAGAIN
            if (errno != EWOULDBLOCK)
                serverLog(LL_VERBOSE,
                    "Error accepting cluster node: %s", server.neterr);
            return;
        }
        // 新的link所用套接字属性置为非阻塞与nodelay
        anetNonBlock(NULL,cfd);
        anetEnableTcpNoDelay(NULL,cfd);

        /* Use non-blocking I/O for cluster messages. */
        serverLog(LL_VERBOSE,"Accepted cluster node %s:%d", cip, cport);

        // 构造一个新的link并将指向node为空,表明该link是被动响应使用
        // 即：在通信中，本机不主动发起通信，而只是应答外部节点的通信。
        link = createClusterLink(NULL);
        link->fd = cfd;
        // 将此新的套接字注册到reactor注册器中
        aeCreateFileEvent(server.el,cfd,AE_READABLE,clusterReadHandler,link);
    }
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
// 根据入参key计算槽位所用的hash值
unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    // 找到第一个{
    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    // 如果没有{,那么就依据整个key进行计算
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    // 已经找到{,开始找第一个}
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing between {} ? Hash the whole key. */
    // 没有找到},或者 在紧挨着{之后就有一个}即两者中间无任何字符, 那么就依据整个key进行计算
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. */
// 创建一个节点数据结构, 如果nodename为空 则主动创建一个新的nodename
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));

    if (nodename)
        memcpy(node->name, nodename, CLUSTER_NAMELEN);
    else
        getRandomHexChars(node->name, CLUSTER_NAMELEN);
    node->ctime = mstime();
    node->configEpoch = 0;
    node->flags = flags;
    memset(node->slots,0,sizeof(node->slots));
    node->numslots = 0;
    node->numslaves = 0;
    node->slaves = NULL;
    node->slaveof = NULL;
    node->ping_sent = node->pong_received = 0;
    node->fail_time = 0;
    node->link = NULL;
    memset(node->ip,0,sizeof(node->ip));
    node->port = 0;
    node->cport = 0;
    node->fail_reports = listCreate();
    node->voted_time = 0;
    node->orphaned_time = 0;
    node->repl_offset_time = 0;
    node->repl_offset = 0;
    listSetFreeMethod(node->fail_reports,zfree);
    return node;
}

/* This function is called every time we get a failure report from a node.
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. */
// 本机记录sender发来的举报failing节点是失败or疑似失败
// sender必须是外部主节点, failing不能为本机节点
// 返回0表示该失败报告已存在,只更新了时间戳；
// 返回1表示成功新增一份失败报告;
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {
    list *l = failing->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* If a failure report from the same sender already exists, just update
     * the timestamp. */
    // 查重,如果已经存在则更新时间戳即可
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) {
            fr->time = mstime();
            return 0;
        }
    }

    /* Otherwise create a new report. */
    // 至此需要创建一个新的失败报告节点添加到list的尾部
    fr = zmalloc(sizeof(*fr));
    fr->node = sender;
    fr->time = mstime();
    listAddNodeTail(l,fr);
    return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. */
// 清除过期的失败报告, 过期是指失败报告的太久,已不可信. 过期阈值为2*cluster_node_timeout
void clusterNodeCleanupFailureReports(clusterNode *node) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;
    mstime_t maxtime = server.cluster_node_timeout *
                     CLUSTER_FAIL_REPORT_VALIDITY_MULT;
    mstime_t now = mstime();

    // 将迭代器指向list的头结点,准备开始逐一遍历每个节点数据
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        // 对于过期的失败报告,直接删除点此节点
        if (now - fr->time > maxtime) listDelNode(l,ln);
    }
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. */
// 删除sender举报node的失败报告
// 内部会主动进行一次清除过期的失败报告过程.
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* Search for a failure report from this sender. */
    // 将迭代器指向失败报告list的头结点,逐个遍历比较
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) break;
    }
    // 未找到sender节点发送的失败报告数据则直接返回
    if (!ln) return 0; /* No failure report from this sender. */

    /* Remove the failure report. */
    // 从list中删除此节点数据
    listDelNode(l,ln);
    // 趁此机会清除一遍过期的失败报告节点
    clusterNodeCleanupFailureReports(node);
    return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. */
// 统计指定节点的失败报告的个数
// 内部首先会进行一次清除过期的失败报告过程
int clusterNodeFailureReportsCount(clusterNode *node) {
    // 清除一遍过期的失败报告数据
    clusterNodeCleanupFailureReports(node);
    // 返回失败报告list的节点个数
    return listLength(node->fail_reports);
}

// 将slave节点从master节点里移除
// 操作完毕后,如果master名下已没有任何备节点,则将master去掉CLUSTER_NODE_MIGRATE_TO标记
int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
    int j;

    // 依次遍历每一个备节点,通过节点指针是否相同来判定
    // 因为存储方式为数值, 所以不会重新缩小数组内存,只是移动成员位置+修改数组有效成员个数变量
    for (j = 0; j < master->numslaves; j++) {
        if (master->slaves[j] == slave) {
            // 备节点指针数组从中间移除的话,需要将后续所有的数据前移至当前位置
            // 并不是最后一个备节点,因为如果是最后一个成员,只需要修改有效成员个数即可
            if ((j+1) < master->numslaves) {
                int remaining_slaves = (master->numslaves - j) - 1;
                memmove(master->slaves+j,master->slaves+(j+1),
                        (sizeof(*master->slaves) * remaining_slaves));
            }
            master->numslaves--;
            if (master->numslaves == 0)
                // 主节点无任何备节点,那么标记一下当前主节点已不适合做复制操作
                master->flags &= ~CLUSTER_NODE_MIGRATE_TO;
            return C_OK;
        }
    }
    return C_ERR;
}

// 向master节点增加一个备节点,并标记该master有资格进行复制CLUSTER_NODE_MIGRATE_TO
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave) {
    int j;

    /* If it's already a slave, don't add it again. */
    // 查重
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] == slave) return C_ERR;
    // 此新的备节点需要加入备节点数组的尾部
    master->slaves = zrealloc(master->slaves,
        sizeof(clusterNode*)*(master->numslaves+1));
    master->slaves[master->numslaves] = slave;
    master->numslaves++;
    master->flags |= CLUSTER_NODE_MIGRATE_TO;
    return C_OK;
}

// 统计主节点名下有多少非FAIL的备节点
int clusterCountNonFailingSlaves(clusterNode *n) {
    int j, okslaves = 0;

    // 逐个判定备节点是否处于FAIL失败状态
    for (j = 0; j < n->numslaves; j++)
        if (!nodeFailed(n->slaves[j])) okslaves++;
    return okslaves;
}


/* Low level cleanup of the node structure. Only called by clusterDelNode(). */
// 释放指定node节点,只能被clusterDelNode函数调用
// 内部并没有移除slot->node的映射关系
void freeClusterNode(clusterNode *n) {
    sds nodename;
    int j;

    /* If the node has associated slaves, we have to set
     * all the slaves->slaveof fields to NULL (unknown). */
    // 如果此节点有备节点,则将备节点数组各个元素置为NULL
    for (j = 0; j < n->numslaves; j++)
        n->slaves[j]->slaveof = NULL;

    /* Remove this node from the list of slaves of its master. */
    // 如果此节点是备节点,那么从主节点中移除此备节点数据
    if (nodeIsSlave(n) && n->slaveof) clusterNodeRemoveSlave(n->slaveof,n);

    /* Unlink from the set of nodes. */
    // 构造一个临时的新nodeid,用于从全局的集群节点字典中删除
    nodename = sdsnewlen(n->name, CLUSTER_NAMELEN);
    serverAssert(dictDelete(server.cluster->nodes,nodename) == DICT_OK);
    sdsfree(nodename);

    /* Release link and associated data structures. */
    // 释放此节点对应的link数据
    if (n->link) freeClusterLink(n->link);
    // 释放此节点的失败报告list
    listRelease(n->fail_reports);
    // 释放此节点的备节点指针数组, 内部会判定是否为NULL
    zfree(n->slaves);
    zfree(n);
}

/* Add a node to the nodes hash table */
// 将一个新节点添加到全局的集群字典里
// 如果该节点已存在，则返回错误
int clusterAddNode(clusterNode *node) {
    int retval;

    retval = dictAdd(server.cluster->nodes,
            sdsnewlen(node->name,CLUSTER_NAMELEN), node);
    return (retval == DICT_OK) ? C_OK : C_ERR;
}

/* Remove a node from the cluster. The functio performs the high level
 * cleanup, calling freeClusterNode() for the low level cleanup.
 * Here we do the following:
 *
 * 1) Mark all the slots handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node and referenced by
 *    other nodes.
 * 3) Free the node with freeClusterNode() that will in turn remove it
 *    from the hash table and from the list of slaves of its master, if
 *    it is a slave node.
 */
// 集群中删除指定的节点
void clusterDelNode(clusterNode *delnode) {
    int j;
    dictIterator *di;
    dictEntry *de;

    // 从全局的槽数组中删除指定节点负责的槽位信息
    /* 1) Mark slots as unassigned. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 逐项遍历迁入槽位数组,发现由此节点服务的槽位则置为NULL
        if (server.cluster->importing_slots_from[j] == delnode)
            server.cluster->importing_slots_from[j] = NULL;
        // 逐项遍历迁出槽位数组,发现由此节点服务的槽位则置为NULL
        if (server.cluster->migrating_slots_to[j] == delnode)
            server.cluster->migrating_slots_to[j] = NULL;
        // 将槽位图 与 此节点 的相互映射关系删除
        if (server.cluster->slots[j] == delnode)
            clusterDelSlot(j);
    }

    /* 2) Remove failure reports. */
    // 遍历集群里的每个节点里的失败报告,移除有该指定节点发送的所有失败报告
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == delnode) continue;
        clusterNodeDelFailureReport(node,delnode);
    }
    dictReleaseIterator(di);

    /* 3) Free the node, unlinking it from the cluster. */
    // 释放此节点自身各项数据
    freeClusterNode(delnode);
}

/* Node lookup by name */
// 从本机集群字典中根据节点名称查找获取节点
clusterNode *clusterLookupNode(const char *name) {
    sds s = sdsnewlen(name, CLUSTER_NAMELEN);
    dictEntry *de;

    de = dictFind(server.cluster->nodes,s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
// 对指定node节点修改为新节点表示nodename
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    // 构造一个临时节点名称,从字典中删除
    sds s = sdsnewlen(node->name, CLUSTER_NAMELEN);

    serverLog(LL_DEBUG,"Renaming node %.40s into %.40s",
        node->name, newname);
    // 从字典里删除此key对应的节点数据
    retval = dictDelete(server.cluster->nodes, s);
    sdsfree(s);
    serverAssert(retval == DICT_OK);
    // 将新的名称拷贝如node节点中
    memcpy(node->name, newname, CLUSTER_NAMELEN);
    // 重新将此节点加入到集群字典中
    clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER config epoch handling
 * -------------------------------------------------------------------------- */

/* Return the greatest configEpoch found in the cluster, or the current
 * epoch if greater than any node configEpoch. */
// 根据本机记录的集群中的各个节点的configEpoch以及集群currentEpoch计算出最大的纪元
uint64_t clusterGetMaxEpoch(void) {
    uint64_t max = 0;
    dictIterator *di;
    dictEntry *de;

    // 逐个遍历每个节点的纪元, 获取最大纪元
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->configEpoch > max) max = node->configEpoch;
    }
    dictReleaseIterator(di);
    // 结合考虑集群的纪元
    if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
    return max;
}

/* If this node epoch is zero or is not already the greatest across the
 * cluster (from the POV of the local configuration), this function will:
 *
 * 1) Generate a new config epoch, incrementing the current epoch.
 * 2) Assign the new epoch to this node, WITHOUT any consensus.
 * 3) Persist the configuration on disk before sending packets with the
 *    new configuration.
 *
 * If the new config epoch is generated and assigend, C_OK is returned,
 * otherwise C_ERR is returned (since the node has already the greatest
 * configuration around) and no operation is performed.
 *
 * Important note: this function violates the principle that config epochs
 * should be generated with consensus and should be unique across the cluster.
 * However Redis Cluster uses this auto-generated new config epochs in two
 * cases:
 *
 * 1) When slots are closed after importing. Otherwise resharding would be
 *    too expensive.
 * 2) When CLUSTER FAILOVER is called with options that force a slave to
 *    failover its master even if there is not master majority able to
 *    create a new configuration epoch.
 *
 * Redis Cluster will not explode using this function, even in the case of
 * a collision between this node and another node, generating the same
 * configuration epoch unilaterally, because the config epoch conflict
 * resolution algorithm will eventually move colliding nodes to different
 * config epochs. However using this function may violate the "last failover
 * wins" rule, so should only be used with care. */
// 本机节点纪元缺失或者并不是最大纪元时,自增本机记录的集群纪元+本机节点纪元
int clusterBumpConfigEpochWithoutConsensus(void) {
    uint64_t maxEpoch = clusterGetMaxEpoch();

    if (myself->configEpoch == 0 ||
        myself->configEpoch != maxEpoch)
    {
        server.cluster->currentEpoch++;
        myself->configEpoch = server.cluster->currentEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);
        serverLog(LL_WARNING,
            "New configEpoch set to %llu",
            (unsigned long long) myself->configEpoch);
        return C_OK;
    } else {
        return C_ERR;
    }
}

/* This function is called when this node is a master, and we receive from
 * another master a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different slaves get the same config
 * epoch during a failover election, because the slaves need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for masters serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * masters having different configuration epochs whatever happened, since
 * nothign is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 */
/**
 * 解决本机节点(主节点)纪元与入参节点(主节点) 节点纪元 相同的问题
 * 只有nodename小的节点才需要增加自己的集群纪元 并作为 自己的节点纪元
 */ 
void clusterHandleConfigEpochCollision(clusterNode *sender) {
    // 本机节点跟收到的其他节点都是主节点,而且两者的configEpoch相同,才能继续执行本函数
    /* Prerequisites: nodes have the same configEpoch and are both masters. */
    if (sender->configEpoch != myself->configEpoch ||
        !nodeIsMaster(sender) || !nodeIsMaster(myself)) return;
    /* Don't act if the colliding node has a smaller Node ID. */
    // 当前节点id的字母序大于等于对方,则不执行本函数
    if (memcmp(sender->name,myself->name,CLUSTER_NAMELEN) <= 0) return;
    // 只有本机节点id字母序小才可以增1
    /* Get the next ID available at the best of this node knowledge. */
    // 增加本节点记录的集群纪元,同时付给本节点纪元
    server.cluster->currentEpoch++;
    myself->configEpoch = server.cluster->currentEpoch;
    clusterSaveConfigOrDie(1);
    serverLog(LL_VERBOSE,
        "WARNING: configEpoch collision with node %.40s."
        " configEpoch set to %llu",
        sender->name,
        (unsigned long long) myself->configEpoch);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not readded before some time elapsed (this time is specified
 * in seconds in CLUSTER_BLACKLIST_TTL).
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * Currently the CLUSTER_BLACKLIST_TTL is set to 1 minute, this means
 * that redis-trib has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 * -------------------------------------------------------------------------- */

// 节点黑名单失效时长
#define CLUSTER_BLACKLIST_TTL 60      /* 1 minute. */


/* Before of the addNode() or Exists() operations we always remove expired
 * entries from the black list. This is an O(N) operation but it is not a
 * problem since add / exists operations are called very infrequently and
 * the hash table is supposed to contain very little elements at max.
 * However without the cleanup during long uptimes and with some automated
 * node add/removal procedures, entries could accumulate. */
 // 清除节点黑名单中过期的节点数据
 // 遍历黑名单字典里的过期时间,将过期的数据从黑名单里清除
void clusterBlacklistCleanup(void) {
    dictIterator *di;
    dictEntry *de;

    // 获取迭代器
    di = dictGetSafeIterator(server.cluster->nodes_black_list);
    while((de = dictNext(di)) != NULL) {
        // value是过期时刻
        int64_t expire = dictGetUnsignedIntegerVal(de);

        if (expire < server.unixtime)
            // 从黑名单字典里将此过期的key对应的数据删除掉
            // todo ??函数内部同样会摧毁key指向的内存,此时的入参参数是否有风险??
            dictDelete(server.cluster->nodes_black_list,dictGetKey(de));
    }
    dictReleaseIterator(di);
}

/* Cleanup the blacklist and add a new node ID to the black list. */
// 将指定的节点数据加入到黑名单字典里,定时失效,时长1分钟
void clusterBlacklistAddNode(clusterNode *node) {
    dictEntry *de;
    // 创建一个临时的key
    sds id = sdsnewlen(node->name,CLUSTER_NAMELEN);

    // 清理一次过期的黑名单字典里的数据
    clusterBlacklistCleanup();
    // 黑名单字典里的key与value都是直接使用入参传入的数值
    if (dictAdd(server.cluster->nodes_black_list,id,NULL) == DICT_OK) {
        /* If the key was added, duplicate the sds string representation of
         * the key for the next lookup. We'll free it at the end. */
        // 黑名单字典里的key是直接使用传入的key内存,其内部不会开辟新内存
        // 在字典加入成功后,此处需要再度创建一份
        id = sdsdup(id);
    }
    // 因为需要更新此节点的过期时刻,所以获取一下字典里的节点
    de = dictFind(server.cluster->nodes_black_list,id);
    dictSetUnsignedIntegerVal(de,time(NULL)+CLUSTER_BLACKLIST_TTL);
    sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. */
// 查看指定的nodeid释放在黑名单字典里
int clusterBlacklistExists(char *nodeid) {
    // 构造一个临时的key
    sds id = sdsnewlen(nodeid,CLUSTER_NAMELEN);
    int retval;
    // 清理一次过期的黑名单字典里的数据
    clusterBlacklistCleanup();
    retval = dictFind(server.cluster->nodes_black_list,id) != NULL;
    sdsfree(id);
    return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

/* This function checks if a given node should be marked as FAIL.
 * It happens if the following conditions are met:
 *
 * 1) We received enough failure reports from other master nodes via gossip.
 *    Enough means that the majority of the masters signaled the node is
 *    down recently.
 * 2) We believe this node is in PFAIL state.
 *
 * If a failure is detected we also inform the whole cluster about this
 * event trying to force every other node to set the FAIL flag for the node.
 *
 * Note that the form of agreement used here is weak, as we collect the majority
 * of masters state during some time, and even if we force agreement by
 * propagating the FAIL message, because of partitions we may not reach every
 * node. However:
 *
 * 1) Either we reach the majority and eventually the FAIL state will propagate
 *    to all the cluster.
 * 2) Or there is no majority so no slave promotion will be authorized and the
 *    FAIL flag will be cleared after some time.
 */
// 本机节点基于自身数据,尝试将该node置为fail状态
// 另外,只有本机为主节点时才能在集群里广播"本机将某节点置为fail"消息
// 注意:只有本机认为该node已经pfail才能依据失败报告进行判别是否升级为fail;如果本机认为该node正常则直接返回
void markNodeAsFailingIfNeeded(clusterNode *node) {
    int failures;
    // 根据集群实际复制slot的主节点个数,计算出要判定一个节点为fail所需要的最低主节点投票数
    int needed_quorum = (server.cluster->size / 2) + 1;

    // 校验该节点是否处于疑似fail状态,如果未处于此状态,则表示此节点正常
    // 这里表示:只有本机认为入参节点处于疑似fail时,才会可能将其转为fail. 否则本机认为入参节点一切正常.
    if (!nodeTimedOut(node)) return; /* We can reach it. */
    // 本机校验该节点已经处于fail状态,则无需后续逻辑直接返回
    if (nodeFailed(node)) return; /* Already FAILing. */

    // 获取关于目标节点的失败报告个数
    failures = clusterNodeFailureReportsCount(node);
    /* Also count myself as a voter if I'm a master. */
    // 本机为主节点时,对失败报告数+1,因为失败报告列表中不会记录本机,走到此处表明,本机也认为该node处于pfail
    if (nodeIsMaster(myself)) failures++;
    if (failures < needed_quorum) return; /* No weak agreement from masters. */

    serverLog(LL_NOTICE,
        "Marking node %.40s as failing (quorum reached).", node->name);

    /* Mark the node as failing. */
    // 该节点确认失败,移除疑似失败标记,打上FAIL标记
    node->flags &= ~CLUSTER_NODE_PFAIL;
    node->flags |= CLUSTER_NODE_FAIL;
    node->fail_time = mstime();

    /* Broadcast the failing node name to everybody, forcing all the other
     * reachable nodes to flag the node as FAIL. */
    // 本机为主节点时,需要在集群里广播本机已将指定节点为FAIL状态,这样收到的节点也会将其置为fail
    if (nodeIsMaster(myself)) clusterSendFail(node->name);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. */
// 本机将指定节点的fail状态尝试清除
// 前提条件:指定节点之前处于FAIL,而目前收到了PONG消息
// 如果指定节点是备节点 or 不负责任何槽位时,可以去除掉FAIL标记
// 如果指定节点是主节点 且 负责某些槽位 且 从上一次FAIL到目前的时长超过2nodetime,也可以去掉FAIL标记
void clearNodeFailureIfNeeded(clusterNode *node) {
    mstime_t now = mstime();

    serverAssert(nodeFailed(node));

    /* For slaves we always clear the FAIL flag if we can contact the
     * node again. */
    if (nodeIsSlave(node) || node->numslots == 0) {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: %s is reachable again.",
                node->name,
                nodeIsSlave(node) ? "replica" : "master without slots");
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }

    /* If it is a master and...
     * 1) The FAIL state is old enough.
     * 2) It is yet serving slots from our point of view (not failed over).
     * Apparently no one is going to fix these slots, clear the FAIL flag. */
    if (nodeIsMaster(node) && node->numslots > 0 &&
        (now - node->fail_time) >
        (server.cluster_node_timeout * CLUSTER_FAIL_UNDO_TIME_MULT))
    {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: is reachable again and nobody is serving its slots after some time.",
                node->name);
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }
}

/* Return true if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. */
// 校验在本机记录的集群节点中, 指定的ip 端口之前是否已经处于握手状态
// 注意如果集群中已经有处于握手中而且地址相同,那么返回true
// 注意:此函数未检查该信息是否以 非握手态 存在于server.cluster->nodes中,后续在握手完毕时会有查重机制避免同一个节点有多分node
int clusterHandshakeInProgress(char *ip, int port, int cport) {
    dictIterator *di;
    dictEntry *de;

    // 逐一遍历所有的节点数据
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!nodeInHandshake(node)) continue;
        // 处于握手中的节点需要判定ip 端口等信息
        if (!strcasecmp(node->ip,ip) &&
            node->port == port &&
            node->cport == cport) break;
    }
    dictReleaseIterator(di);
    return de != NULL;
}

/* Start an handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * EAGAIN - There is already an handshake in progress for this address.
 * EINVAL - IP or port are not valid. */
// 本机主动跟指定的地址进行握手操作
// 使用场景:
// 1. 运维通过meet命令向集群中添加新的node;
// 2. 本机收到集群其他节点的ping/pong/meet消息体里有本机不认识的新节点
int clusterStartHandshake(char *ip, int port, int cport) {
    clusterNode *n;
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP sanity check */
    // 校验ip入参格式是否合法同时分辨出协议是ipv4还是ipv6
    if (inet_pton(AF_INET,ip,
            &(((struct sockaddr_in *)&sa)->sin_addr)))
    {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6,ip,
            &(((struct sockaddr_in6 *)&sa)->sin6_addr)))
    {
        sa.ss_family = AF_INET6;
    } else {
        errno = EINVAL;
        return 0;
    }

    /* Port sanity check */
    // 校验设置的端口以及总线端口是否在范围内
    if (port <= 0 || port > 65535 || cport <= 0 || cport > 65535) {
        errno = EINVAL;
        return 0;
    }

    /* Set norm_ip as the normalized string representation of the node
     * IP address. */
    // IP数字转换为字符串
    memset(norm_ip,0,NET_IP_STR_LEN);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET,
            (void*)&(((struct sockaddr_in *)&sa)->sin_addr),
            norm_ip,NET_IP_STR_LEN);
    else
        inet_ntop(AF_INET6,
            (void*)&(((struct sockaddr_in6 *)&sa)->sin6_addr),
            norm_ip,NET_IP_STR_LEN);

    // 校验在本机记录的集群节点中, 指定的ip 端口之前是否已经处于握手状态
    if (clusterHandshakeInProgress(norm_ip,port,cport)) {
        errno = EAGAIN;
        return 0;
    }

    /* Add the node with a random address (NULL as first argument to
     * createClusterNode()). Everything will be fixed during the
     * handshake. */
    // 创建一个新的node节点并添加到集群中,节点id此时暂时使用一个随机值,后续在握手成功完毕后会更正为正确的数值
    n = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_MEET);
    memcpy(n->ip,norm_ip,sizeof(n->ip));
    n->port = port;
    n->cport = cport;
    clusterAddNode(n);
    return 1;
}

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. */
// 处理ping/pong/meet消息里的载体数据,并更新本机记录的数据
// 1. 依据主节点消息中关于某节点的状态 更新 本机记录的该节点的fail_report:添加or更新or移除;
// 2. 在极其严格的条件下,依据信息中某节点的pong_received字段更新本机中该节点的pong_received字段;
// 3. 尝试以消息中的数据更新本机中该节点的地址数据;
// 4. 依据发送方提供的新节点,本机主动与该新节点握手连接.
// 本函数可能是 主动外呼的link收到了回复消息;  可能是 被动呼起的link直接收到ping/pong/meet消息
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
    // 消息头之后紧挨着body个数
    uint16_t count = ntohs(hdr->count);
    clusterMsgDataGossip *g = (clusterMsgDataGossip*) hdr->data.ping.gossip;
    clusterNode *sender = link->node ? link->node/*主动外呼场景*/ : clusterLookupNode(hdr->sender)/*被动呼起场景*/;

    while(count--) {
        uint16_t flags = ntohs(g->flags);
        clusterNode *node;
        sds ci;

        if (server.verbosity == LL_DEBUG) {
            ci = representClusterNodeFlags(sdsempty(), flags);
            serverLog(LL_DEBUG,"GOSSIP %.40s %s:%d@%d %s",
                g->nodename,
                g->ip,
                ntohs(g->port),
                ntohs(g->cport),
                ci);
            sdsfree(ci);
        }

        /* Update our state accordingly to the gossip sections */
        // 遍历到的node数据是否已存在本机记录的集群数据中
        node = clusterLookupNode(g->nodename);
        if (node) {
            /* We already know this node.
               Handle failure reports, only when the sender is a master. */
            /**
             * 发送方是一个已知主节点,采信对方,依据消息中记录的除本机之外各个节点状态来更新本机记录的数据
             * 
             * 1. 当遍历到的节点是一个fail or pfail时,将其添加到该节点的fail-report记录里,并确认该节点是否需要升级为FAIL;
             * 2. 当遍历到的节点是一个正常节点,就从该节点的fail-report记录中移除该sender对应的数据,表示该sender认为该节点已经与其恢复联系;
             */ 
            if (sender && nodeIsMaster(sender) && node != myself) {
                if (flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) {
                    // 所描述的节点是FAIL or PFAIL状态
                    if (clusterNodeAddFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s as not reachable.",
                            sender->name, node->name);
                    }
                    // 确认该节点是否为FAIL状态
                    markNodeAsFailingIfNeeded(node);
                } else {
                    // 此处表示所描述的节点为正常节点, 将此节点的失败报告中移除指定发送方
                    if (clusterNodeDelFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s is back online.",
                            sender->name, node->name);
                    }
                }
            }

            /* If from our POV the node is up (no failure flags are set),
             * we have no pending ping for the node, nor we have failure
             * reports for this node, update the last pong time with the
             * one we see from the other nodes. */
            /**
             * 尝试更新本机里的某节点pong_received
             * 
             * 对集群中同一个节点,有本机视角与sender视角两方进行描述.
             * 在极其严格的条件下,可以用sender视角的数据直接更新本机视角的数据,
             * 从某种意义上来说,是本机 信任 sender对某个节点的描述,进而采信sender的数据
             * 
             * 此举可以减少集群内部通信数据量
             * 
             * 条件必须全部满足:
             * 1. sender认为该节点正常(非FAIL or 非PFAIL);
             * 2. 本机对该节点当前没有处于待回复的ping消息;
             * 3. 本机记录的该节点失败报告为0;
             * 4. sender记录该节点的最新pong_received时间戳 比 本机记录的pong_received数值 更新
             *    但是还没有超过本机服务器时间500ms
             */ 
            if (!(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                node->ping_sent == 0 &&
                clusterNodeFailureReportsCount(node) == 0)
            {
                mstime_t pongtime = ntohl(g->pong_received);
                pongtime *= 1000; /* Convert back to milliseconds. */

                /* Replace the pong time with the received one only if
                 * it's greater than our view but is not in the future
                 * (with 500 milliseconds tolerance) from the POV of our
                 * clock. */
                if (pongtime <= (server.mstime+500) &&
                    pongtime > node->pong_received)
                {
                    node->pong_received = pongtime;
                }
            }

            /* If we already know this node, but it is not reachable, and
             * we see a different address in the gossip section of a node that
             * can talk with this other node, update the address, disconnect
             * the old link if any, so that we'll attempt to connect with the
             * new address. */
            /**
             * 尝试更新本机的某节点地址数据
             * 
             * 本机对于已知节点的状态是fail或者pfail,但是发送方对该已知节点正常
             * 而且两方对该节点的地址数据不同,那么说明本机对该节点的地址有误,需更新.
             * 
             * 满足条件:
             * 1. 本机已知当前遍历到的节点node,但是本机认为该节点处于fail or pfail;
             * 2. 发送方知道该节点的地址
             * 3. 发送方记录该节点的状态既不是fail也不是pfail
             * 4. 本机与发送方双方记录的该节点地址有不同
             */ 
            if (node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL) && // 本机判定node失联
                !(flags & CLUSTER_NODE_NOADDR) &&//pong中该node有地址
                !(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) && //pong中该node状态正常
                (strcasecmp(node->ip,g->ip) || // 双方记录的地址不同
                 node->port != ntohs(g->port) ||
                 node->cport != ntohs(g->cport)))
            {
                // 地址发生了变化,所以需要将旧的link移除待后续重连,更新node里的ip 端口地址
                if (node->link) freeClusterLink(node->link);
                memcpy(node->ip,g->ip,NET_IP_STR_LEN);
                node->port = ntohs(g->port);
                node->cport = ntohs(g->cport);
                node->flags &= ~CLUSTER_NODE_NOADDR;
            }
        } else {
            /* If it's not in NOADDR state and we don't have it, we
             * start a handshake process against this IP/PORT pairs.
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster. */
            // 消息中的节点是未知的新节点,且发送方为已知节点,那么就跟此新节点进行握手链接
            /**
             * 本机收到一个已知发送方记录的一个新节点,而且发送方知道新节点的地址数据
             * 如果该新节点不在本机的黑名单中,那么本机就主动发起对此新节点的握手连接.
             */ 
            if (sender &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !clusterBlacklistExists(g->nodename))
            {
                clusterStartHandshake(g->ip,ntohs(g->port),ntohs(g->cport));
            }
        }

        /* Next node */
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes.
 * If 'announced_ip' length is non-zero, it is used instead of extracting
 * the IP from the socket peer address. */
// 基于入参announced_ip 以及 link中的套接字得到的远端地址, 获得字符串格式的ip地址
// announced_ip优先级高于link中的套接字
void nodeIp2String(char *buf, clusterLink *link, char *announced_ip) {
    if (announced_ip[0] != '\0') {
        memcpy(buf,announced_ip,NET_IP_STR_LEN);
        buf[NET_IP_STR_LEN-1] = '\0'; /* We are not sure the input is sane. */
    } else {
        // 根据link中的fd获取远端的ip地址
        anetPeerToString(link->fd, buf, NET_IP_STR_LEN, NULL);
    }
}

/* Update the node address to the IP address that can be extracted
 * from link->fd, or if hdr->myip is non empty, to the address the node
 * is announcing us. The port is taken from the packet header as well.
 *
 * If the address or port changed, disconnect the node link so that we'll
 * connect again to the new address.
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. */
// 尝试更新指定节点node的地址数据
// 综合考虑 入参link中套接字句柄获得的远端IP地址 以及 消息头携带的发送方地址+端口
// 如果发现需要更新指定节点node的地址时,则
//   1. 释放其对应的link数据,待后续重连;
//   2. 该node节点当前是本机的主节点时,调整本机的复制源ip+端口地址
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link,
                              clusterMsg *hdr)
{
    char ip[NET_IP_STR_LEN] = {0};
    int port = ntohs(hdr->port);
    int cport = ntohs(hdr->cport);

    /* We don't proceed if the link is the same as the sender link, as this
     * function is designed to see if the node link is consistent with the
     * symmetric link that is used to receive PINGs from the node.
     *
     * As a side effect this function never frees the passed 'link', so
     * it is safe to call during packet processing. */
    if (link == node->link) return 0;
    // 优先以消息中记录的地址保存到ip数组中,否则以link的socket套接字获取远端的地址保存到ip数组中
    nodeIp2String(ip,link,hdr->myip);
    // 地址相同没有变化
    if (node->port == port && node->cport == cport &&
        strcmp(ip,node->ip) == 0) return 0;

    /* IP / port is different, update it. */
    // ip端口不同,以最新的替换旧值
    memcpy(node->ip,ip,sizeof(ip));
    node->port = port;
    node->cport = cport;
    if (node->link) freeClusterLink(node->link);
    node->flags &= ~CLUSTER_NODE_NOADDR;
    serverLog(LL_WARNING,"Address updated for node %.40s, now %s:%d",
        node->name, node->ip, node->port);

    /* Check if this is our master and we have to change the
     * replication target as well. */
    if (nodeIsSlave(myself) && myself->slaveof == node)
        // 本节点是备节点,而且此node为本节点的主节点, 那么将本机的复制地址更新为最新的ip端口
        replicationSetMaster(node->ip, node->port);
    return 1;
}

/* Reconfigure the specified node 'n' as a master. This function is called when
 * a node that we believed to be a slave is now acting as master in order to
 * update the state of the node. */
// 将指定节点改为主节点
// 只修改本机记录的数据
void clusterSetNodeAsMaster(clusterNode *n) {
    // 已经是主节点情况下,直接返回
    if (nodeIsMaster(n)) return;

    // 为某个主节点的备节点,则将其摘除
    if (n->slaveof) {
        clusterNodeRemoveSlave(n->slaveof,n);
        if (n != myself) n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    n->flags &= ~CLUSTER_NODE_SLAVE;//移除备节点标记
    n->flags |= CLUSTER_NODE_MASTER;//打上主节点标记
    n->slaveof = NULL;//主节点模式下是没有上级节点信息

    /* Update config and state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a master configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actually the "Sender" of the information, like in the
 * case we receive the info via an UPDATE packet. */
// 使用指定node的slots信息来更新本机集群slots信息
/**
 * 1. 本机记录的消息中涉及到所有slot均改由sender负责;
 * 2. 如果本机所在的子集群原主节点 由第一步原因 发生过slot迁移,并且已经没有任何slot了,那么本机将sender置为主;
 * 3. 对于已经迁移走的slot,本机需要从db库中删除该slot里的keys
 */ 
void clusterUpdateSlotsConfigWith(clusterNode *sender, uint64_t senderConfigEpoch, unsigned char *slots) {
    int j;
    clusterNode *curmaster, *newmaster = NULL;
    /* The dirty slots list is a list of slots for which we lose the ownership
     * while having still keys inside. This usually happens after a failover
     * or after a manual cluster reconfiguration operated by the admin.
     *
     * If the update message is not able to demote a master to slave (in this
     * case we'll resync with the master updating the whole key space), we
     * need to delete all the keys in the slots we lost ownership. */
    uint16_t dirty_slots[CLUSTER_SLOTS];
    int dirty_slots_count = 0;

    /* Here we set curmaster to this node or the node this node
     * replicates to if it's a slave. In the for loop we are
     * interested to check if slots are taken away from curmaster. */
    // 获取本机所在子集群的主节点
    curmaster = nodeIsMaster(myself) ? myself : myself->slaveof;

    // 放弃描述本机节点的数据
    if (sender == myself) {
        serverLog(LL_WARNING,"Discarding UPDATE message about myself.");
        return;
    }

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 遍历消息中的槽位
        if (bitmapTestBit(slots,j)) {
            /* The slot is already bound to the sender of this message. */
            // 本机记录的槽位已经由sender负责
            if (server.cluster->slots[j] == sender) continue;

            /* The slot is in importing state, it should be modified only
             * manually via redis-trib (example: a resharding is in progress
             * and the migrating side slot was already closed and is advertising
             * a new config. We still want the slot to be closed manually). */
            // 该槽位当前正在迁入本机,那么槽位会由迁入动作完毕时确认
            // 不采信消息中的标记
            if (server.cluster->importing_slots_from[j]) continue;

            /* We rebind the slot to the new node claiming it if:
             * 1) The slot was unassigned or the new node claims it with a
             *    greater configEpoch.
             * 2) We are not currently importing the slot. */
            /**
             * 业务走到这里,表明消息中有槽位是由sender负责.
             * 
             * 负责该槽位的节点有变化,需要依据信息对本机进行调整,满足如下任何一个条件就需要进行调整
             * 1. 本机记录的槽位无节点负责;
             * 2. 消息中的集群纪元大于该槽位对应的节点节点纪元;
             */ 
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->configEpoch < senderConfigEpoch)
            {
                /* Was this slot mine, and still contains keys? Mark it as
                 * a dirty slot. */
                /**
                 * 该槽位之前是有本机负责,但是最新已经变为sender
                 * 
                 * 本机记录的该槽位由本机负责,
                 * 本机记录的该槽位目前有key在使用,
                 * 而消息中该槽位的处理节点不是本机
                 */ 
                if (server.cluster->slots[j] == myself &&
                    countKeysInSlot(j) &&
                    sender != myself)
                {
                    dirty_slots[dirty_slots_count] = j;
                    dirty_slots_count++;
                }

                // 本机记录的该槽位原先是由curmaster负责
                // 但是最新的消息显示该槽位换由sender节点负责
                if (server.cluster->slots[j] == curmaster)
                    newmaster = sender;

                // 删除本机该slot槽位原映射关系
                clusterDelSlot(j);
                // 添加该slot槽位跟sender的映射关系
                clusterAddSlot(sender,j);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE|
                                     CLUSTER_TODO_FSYNC_CONFIG);
            }
        }
    }

    /* After updating the slots configuration, don't do any actual change
     * in the state of the server if a module disabled Redis Cluster
     * keys redirections. */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return;

    /* If at least one slot was reassigned from a node to another node
     * with a greater configEpoch, it is possible that:
     * 1) We are a master left without slots. This means that we were
     *    failed over and we should turn into a replica of the new
     *    master.
     * 2) We are a slave and our master is left without slots. We need
     *    to replicate to the new slots owner. */
    /**
     * 消息显示:本机所在的子集群原主节点已经不负责任何槽位,之前所负责的至少一个槽位已经发生了迁移
     * 那么本机就需要挂靠到新的主节点:sender
     */ 
    if (newmaster && curmaster->numslots == 0) {
        serverLog(LL_WARNING,
            "Configuration change detected. Reconfiguring myself "
            "as a replica of %.40s", sender->name);
        // 将sender置为本机的主节点
        clusterSetMaster(sender);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
    } else if (dirty_slots_count) {
        /* If we are here, we received an update message which removed
         * ownership for certain slots we still have keys about, but still
         * we are serving some slots, so this master node was not demoted to
         * a slave.
         *
         * In order to maintain a consistent state between keys and slots
         * we need to remove all the keys from the slots we lost. */
        /**
         * 走到这里表示:之前本机负责的槽位已经有一些改为sender
         * 那么原先本机负责槽位里的key都需要删除掉
         */ 
        for (j = 0; j < dirty_slots_count; j++)
            delKeysInSlot(dirty_slots[j]);
    }
}

/* When this function is called, there is a packet to process starting
 * at node->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). */
// 处理集群内部总线通信消息入口函数
// 注意: 只有收到完整的通信消息,才能调用本函数;
//       本函数内部在某些异常的场景下,会释放link内存,并返回0
int clusterProcessPacket(clusterLink *link) {
    clusterMsg *hdr = (clusterMsg*) link->rcvbuf;
    uint32_t totlen = ntohl(hdr->totlen);
    uint16_t type = ntohs(hdr->type);

    // 校验本次消息是否合法
    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->stats_bus_messages_received[type]++;//统计收到的消息次数
    serverLog(LL_DEBUG,"--- Processing packet of type %d, %lu bytes",
        type, (unsigned long) totlen);

    /* Perform sanity checks */
    if (totlen < 16) return 1; /* At least signature, version, totlen, count. */
    // 消息体内记录的长度跟接收缓冲区数据长度不足
    if (totlen > sdslen(link->rcvbuf)) return 1;

    // 协议版本号不同
    if (ntohs(hdr->ver) != CLUSTER_PROTO_VER) {
        /* Can't handle messages of different versions. */
        return 1;
    }

    uint16_t flags = ntohs(hdr->flags);
    uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;
    clusterNode *sender;

    // 根据各类协议校验长度是否合法
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        uint16_t count = ntohs(hdr->count);
        uint32_t explen; /* expected length of this packet */
        // 获取消息头后面跟着的body数据个数,计算整体此类型数据的精确长度
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip)*count);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        // FAIL型消息长度
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataFail);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        // publish消息长度
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataPublish) -
                8 +
                ntohl(hdr->data.publish.msg.channel_len) +
                ntohl(hdr->data.publish.msg.message_len);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
               type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
               type == CLUSTERMSG_TYPE_MFSTART)
    {
        // 无body型的消息长度
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataUpdate);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataPublish) -
                3 + ntohl(hdr->data.module.msg.len);
        if (totlen != explen) return 1;
    }

    /* Check if the sender is a known node. */
    // 确认本机是否认识发送方
    sender = clusterLookupNode(hdr->sender);
    if (sender && !nodeInHandshake(sender)) {
        // 发送方是已知节点,并且此节点并不处于握手中
        /* Update our curretEpoch if we see a newer epoch in the cluster. */
        // 更新本进程保存的sender节点的纪元数据
        senderCurrentEpoch = ntohu64(hdr->currentEpoch);
        senderConfigEpoch = ntohu64(hdr->configEpoch);
        // 集群纪元需要保证集群一致且为各个节点里记录的最大值
        if (senderCurrentEpoch > server.cluster->currentEpoch)
            server.cluster->currentEpoch = senderCurrentEpoch;
        /* Update the sender configEpoch if it is publishing a newer one. */
        if (senderConfigEpoch > sender->configEpoch) {
            // 更新本节点记录的对方的配置纪元
            sender->configEpoch = senderConfigEpoch;
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_FSYNC_CONFIG);
        }
        /* Update the replication offset info for this node. */
        // 更新本机记录的sender复制偏移量,在故障切换时计算rank值使用
        sender->repl_offset = ntohu64(hdr->offset);
        sender->repl_offset_time = mstime();
        /* If we are a slave performing a manual failover and our master
         * sent its offset while already paused, populate the MF state. */
        if (server.cluster->mf_end &&
            nodeIsSlave(myself) &&
            myself->slaveof == sender &&
            hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
            server.cluster->mf_master_offset == 0)
        {
            server.cluster->mf_master_offset = sender->repl_offset;
            serverLog(LL_WARNING,
                "Received replication offset for paused "
                "master manual failover: %lld",
                server.cluster->mf_master_offset);
        }
    }

    /* Initial processing of PING and MEET requests replying with a PONG. */
    // 对于收到ping or meet需要回复一个pong
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        serverLog(LL_DEBUG,"Ping packet received: %p", (void*)link->node);

        /* We use incoming MEET messages in order to set the address
         * for 'myself', since only other cluster nodes will send us
         * MEET messages on handshakes, when the cluster joins, or
         * later if we changed address, and those nodes will use our
         * official address to connect to us. So by obtaining this address
         * from the socket is a simple way to discover / update our own
         * address in the cluster without it being hardcoded in the config.
         *
         * However if we don't have an address at all, we update the address
         * even with a normal PING packet. If it's wrong it will be fixed
         * by MEET later. */
        // 当没有明确配置本机的ip地址时, 会在每次收到meet类型消息,都会根据实际的tcp连接设置本机的ip地址
        if ((type == CLUSTERMSG_TYPE_MEET || myself->ip[0] == '\0') &&
            server.cluster_announce_ip == NULL)
        {
            // 消息类型是MEET或者本机节点暂为空,则需要根据tcp链接填充本node节点的ip地址
            char ip[NET_IP_STR_LEN];

            if (anetSockName(link->fd,ip,sizeof(ip),NULL) != -1 &&
                strcmp(ip,myself->ip))
            {
                // 这里只记录最新的本机ip地址,并不会断开tcp连接
                memcpy(myself->ip,ip,NET_IP_STR_LEN);
                serverLog(LL_WARNING,"IP address for this node updated to %s",
                    myself->ip);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            }
        }

        /* Add this node if it is new for us and the msg type is MEET.
         * In this stage we don't try to add the node with the right
         * flags, slaveof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the node. */
        /**
         * 发送方是个陌生的节点,只有在meet消息时才会无条件信任加载到本机的集群中
         */ 
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            // 发送方是一个全新的节点且消息类型是MEET
            clusterNode *node;

            // 构造一个处于握手中的新node节点加入到集群字典中, 注意此时的nodename是由自己生成的临时名称
            node = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE);
            nodeIp2String(node->ip,link,hdr->myip);
            node->port = ntohs(hdr->port);
            node->cport = ntohs(hdr->cport);
            clusterAddNode(node);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }

        /* If this is a MEET packet from an unknown node, we still process
         * the gossip section here since we have to trust the sender because
         * of the message type. */
        /**
         * 发送方是个陌生的节点,只有在meet消息时才会信任其发送的载体数据
         */ 
        if (!sender && type == CLUSTERMSG_TYPE_MEET)
            clusterProcessGossipSection(hdr,link);

        /* Anyway reply with a PONG */
        // 无论是否为陌生节点,在收到ping/mee消息时, 均响应一个pong消息
        clusterSendPing(link,CLUSTERMSG_TYPE_PONG);
    }

    /* PING, PONG, MEET: process config information. */
    // 处理节点状态,地址,槽位等信息
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        serverLog(LL_DEBUG,"%s packet received: %p",
            type == CLUSTERMSG_TYPE_PING ? "ping" : "pong",
            (void*)link->node);
        if (link->node) {
            // 本机为主动发起方,此时收到了link->node节点的响应
            if (nodeInHandshake(link->node)) {
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. */
                /**
                 * 防御使用, 避免集群中存在多个node指向同一个实际nodename节点
                 * (例如:
                 * meet时只会判定处于握手中的是否有跟新节点地址重复的节点;
                 * 本机收到多个节点的pong消息里都携带了本机未知的新节点,
                 * 本机只能以地址作为初始信息发起对新节点的握手连接;
                 * )
                 * 
                 * 业务执行至此,表示本机记录了一个处于握手中的node节点,
                 * 按常理来说此节点的真实nodename我们还不知道,因为真实的nodename只有在握手完毕后才知晓.
                 * 而此时sender非空,表明本机的集群记录中已经存在发送方的nodename
                 * 这说明同一个nodename被重复使用命令添加多次,本机集群中存在单一nodename却多个node节点的情况.
                 * 
                 * 这时唯一有价值的是使用最新的地址数据更新集群中已有的node节点,并断开已有的link迫使其使用新地址重连.
                 * 
                 * 最后需要删掉处于握手中node数据.
                 */ 
                if (sender) {
                    serverLog(LL_VERBOSE,
                        "Handshake: we already know node %.40s, "
                        "updating the address if needed.", sender->name);
                    if (nodeUpdateAddressIfNeeded(sender,link,hdr))
                    {
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                             CLUSTER_TODO_UPDATE_STATE);
                    }
                    /* Free this node as we already have it. This will
                     * cause the link to be freed as well. */
                    clusterDelNode(link->node);
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was a handshake stage. */
                // 握手中的节点nodename之前是暂时生成的,这里使用收到的真实数据填充
                clusterRenameNode(link->node, hdr->sender);
                serverLog(LL_DEBUG,"Handshake with node %.40s completed.",
                    link->node->name);
                // 收到对方的回复,则表示该节点正常,完成握手
                link->node->flags &= ~CLUSTER_NODE_HANDSHAKE;
                // 根据消息头里的sender标记设置主or备
                link->node->flags |= flags&(CLUSTER_NODE_MASTER|CLUSTER_NODE_SLAVE);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            } else if (memcmp(link->node->name,hdr->sender,
                        CLUSTER_NAMELEN) != 0)
            {
                // 如果收到的消息表明:已完成握手后,对方节点id改变,但是link连接未断.
                // 那么我们需要将本机记录的该节点link数据删除,并将ip地址抹除, 
                // 只保留node节点,等待其他的消息中更新地址再重连.
                // TODO 无地址信息的node最终归宿是什么?一直孤悬?还是被删除?
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. */
                serverLog(LL_DEBUG,"PONG contains mismatching sender ID. About node %.40s added %d ms ago, having flags %d",
                    link->node->name,
                    (int)(mstime()-(link->node->ctime)),
                    link->node->flags);
                // 本链接没有地址
                link->node->flags |= CLUSTER_NODE_NOADDR;
                link->node->ip[0] = '\0';
                link->node->port = 0;
                link->node->cport = 0;
                // 删除此节点的link数据,但是会保留node节点
                freeClusterLink(link);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                return 0;
            }
        }

        /* Copy the CLUSTER_NODE_NOFAILOVER flag from what the sender
         * announced. This is a dynamic flag that we receive from the
         * sender, and the latest status must be trusted. We need it to
         * be propagated because the slave ranking used to understand the
         * delay of each slave in the voting process, needs to know
         * what are the instances really competing. */
        if (sender) {
            int nofailover = flags & CLUSTER_NODE_NOFAILOVER;
            sender->flags &= ~CLUSTER_NODE_NOFAILOVER;
            sender->flags |= nofailover;
        }

        /* Update the node address if it changed. */
        // 本机收到已知节点的ping消息,非握手状态下,尝试更新本机记录的节点地址数据以防有变化.
        if (sender && type == CLUSTERMSG_TYPE_PING &&
            !nodeInHandshake(sender) &&
            nodeUpdateAddressIfNeeded(sender,link,hdr))
        {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_UPDATE_STATE);
        }

        /* Update our info about the node */
        /**
         * 本机发送的ping消息得到了pong回复,处理该节点的pfail 以及 fail状态.
         * 1. 更新本机对应节点的pong_received时间戳以及将ping_sent归0表示目前没有待回复的ping消息;
         * 2. 已经收到pong消息,那么就尝试修改该节点的状态
         *    a, 该节点之前是pfail,直接去掉pfail标记;
         *    b, 该节点之前是fail, 
         *       如果是备节点 or 主节点但不负责任何槽位时,则直接除掉FAIL标记;
         *       如果是主节点 且 负责某些槽位 且 从上一次FAIL到目前的时长超过2nodetime,直接去掉FAIL标记
         */ 
        if (link->node && type == CLUSTERMSG_TYPE_PONG) {
            // 本机作为主动发起方,收到的之前PING的应答PONG消息,更新本地的标记
            link->node->pong_received = mstime();
            link->node->ping_sent = 0;

            /* The PFAIL condition can be reversed without external
             * help if it is momentary (that is, if it does not
             * turn into a FAIL state).
             *
             * The FAIL condition is also reversible under specific
             * conditions detected by clearNodeFailureIfNeeded(). */
            if (nodeTimedOut(link->node)) {
                // 立即取消PFAIL标记
                link->node->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            } else if (nodeFailed(link->node)) {
                // 适时的取消FAIL标记
                clearNodeFailureIfNeeded(link->node);
            }
        }

        /* Check for role switch: slave -> master or master -> slave. */
        /**
         * 根据收到的消息,调整本机记录集群中sender对应节点的主备关系
         * 只会修改sender这个节点的主备信息
         */ 
        if (sender) {
            if (!memcmp(hdr->slaveof,CLUSTER_NODE_NULL_NAME,
                sizeof(hdr->slaveof)))
            {
                /* Node is a master. */
                // 发送方宣称自己是一个主节点
                clusterSetNodeAsMaster(sender);
            } else {
                // 发送方宣称自己是一个备节点
                /* Node is a slave. */
                clusterNode *master = clusterLookupNode(hdr->slaveof);

                // 本机记录sender是个主节点,则需要将sender由主变为备
                if (nodeIsMaster(sender)) {
                    /* Master turned into a slave! Reconfigure the node. */
                    clusterDelNodeSlots(sender);
                    sender->flags &= ~(CLUSTER_NODE_MASTER|
                                       CLUSTER_NODE_MIGRATE_TO);
                    sender->flags |= CLUSTER_NODE_SLAVE;

                    /* Update config and state. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                         CLUSTER_TODO_UPDATE_STATE);
                }

                /* Master node changed for this slave? */
                /**
                 * 消息宣称:发送方是备节点,且所隶属的主节点已经改变
                 * 本机记录的数据显示:发送方为主or发送方为备
                 * 此处的master: 消息中宣称为sender的新主
                 * sender->slaveof: sender的旧主 or 无主
                 */ 
                if (master && sender->slaveof != master) {
                    if (sender->slaveof)//sender有旧主
                        clusterNodeRemoveSlave(sender->slaveof,sender);

                    // 将发送方指向新主    
                    clusterNodeAddSlave(master,sender);
                    sender->slaveof = master;

                    /* Update config. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                }
            }
        }

        /* Update our info about served slots.
         *
         * Note: this MUST happen after we update the master/slave state
         * so that CLUSTER_NODE_MASTER flag will be set. */

        /* Many checks are only needed if the set of served slots this
         * instance claims is different compared to the set of slots we have
         * for it. Check this ASAP to avoid other computational expansive
         * checks later. */
        clusterNode *sender_master = NULL; /* Sender or its master if slave. */
        int dirty_slots = 0; /* Sender claimed slots don't match my view? */

        // 确认消息中的slot数据与本地记录的slot信息是否有变
        if (sender) {
            sender_master = nodeIsMaster(sender) ? sender : sender->slaveof;
            // 检查本地记录的slot槽位与消息体中的槽位信息是否有不同
            if (sender_master) {
                dirty_slots = memcmp(sender_master->slots,
                        hdr->myslots,sizeof(hdr->myslots)) != 0;
            }
        }

        /* 1) If the sender of the message is a master, and we detected that
         *    the set of slots it claims changed, scan the slots to see if we
         *    need to update our configuration. */
        if (sender && nodeIsMaster(sender) && dirty_slots)
            /**
             * 发送消息的节点是主节点且与本地记录的槽位数据不一致,则以消息中为准,更新本机
             * 原先由本机负责的slot改由sender负责;
             * 如果本机所在的子集群原主节点发生过slot迁移,并且已经没有任何slot了,那么本机将sender置为主;
             * 对于已经迁移走的slot,本机需要从db库中删除该slot里的keys
             */ 
            clusterUpdateSlotsConfigWith(sender,senderConfigEpoch,hdr->myslots);

        /* 2) We also check for the reverse condition, that is, the sender
         *    claims to serve slots we know are served by a master with a
         *    greater configEpoch. If this happens we inform the sender.
         *
         * This is useful because sometimes after a partition heals, a
         * reappearing master may be the last one to claim a given set of
         * hash slots, but with a configuration that other instances know to
         * be deprecated. Example:
         *
         * A and B are master and slave for slots 1,2,3.
         * A is partitioned away, B gets promoted.
         * B is partitioned away, and A returns available.
         *
         * Usually B would PING A publishing its set of served slots and its
         * configEpoch, but because of the partition B can't inform A of the
         * new configuration, so other nodes that have an updated table must
         * do it. In this way A will stop to act as a master (or can try to
         * failover if there are the conditions to win the election). */
        /**
         * 之前已经处理:依据消息里的slot数据以及集群纪元,将slot迁移到sender名下;
         * 
         * 还需要反向检查, sender消息里宣称slot属于自己,但是本机显示该slot属于其他节点且节点纪元更大
         */ 
        if (sender && dirty_slots) {
            int j;

            for (j = 0; j < CLUSTER_SLOTS; j++) {
                if (bitmapTestBit(hdr->myslots,j)) {
                    if (server.cluster->slots[j] == sender ||
                        server.cluster->slots[j] == NULL) continue;
                    if (server.cluster->slots[j]->configEpoch >
                        senderConfigEpoch)
                    {
                        serverLog(LL_VERBOSE,
                            "Node %.40s has old slots configuration, sending "
                            "an UPDATE message about %.40s",
                                sender->name, server.cluster->slots[j]->name);
                        clusterSendUpdate(sender->link,
                            server.cluster->slots[j]);

                        /* TODO: instead of exiting the loop send every other
                         * UPDATE packet for other nodes that are the new owner
                         * of sender's slots. */
                        break;
                    }
                }
            }
        }

        /* If our config epoch collides with the sender's try to fix
         * the problem. */
        /**
         * 处理集群中两个主节点-节点纪元相同的问题
         */ 
        if (sender &&
            nodeIsMaster(myself) && nodeIsMaster(sender) &&
            senderConfigEpoch == myself->configEpoch)
        {
            clusterHandleConfigEpochCollision(sender);
        }

        //至此,已经处理完消息头信息,准备开始处理 已知sender的载体数据
        
        /* Get info from the gossip section */
        if (sender) clusterProcessGossipSection(hdr,link);
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        // 收到一个FAIL消息
        clusterNode *failing;
        /**
         * 本机在收到一个已知节点sender(其实只有主节点才能发出fail消息)发送来关于某节点(无论主备)的FAIL消息时,
         * 如果本机未将该节点标注为FAIL,那么就需要将该节点变为fail.
         * 
         */ 
        if (sender) {
            // 如果发送方为已知节点
            failing = clusterLookupNode(hdr->data.fail.about.nodename);
            if (failing &&
                !(failing->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_MYSELF)))
            {
                // 已知节点 并且 不是本机自己 且 本机认为该节点未处于FAIL状态
                serverLog(LL_NOTICE,
                    "FAIL message received from %.40s about %.40s",
                    hdr->sender, hdr->data.fail.about.nodename);
                // 标记此失败的节点状态为FAIL 并且移除PFAIL状态
                failing->flags |= CLUSTER_NODE_FAIL;
                failing->fail_time = mstime();
                failing->flags &= ~CLUSTER_NODE_PFAIL;//已经确认fail了,尽可能的移除PFAIL标记
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            }
        } else {
            serverLog(LL_NOTICE,
                "Ignoring FAIL message from unknown node %.40s about %.40s",
                hdr->sender, hdr->data.fail.about.nodename);
        }
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        robj *channel, *message;
        uint32_t channel_len, message_len;

        /* Don't bother creating useless objects if there are no
         * Pub/Sub subscribers. */
        if (dictSize(server.pubsub_channels) ||
           listLength(server.pubsub_patterns))
        {
            channel_len = ntohl(hdr->data.publish.msg.channel_len);
            message_len = ntohl(hdr->data.publish.msg.message_len);
            channel = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data,channel_len);
            message = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data+channel_len,
                        message_len);
            pubsubPublishMessage(channel,message);
            decrRefCount(channel);
            decrRefCount(message);
        }
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
        // 本机收到一个请求本机投票给对方的请求
        if (!sender) return 1;  /* We don't know that node. */
        clusterSendFailoverAuthIfNeeded(sender,hdr);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
        // 本机收到投票给自己的消息
        if (!sender) return 1;  /* We don't know that node. */
        /* We consider this vote only if the sender is a master serving
         * a non zero number of slots, and its currentEpoch is greater or
         * equal to epoch where this node started the election. */
        /**
         * 本机收到集群中某个负责slot处理的主节点的投票,并且对方记录的集群纪元>=自己记录的failover_auth_epoch
         */ 
        if (nodeIsMaster(sender) && sender->numslots > 0 &&
            senderCurrentEpoch >= server.cluster->failover_auth_epoch)
        {
            server.cluster->failover_auth_count++;
            /* Maybe we reached a quorum here, set a flag to make sure
             * we check ASAP. */
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
    } else if (type == CLUSTERMSG_TYPE_MFSTART) {
        /* This message is acceptable only if I'm a master and the sender
         * is one of my slaves. */
        if (!sender || sender->slaveof != myself) return 1;
        /* Manual failover requested from slaves. Initialize the state
         * accordingly. */
        resetManualFailover();
        server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;
        server.cluster->mf_slave = sender;
        pauseClients(mstime()+(CLUSTER_MF_TIMEOUT*2));
        serverLog(LL_WARNING,"Manual failover requested by replica %.40s.",
            sender->name);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        // update消息载体里记录的node一定是主节点以及它所负责的槽位图
        clusterNode *n; /* The node the update is about. */
        uint64_t reportedConfigEpoch =
                    ntohu64(hdr->data.update.nodecfg.configEpoch);

        // 本机不认识发送方
        if (!sender) return 1;  /* We don't know the sender. */
        // 本机不认识update消息载体里的主节点
        n = clusterLookupNode(hdr->data.update.nodecfg.nodename);
        if (!n) return 1;   /* We don't know the reported node. */
        // 本机记录的待更新节点的节点纪元足够的新,无需更新
        if (n->configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. */

        /* If in our current config the node is a slave, set it as a master. */
        // 本机记录的待更新节点是个备节点,那么将其设置为主节点
        if (nodeIsSlave(n)) clusterSetNodeAsMaster(n);

        /* Update the node's configEpoch. */
        // 记录待更新节点的最新节点纪元
        n->configEpoch = reportedConfigEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);

        /* Check the bitmap of served slots and update our
         * config accordingly. */
        // 使用指定node的slots信息来更新本机集群slots信息
        clusterUpdateSlotsConfigWith(n,reportedConfigEpoch,
            hdr->data.update.nodecfg.slots);
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        if (!sender) return 1;  /* Protect the module from unknown nodes. */
        /* We need to route this message back to the right module subscribed
         * for the right message type. */
        uint64_t module_id = hdr->data.module.msg.module_id; /* Endian-safe ID */
        uint32_t len = ntohl(hdr->data.module.msg.len);
        uint8_t type = hdr->data.module.msg.type;
        unsigned char *payload = hdr->data.module.msg.bulk_data;
        moduleCallClusterReceivers(sender->name,module_id,type,payload,len);
    } else {
        serverLog(LL_WARNING,"Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
// 内部通信连接异常,直接释放link数据即可
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. */
// 内部通信, 将link中累积的发送缓冲区数据发送出去,如果全部发送完毕,则将该link套接字从事件分配器中注销写事件
void clusterWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    clusterLink *link = (clusterLink*) privdata;
    ssize_t nwritten;
    UNUSED(el);
    UNUSED(mask);

    // 将link中的发送缓存区里的数据写入指定的tcp套接字
    nwritten = write(fd, link->sndbuf, sdslen(link->sndbuf));
    if (nwritten <= 0) {
        serverLog(LL_DEBUG,"I/O error writing to node link: %s",
            (nwritten == -1) ? strerror(errno) : "short write");
        handleLinkIOError(link);
        return;
    }
    // 调整未写完的数据空间
    sdsrange(link->sndbuf,nwritten,-1);
    if (sdslen(link->sndbuf) == 0)
        // 发送缓冲区为空时, 将reactor里注册的套接字移除write事件
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
// 内部通信, 读取指定套接字的数据,并存于link的接收缓冲区里
// 如果已收到完整的消息包,则直接解析执行
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[sizeof(clusterMsg)];//栈内存, 每次从内核TCP协议栈单性read的最多字节数
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = (clusterLink*) privdata;//此套接字对应的link tcp长连接
    unsigned int readlen, rcvbuflen;
    UNUSED(el);
    UNUSED(mask);

    while(1) { /* Read as long as there is data to read. */
        // 之前已读取到的数据长度, 一个完整的数据包,头8个字节非常关键,包含协议标示与数据包整体长度信息,所以必须优先获取头部8字节数据
        // 如下逻辑一定会读头8个字节为一组,之后再读后续的数据
        rcvbuflen = sdslen(link->rcvbuf);
        if (rcvbuflen < 8) {
            /* First, obtain the first 8 bytes to get the full message
             * length. */
            // 不足8字节,算出还剩多少必须优先读取的长度
            readlen = 8 - rcvbuflen;
        } else {
            /* Finally read the full message. */
            hdr = (clusterMsg*) link->rcvbuf;
            if (rcvbuflen == 8) {
                // 已经读取到消息头的最开始8字节,开始校验消息,注意获取到的数据整型都是大尾
                /* Perform some sanity check on the message signature
                 * and length. */
                if (memcmp(hdr->sig,"RCmb",4) != 0 ||
                    ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN)
                {
                    serverLog(LL_WARNING,
                        "Bad message length or signature received "
                        "from Cluster bus.");
                    handleLinkIOError(link);
                    return;
                }
            }
            // 至此totlen字段已经填充了正确有效的数值, 计算还剩余多少待读取的数据
            readlen = ntohl(hdr->totlen) - rcvbuflen;
            // 如果待读取的数据长度超过临时缓冲区大小,则本次先读取缓冲区长度的数据
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        nread = read(fd,buf,readlen);
        // 非阻塞句柄,只有read()返回-1并且errno为EAGAIN,表示内核已无数据,待到下次再读取
        if (nread == -1 && errno == EAGAIN) return; /* No more data ready. */

        if (nread <= 0) {
            /* I/O error... */
            // 网络出错
            serverLog(LL_DEBUG,"I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : strerror(errno));
            handleLinkIOError(link);
            return;
        } else {
            /* Read data and recast the pointer to the new buffer. */
            // 将新读取到的数据添加到接收缓冲区的尾部
            link->rcvbuf = sdscatlen(link->rcvbuf,buf,nread);
            hdr = (clusterMsg*) link->rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. */
        // 已经读取到一个完整的数据包则开始解析处理,所有的数据均已放在link中的recvbuf
        if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->totlen)) {
            if (clusterProcessPacket(link)) {
                // 当次一个数据包已经处理完毕,则置空
                sdsfree(link->rcvbuf);
                link->rcvbuf = sdsempty();
            } else {
                return; /* Link no longer valid. */
            }
        }
    }
}

/* Put stuff into the send buffer.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. */
// 将已经攒好的待发送数据写入指定link的发送缓冲区里
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {
    if (sdslen(link->sndbuf) == 0 && msglen != 0)
        // 之前未注册过, 本次需要将link的套接字句柄注册到reactor注册器中,准备发送数据
        aeCreateFileEvent(server.el,link->fd,AE_WRITABLE|AE_BARRIER,
                    clusterWriteHandler,link);
    // 新消息添加到发送缓冲区的尾部
    link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);

    // 更新对应的统计信息
    /* Populate sent messages stats. */
    clusterMsg *hdr = (clusterMsg*) msg;
    uint16_t type = ntohs(hdr->type);
    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->stats_bus_messages_sent[type]++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
// 将指定的数据统一广播到集群里的所有主备节点,除了本节点以及处于握手中的节点
void clusterBroadcastMessage(void *buf, size_t len) {
    dictIterator *di;
    dictEntry *de;

    // 获取集群中所有节点的迭代器
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        // 当前节点没有link长连接则跳过
        if (!node->link) continue;
        // 处于握手中的节点or本节点
        if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
            continue;
        // 通过link发送消息
        clusterSendMessage(node->link,buf,len);
    }
    dictReleaseIterator(di);
}

/* Build the message header. hdr must point to a buffer at least
 * sizeof(clusterMsg) in bytes. */
// 通用构造一个cluster消息体,消息类型为指定入参type
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen = 0;
    uint64_t offset;
    clusterNode *master;

    /* If this node is a master, we send its slots bitmap and configEpoch.
     * If this node is a slave we send the master's information instead (the
     * node is flagged as slave so the receiver knows that it is NOT really
     * in charge for this slots. */
    // 广播的消息中的位图是需要是主节点的位图
    // 所以对于当前节点为主节点,则使用自己node中的槽位图数据
    // 对于当前节点为备节点,则使用自己复制的主节点中的槽位图数据
    master = (nodeIsSlave(myself) && myself->slaveof) ?
              myself->slaveof : myself;

    memset(hdr,0,sizeof(*hdr));
    hdr->ver = htons(CLUSTER_PROTO_VER);//协议版本号,目前为1 并转为大尾格式
    // 如下放置方式已经是大尾格式
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';
    hdr->type = htons(type);
    // 调用此函数的节点就是发送消息方,填充进sender字段
    memcpy(hdr->sender,myself->name,CLUSTER_NAMELEN);

    /* If cluster-announce-ip option is enabled, force the receivers of our
     * packets to use the specified address for this node. Otherwise if the
     * first byte is zero, they'll do auto discovery. */
    memset(hdr->myip,0,NET_IP_STR_LEN);
    if (server.cluster_announce_ip) {
        // 配置文件中配置了cluster_announce_ip则将此配置填入消息中的发送方ip地址
        strncpy(hdr->myip,server.cluster_announce_ip,NET_IP_STR_LEN);
        hdr->myip[NET_IP_STR_LEN-1] = '\0';
    }

    /* Handle cluster-announce-port as well. */
    int announced_port = server.cluster_announce_port ?
                         server.cluster_announce_port : server.port;
    int announced_cport = server.cluster_announce_bus_port ?
                          server.cluster_announce_bus_port :
                          (server.port + CLUSTER_PORT_INCR);

    // 填充主节点中记录的槽位图
    memcpy(hdr->myslots,master->slots,sizeof(hdr->myslots));
    // 填充发送方所隶属的主节点名称
    memset(hdr->slaveof,0,CLUSTER_NAMELEN);
    if (myself->slaveof != NULL)
        memcpy(hdr->slaveof,myself->slaveof->name, CLUSTER_NAMELEN);
    hdr->port = htons(announced_port);
    hdr->cport = htons(announced_cport);
    hdr->flags = htons(myself->flags);
    // 填充发送方记录的集群状态字段
    hdr->state = server.cluster->state;

    /* Set the currentEpoch and configEpochs. */
    hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
    hdr->configEpoch = htonu64(master->configEpoch);

    /* Set the replication offset. */
    if (nodeIsSlave(myself))
        offset = replicationGetSlaveOffset();
    else
        offset = server.master_repl_offset;
    hdr->offset = htonu64(offset);

    /* Set the message flags. */
    if (nodeIsMaster(myself) && server.cluster->mf_end)
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

    /* Compute the message length for certain messages. For other messages
     * this is up to the caller. */
    // 计算整个消息体的长度,根据不同消息类型进行计算
    if (type == CLUSTERMSG_TYPE_FAIL) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataFail);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataUpdate);
    }
    hdr->totlen = htonl(totlen);
    /* For PING, PONG, and MEET, fixing the totlen field is up to the caller. */
}

/* Return non zero if the node is already present in the gossip section of the
 * message pointed by 'hdr' and having 'count' gossip entries. Otherwise
 * zero is returned. Helper for clusterSendPing(). */
// 检查指定的节点是否在gossip消息体中
// 采用从头遍历比较的方式进行查重处理
int clusterNodeIsInGossipSection(clusterMsg *hdr, int count, clusterNode *n) {
    int j;
    for (j = 0; j < count; j++) {
        if (memcmp(hdr->data.ping.gossip[j].nodename,n->name,
                CLUSTER_NAMELEN) == 0) break;
    }
    return j != count;
}

/* Set the i-th entry of the gossip section in the message pointed by 'hdr'
 * to the info of the specified node 'n'. */
// 将指定的节点信息塞入到下标为i的gossip消息体中
// 消息体整数型数据均为大尾格式
void clusterSetGossipEntry(clusterMsg *hdr, int i, clusterNode *n) {
    clusterMsgDataGossip *gossip;
    gossip = &(hdr->data.ping.gossip[i]);
    memcpy(gossip->nodename,n->name,CLUSTER_NAMELEN);
    gossip->ping_sent = htonl(n->ping_sent/1000);
    gossip->pong_received = htonl(n->pong_received/1000);
    memcpy(gossip->ip,n->ip,sizeof(n->ip));
    gossip->port = htons(n->port);
    gossip->cport = htons(n->cport);
    gossip->flags = htons(n->flags);
    gossip->notused1 = 0;
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip informations. */
// 向指定的link长连接里发送指定类型的ping/pong消息
// 本函数内对于ping消息会将该link的node里的ping_sent设置为最新时间戳
// 发送ping与pong信息整体结构相同，只有一处有差异：ping类型的消息会更新对应node->ping_sent字段。
/**
 * 消息体中携带的节点数据含有:
 * 1. 随机获取一定数目的主备节点数据(不包含本机/处于pfail/处于握手中/没有地址/已断开且没有槽位的节点)
 * 2. 所有处于pfail的主备节点(不包含处于pfail/处于握手中的节点)
 */ 
void clusterSendPing(clusterLink *link, int type) {
    unsigned char *buf;
    clusterMsg *hdr;
    int gossipcount = 0; /* Number of gossip sections added so far. */
    int wanted; /* Number of gossip sections we want to append if possible. */
    int totlen; /* Total packet length. */
    /* freshnodes is the max number of nodes we can hope to append at all:
     * nodes available minus two (ourself and the node we are sending the
     * message to). However practically there may be less valid nodes since
     * nodes in handshake state, disconnected, are not considered. */
    // 需要填充的最多节点信息(排除掉自身节点与接收此消息的节点)
    int freshnodes = dictSize(server.cluster->nodes)-2;

    /* How many gossip sections we want to add? 1/10 of the number of nodes
     * and anyway at least 3. Why 1/10?
     *
     * If we have N masters, with N/10 entries, and we consider that in
     * node_timeout we exchange with each other node at least 4 packets
     * (we ping in the worst case in node_timeout/2 time, and we also
     * receive two pings from the host), we have a total of 8 packets
     * in the node_timeout*2 falure reports validity time. So we have
     * that, for a single PFAIL node, we can expect to receive the following
     * number of failure reports (in the specified window of time):
     *
     * PROB * GOSSIP_ENTRIES_PER_PACKET * TOTAL_PACKETS:
     *
     * PROB = probability of being featured in a single gossip entry,
     *        which is 1 / NUM_OF_NODES.
     * ENTRIES = 10.
     * TOTAL_PACKETS = 2 * 4 * NUM_OF_MASTERS.
     *
     * If we assume we have just masters (so num of nodes and num of masters
     * is the same), with 1/10 we always get over the majority, and specifically
     * 80% of the number of nodes, to account for many masters failing at the
     * same time.
     *
     * Since we have non-voting slaves that lower the probability of an entry
     * to feature our node, we set the number of entries per packet as
     * 10% of the total nodes we have. */
    // 期望拼接节点的个数为集群规模的十分之一,最小值为3,如果超过最大值,则以最大值为界
    wanted = floor(dictSize(server.cluster->nodes)/10);
    if (wanted < 3) wanted = 3;
    if (wanted > freshnodes) wanted = freshnodes;

    /* Include all the nodes in PFAIL state, so that failure reports are
     * faster to propagate to go from PFAIL to FAIL state. */
    // 需要包含全部的处于PFAIL状态的节点
    int pfail_wanted = server.cluster->stats_pfail_nodes;

    // 至此需要拼接的总节点个数为wanted+pfail_wanted
    /* Compute the maxium totlen to allocate our buffer. We'll fix the totlen
     * later according to the number of gossip sections we really were able
     * to put inside the packet. */
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*(wanted+pfail_wanted));
    /* Note: clusterBuildMessageHdr() expects the buffer to be always at least
     * sizeof(clusterMsg) or more. */
    // 总长度不能低于消息头的长度
    if (totlen < (int)sizeof(clusterMsg)) totlen = sizeof(clusterMsg);
    buf = zcalloc(totlen);
    hdr = (clusterMsg*) buf;

    /* Populate the header. */
    if (link->node && type == CLUSTERMSG_TYPE_PING)
        link->node->ping_sent = mstime();//如果发送的是ping类型消息,则在此更新此节点的ping_sent字段
    clusterBuildMessageHdr(hdr,type);

    /* Populate the gossip fields */
    int maxiterations = wanted*3;//至多采样次数,是wanted的3倍
    while(freshnodes > 0 && gossipcount < wanted && maxiterations--) {
        // 从集群节点中随机获取一个节点数据
        dictEntry *de = dictGetRandomKey(server.cluster->nodes);
        clusterNode *this = dictGetVal(de);

        /* Don't include this node: the whole packet header is about us
         * already, so we just gossip about other nodes. */
        // 如果采样到自身节点则跳过
        if (this == myself) continue;

        /* PFAIL nodes will be added later. */
        // 如果采样到的节点已经处于PFAIL状态也跳过,因为后面会统一添加所有处于PFAIL状态的节点
        if (this->flags & CLUSTER_NODE_PFAIL) continue;

        /* In the gossip section don't include:
         * 1) Nodes in HANDSHAKE state.
         * 3) Nodes with the NOADDR flag set.
         * 4) Disconnected nodes if they don't have configured slots.
         */
        // 排除一些处于特殊状态的节点:处于握手中的;暂时没有地址的;没有槽位的
        if (this->flags & (CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_NOADDR) ||
            (this->link == NULL && this->numslots == 0))
        {
            freshnodes--; /* Tecnically not correct, but saves CPU. */
            continue;
        }

        /* Do not add a node we already have. */
        // 去重
        if (clusterNodeIsInGossipSection(hdr,gossipcount,this)) continue;

        /* Add it */
        // 将此节点添加到gossip消息体中
        clusterSetGossipEntry(hdr,gossipcount,this);
        freshnodes--;
        gossipcount++;
    }

    /* If there are PFAIL nodes, add them at the end. */
    if (pfail_wanted) {
        // 统一添加所有处于PFAIL状态的节点
        dictIterator *di;
        dictEntry *de;

        // 逐一遍历所有的节点获取处于PFAIL的节点
        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL && pfail_wanted > 0) {
            clusterNode *node = dictGetVal(de);
            // 跳过处于握手中的;没有地址的
            if (node->flags & CLUSTER_NODE_HANDSHAKE) continue;
            if (node->flags & CLUSTER_NODE_NOADDR) continue;
            if (!(node->flags & CLUSTER_NODE_PFAIL)) continue;
            clusterSetGossipEntry(hdr,gossipcount,node);
            freshnodes--;
            gossipcount++;
            /* We take the count of the slots we allocated, since the
             * PFAIL stats may not match perfectly with the current number
             * of PFAIL nodes. */
            pfail_wanted--;
        }
        dictReleaseIterator(di);
    }

    /* Ready to send... fix the totlen fiend and queue the message in the
     * output buffer. */
    // 统一整理一下待发送的数据边界,因为实际攒的数据会小于开辟的空间
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*gossipcount);
    hdr->count = htons(gossipcount);
    hdr->totlen = htonl(totlen);
    // 将发送消息填充到link的发送缓冲区里
    clusterSendMessage(link,buf,totlen);
    zfree(buf);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * In Redis Cluster pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a slave promotion).
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_SLAVES -> All slaves in my master-slaves ring.
 */
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1 // 发送给同属一个子集群的所有备节点(注意不包含主节点)
// 主动发送pong消息给一定范围的节点
// 只在两种场景下会使用本函数:
//  1. 在故障切换阶段，每一轮新的选举申请发送前，备节点将自己的复制偏移量以pong消息发送给本节点所隶属的主节点名下所有备节点（不包含发送方自身）;
//  2. 在故障切换完毕时, 最终成功当选为主节点的备节点以pong消息向集群内所有节点广播自身记录的集群数据；
void clusterBroadcastPong(int target) {
    dictIterator *di;
    dictEntry *de;

    // 遍历集群里的每个节点,使用其中link用于发送消息使用
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        // 还没有建立外呼的节点跳过
        if (!node->link) continue;
        // 排查给自己发消息以及处于握手状态的节点
        if (node == myself || nodeInHandshake(node)) continue;
        if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
            // 发送范围是备节点的话,需要判定主备关系or同一主节点的备兄弟
            int local_slave =
                nodeIsSlave(node) && node->slaveof &&
                (node->slaveof == myself || node->slaveof == myself->slaveof);
            if (!local_slave) continue;
        }
        clusterSendPing(node->link,CLUSTERMSG_TYPE_PONG);
    }
    dictReleaseIterator(di);
}

/* Send a PUBLISH message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. */
// 向指定link发送publish消息,如果link为空,表示向整个集群发送消息
void clusterSendPublish(clusterLink *link, robj *channel, robj *message) {
    unsigned char buf[sizeof(clusterMsg)], *payload;//栈内存
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;
    uint32_t channel_len, message_len;

    channel = getDecodedObject(channel);//解码为string型的内部对象
    message = getDecodedObject(message);
    channel_len = sdslen(channel->ptr);
    message_len = sdslen(message->ptr);

    // 填充通用的消息头数据
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_PUBLISH);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    // 扣除8个字节的填充符, 另外加上消息头之后跟着的channel字符串与message字符串数据
    totlen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;

    hdr->data.publish.msg.channel_len = htonl(channel_len);
    hdr->data.publish.msg.message_len = htonl(message_len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible */
    if (totlen < sizeof(buf)) {
        // 使用栈空间足够填充本次数据
        payload = buf;
    } else {
        // 栈空间不足,则使用堆内存用于承载本消息数据
        payload = zmalloc(totlen);
        memcpy(payload,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) payload;
    }
    // 依次填充channel内容与message内容
    memcpy(hdr->data.publish.msg.bulk_data,channel->ptr,sdslen(channel->ptr));
    memcpy(hdr->data.publish.msg.bulk_data+sdslen(channel->ptr),
        message->ptr,sdslen(message->ptr));

    if (link)
        // 此消息发送给指定的link
        clusterSendMessage(link,payload,totlen);
    else
        // 此消息发送给所有节点包含主备节点,除了当前节点与处于握手中的节点
        clusterBroadcastMessage(payload,totlen);

    decrRefCount(channel);
    decrRefCount(message);
    // 释放临时的堆内存
    if (payload != buf) zfree(payload);
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (CLUSTER_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to CLUSTER_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. */
// 只有主节点才能向集群里广播指定节点状态为FAIL
void clusterSendFail(char *nodename) {
    unsigned char buf[sizeof(clusterMsg)];//栈内存
    clusterMsg *hdr = (clusterMsg*) buf;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAIL);
    memcpy(hdr->data.fail.about.nodename,nodename,CLUSTER_NAMELEN);
    clusterBroadcastMessage(buf,ntohl(hdr->totlen));
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. */
// 向指定的link对应的节点发送某主节点slot数据的update消息, node一定是主节点,因为只有主节点可以处理slots
// 入参含义: 将node的数据构造update消息体,并发送到link中,这个link不一定隶属于该node
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];//栈内存
    clusterMsg *hdr = (clusterMsg*) buf;

    if (link == NULL) return;
    // 通用方式构造一个update消息
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_UPDATE);
    // 填充update所需的各个字段
    memcpy(hdr->data.update.nodecfg.nodename,node->name,CLUSTER_NAMELEN);
    hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);
    memcpy(hdr->data.update.nodecfg.slots,node->slots,sizeof(node->slots));
    clusterSendMessage(link,buf,ntohl(hdr->totlen));
}

/* Send a MODULE message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. */
void clusterSendModule(clusterLink *link, uint64_t module_id, uint8_t type,
                       unsigned char *payload, uint32_t len) {
    unsigned char buf[sizeof(clusterMsg)], *heapbuf;
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MODULE);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgModule) - 3 + len;

    hdr->data.module.msg.module_id = module_id; /* Already endian adjusted. */
    hdr->data.module.msg.type = type;
    hdr->data.module.msg.len = htonl(len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible */
    if (totlen < sizeof(buf)) {
        heapbuf = buf;
    } else {
        heapbuf = zmalloc(totlen);
        memcpy(heapbuf,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) heapbuf;
    }
    memcpy(hdr->data.module.msg.bulk_data,payload,len);

    if (link)
        clusterSendMessage(link,heapbuf,totlen);
    else
        clusterBroadcastMessage(heapbuf,totlen);

    if (heapbuf != buf) zfree(heapbuf);
}

/* This function gets a cluster node ID string as target, the same way the nodes
 * addresses are represented in the modules side, resolves the node, and sends
 * the message. If the target is NULL the message is broadcasted.
 *
 * The function returns C_OK if the target is valid, otherwise C_ERR is
 * returned. */
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, unsigned char *payload, uint32_t len) {
    clusterNode *node = NULL;

    if (target != NULL) {
        node = clusterLookupNode(target);
        if (node == NULL || node->link == NULL) return C_ERR;
    }

    clusterSendModule(target ? node->link : NULL,
                      module_id, type, payload, len);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * For now we do very little, just propagating PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.
 * -------------------------------------------------------------------------- */
void clusterPropagatePublish(robj *channel, robj *message) {
    clusterSendPublish(NULL, channel, message);
}

/* -----------------------------------------------------------------------------
 * SLAVE node specific functions
 * -------------------------------------------------------------------------- */

/* This function sends a FAILOVE_AUTH_REQUEST message to every node in order to
 * see if there is the quorum for this slave instance to failover its failing
 * master.
 *
 * Note that we send the failover request to everybody, master and slave nodes,
 * but only the masters are supposed to reply to our query. */
// 发起投票申请,将FAILOVER_AUTH_REQUEST消息广播到集群里所有的节点
void clusterRequestFailoverAuth(void) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
    /* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
     * in the header to communicate the nodes receiving the message that
     * they should authorized the failover even if the master is working. */
    // 如果时手工故障迁移,那么在消息头字典里添加一个标记,指示收到此消息的节点应该立即进行故障迁移确认,即使此时的主节点还是正常的情况下.
    if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterBroadcastMessage(buf,totlen);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. */
// 本机作为主节点将投票给指定的节点node, 即发送FAILOVER_AUTH_ACK消息给node节点
void clusterSendFailoverAuth(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,buf,totlen);
}

/* Send a MFSTART message to the specified node. */
// 发送MFSTART消息给指定节点
void clusterSendMFStart(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MFSTART);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,buf,totlen);
}

/* Vote for the node asking for our vote if there are the conditions. */
/**
 * 本机收到某个节点node的投票申请request
 */ 
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {
    clusterNode *master = node->slaveof;
    uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);
    uint64_t requestConfigEpoch = ntohu64(request->configEpoch);
    unsigned char *claimed_slots = request->myslots;
    int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
    int j;

    /* IF we are not a master serving at least 1 slot, we don't have the
     * right to vote, as the cluster size in Redis Cluster is the number
     * of masters serving at least one slot, and quorum is the cluster
     * size + 1 */
    // 本机为备节点 不投票
    // 本机为主节点 但是不负责任何slot槽位 不投票
    if (nodeIsSlave(myself) || myself->numslots == 0) return;

    /* Request epoch must be >= our currentEpoch.
     * Note that it is impossible for it to actually be greater since
     * our currentEpoch was updated as a side effect of receiving this
     * request, if the request epoch was greater. */
    // 请求投票的集群纪元 不如 本机记录的集群纪元新,也不投票
    if (requestCurrentEpoch < server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
            "Failover auth denied to %.40s: reqEpoch (%llu) < curEpoch(%llu)",
            node->name,
            (unsigned long long) requestCurrentEpoch,
            (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* I already voted for this epoch? Return ASAP. */
    // 本机已经投过一次票
    if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: already voted for epoch %llu",
                node->name,
                (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* Node must be a slave and its master down.
     * The master can be non failing if the request is flagged
     * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). */
    if (nodeIsMaster(node) || master == NULL ||
        (!nodeFailed(master) && !force_ack))
    {
        if (nodeIsMaster(node)) {
            // 申请投票的节点 在本机视角看来已经是主节点了,无需投票
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: it is a master node",
                    node->name);
        } else if (master == NULL) {
            // 申请投票的节点原主节点为空,不投票
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: I don't know its master",
                    node->name);
        } else if (!nodeFailed(master)) {
            // 申请投票的节点原主节点 在本机视角看来没有处于fail,不投票
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: its master is up",
                    node->name);
        }
        return;
    }

    /* We did not voted for a slave about this master for two
     * times the node timeout. This is not strictly needed for correctness
     * of the algorithm but makes the base case more linear. */
    // 在一定时间段内, 本机不能对某一个主节点下的子集群选主事项频繁投票
    if (mstime() - node->slaveof->voted_time < server.cluster_node_timeout * 2)
    {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "can't vote about this master before %lld milliseconds",
                node->name,
                (long long) ((server.cluster_node_timeout*2)-
                             (mstime() - node->slaveof->voted_time)));
        return;
    }

    /* The slave requesting the vote must have a configEpoch for the claimed
     * slots that is >= the one of the masters currently serving the same
     * slots in the current configuration. */
    /**
     * 申请方所负责的slot的节点纪元 需要>= 本机记录的节点纪元
     */ 
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(claimed_slots, j) == 0) continue;
        if (server.cluster->slots[j] == NULL ||
            server.cluster->slots[j]->configEpoch <= requestConfigEpoch)
        {
            continue;
        }
        /* If we reached this point we found a slot that in our current slots
         * is served by a master with a greater configEpoch than the one claimed
         * by the slave requesting our vote. Refuse to vote for this slave. */
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "slot %d epoch (%llu) > reqEpoch (%llu)",
                node->name, j,
                (unsigned long long) server.cluster->slots[j]->configEpoch,
                (unsigned long long) requestConfigEpoch);
        return;
    }

    /* We can vote for this slave. */
    // 本机确认投票, 本机记录发出投票确认时的集群纪元
    server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
    // 记录申请投票方所在的子集群主节点投票时间,以避免对该子集群频繁投票
    node->slaveof->voted_time = mstime();
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_FSYNC_CONFIG);
    // 本机投出宝贵的一票
    clusterSendFailoverAuth(node);
    serverLog(LL_WARNING, "Failover auth granted to %.40s for epoch %llu",
        node->name, (unsigned long long) server.cluster->currentEpoch);
}

/* This function returns the "rank" of this instance, a slave, in the context
 * of its master-slaves ring. The rank of the slave is given by the number of
 * other slaves for the same master that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A slave with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple slaves may have the same rank, in case they have the same offset.
 *
 * The slave rank is used to add a delay to start an election in order to
 * get voted and replace a failing master. Slaves with better replication
 * offsets are more likely to win. */
// 只在本机为备节点时,才会调用
// 在本机所在的子集群里,计算本机的rank值,rank值是依据复制主节点偏移量为参考,rank值越小则越有可能成为主
int clusterGetSlaveRank(void) {
    long long myoffset;
    int j, rank = 0;
    clusterNode *master;

    serverAssert(nodeIsSlave(myself));
    master = myself->slaveof;
    if (master == NULL) return 0; /* Never called by slaves without master. */

    // 以当前备节点的复制偏移量为参考
    myoffset = replicationGetSlaveOffset();
    // 遍历子集群里的兄弟节点的偏移量,如果某节点比本机有更新的偏移量,则将本节点的rank+1,表示自己越不适合升为主节点
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] != myself &&
            !nodeCantFailover(master->slaves[j]) &&
            master->slaves[j]->repl_offset > myoffset) rank++;
    return rank;
}

/* This function is called by clusterHandleSlaveFailover() in order to
 * let the slave log why it is not able to failover. Sometimes there are
 * not the conditions, but since the failover function is called again and
 * again, we can't log the same things continuously.
 *
 * This function works by logging only if a given set of conditions are
 * true:
 *
 * 1) The reason for which the failover can't be initiated changed.
 *    The reasons also include a NONE reason we reset the state to
 *    when the slave finds that its master is fine (no FAIL flag).
 * 2) Also, the log is emitted again if the master is still down and
 *    the reason for not failing over is still the same, but more than
 *    CLUSTER_CANT_FAILOVER_RELOG_PERIOD seconds elapsed.
 * 3) Finally, the function only logs if the slave is down for more than
 *    five seconds + NODE_TIMEOUT. This way nothing is logged when a
 *    failover starts in a reasonable time.
 *
 * The function is called with the reason why the slave can't failover
 * which is one of the integer macros CLUSTER_CANT_FAILOVER_*.
 *
 * The function is guaranteed to be called only if 'myself' is a slave. */
// 记录本机作为备节点不能进行故障迁移的原因
void clusterLogCantFailover(int reason) {
    char *msg;
    static time_t lastlog_time = 0;
    mstime_t nolog_fail_time = server.cluster_node_timeout + 5000;

    /* Don't log if we have the same reason for some time. */
    if (reason == server.cluster->cant_failover_reason &&
        time(NULL)-lastlog_time < CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
        return;

    server.cluster->cant_failover_reason = reason;

    /* We also don't emit any log if the master failed no long ago, the
     * goal of this function is to log slaves in a stalled condition for
     * a long time. */
    // 对于本机作为备节点,所隶属的主节点刚刚变为FAIL没多久,暂不记录日志
    // 需要确保只记录稳定态的日志数据
    if (myself->slaveof &&
        nodeFailed(myself->slaveof) &&
        (mstime() - myself->slaveof->fail_time) < nolog_fail_time) return;

    switch(reason) {
    case CLUSTER_CANT_FAILOVER_DATA_AGE:
        msg = "Disconnected from master for longer than allowed. "
              "Please check the 'cluster-replica-validity-factor' configuration "
              "option.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_DELAY:
        msg = "Waiting the delay before I can start a new failover.";
        break;
    case CLUSTER_CANT_FAILOVER_EXPIRED:
        msg = "Failover attempt expired.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_VOTES:
        msg = "Waiting for votes, but majority still not reached.";
        break;
    default:
        msg = "Unknown reason code.";
        break;
    }
    lastlog_time = time(NULL);
    serverLog(LL_WARNING,"Currently unable to failover: %s", msg);
}

/* This function implements the final part of automatic and manual failovers,
 * where the slave grabs its master's hash slots, and propagates the new
 * configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
// 本机作为备节点成为所在子集群的主节点, 并将本机最新的集群数据以pong消息周知集群里所有的主备节点
void clusterFailoverReplaceYourMaster(void) {
    int j;
    clusterNode *oldmaster = myself->slaveof;

    // 确认是否满足条件:本机为备节点 且 当前是有主节点(准备被本机取代)
    if (nodeIsMaster(myself) || oldmaster == NULL) return;

    /* 1) Turn this node into a master. */
    clusterSetNodeAsMaster(myself);
    replicationUnsetMaster();

    /* 2) Claim all the slots assigned to our master. */
    // 将旧主负责的槽位 改为 本机节点来负责
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeGetSlotBit(oldmaster,j)) {
            clusterDelSlot(j);
            clusterAddSlot(myself,j);
        }
    }

    /* 3) Update state and save config. */
    // 本机更新集群状态
    clusterUpdateState();
    // 本机保存最新的集群数据到磁盘配置文件
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state
     *    accordingly and detect that we switched to master role. */
    // 将自身记录的集群状态信息主动以pong消息发送到集群所有主备节点
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) If there was a manual failover in progress, clear the state. */
    // 清除手动故障切换字段
    resetManualFailover();
}

/* This function is called if we are a slave node and our master serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * The gaol of this function is:
 * 1) To check if we are able to perform a failover, is our data updated?
 * 2) Try to get elected by masters.
 * 3) Perform the failover informing all the other nodes.
 */
// 本机尝试进行故障切换
// 故障切换,即由备节点发起申请集群主节点进行投票,替换子集群里的处于FAIL的主节点
// 函数内部会确认是否需要进行故障切换
void clusterHandleSlaveFailover(void) {
    mstime_t data_age;
    // 截止到目前,本轮故障切换过程所花的时长
    mstime_t auth_age = mstime() - server.cluster->failover_auth_time;
    // 投票所需的最低个数
    int needed_quorum = (server.cluster->size / 2) + 1;
    // 是否可以开始手动故障迁移
    int manual_failover = server.cluster->mf_end != 0 &&
                          server.cluster->mf_can_start;
    mstime_t auth_timeout, auth_retry_time;

    // 准备执行故障迁移工作,所以清除todo 标记里的对应的标记
    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

    /* Compute the failover timeout (the max time we have to send votes
     * and wait for replies), and the failover retry time (the time to wait
     * before trying to get voted again).
     *
     * Timeout is MAX(NODE_TIMEOUT*2,2000) milliseconds.
     * Retry is two times the Timeout.
     */
    // 单轮故障切换的超时时长,最小2秒
    auth_timeout = server.cluster_node_timeout*2;
    if (auth_timeout < 2000) auth_timeout = 2000;
    // 本轮故障切换开启后,间隔多久开启下一轮故障切换
    auth_retry_time = auth_timeout*2;

    /**
     * 备节点的故障切换触发后
     * 1. 需要主动发送FAILOVER_AUTH_REQUEST消息给集群里所有的主备节点;
     * 2. 需要等待主节点对自己的投票ACK,并统计票数.
     * 3. 直至本机成为主节点 或者 收到别的节点pong消息发现本机已隶属于某主节点 才中止故障切换.
     * 
     * 上述1,2步骤需要考虑网络超时/无响应重试/票数不足等场景,所以需要重新发起投票申请, 逐轮进行直至故障切换中止.
     * 
     * 为实现该逻辑,这里设置了若干时间变量用于确认故障切换的开启/超时/重试等.
     * 1. server.cluster->failover_auth_time 故障切换的计划开启时间戳,毫秒级,
     *    每轮故障切换超时后会重新设置该计划开启时间戳;
     * 2. auth_age 截止到目前,距离本轮故障切换开启时刻已过的时长,毫秒级,
     *    注意可能为负值,表示当前还未到计划开启时刻; 为正值,表示已按计划开启本轮故障切换,数值就是已开启后的时长;
     * 3. auth_timeout 单轮故障切换过程的超时时长,毫秒级
     * 4. auth_retry_time 本轮故障切换开启后,间隔多久重新开启下一轮故障切换,毫秒级
     */ 

    /* Pre conditions to run the function, that must be met both in case
     * of an automatic or manual failover:
     * 1) We are a slave.
     * 2) Our master is flagged as FAIL, or this is a manual failover.
     * 3) We don't have the no failover configuration set, and this is
     *    not a manual failover.
     * 4) It is serving slots. */
    /**
     * 无需进行故障切换
     * 
     * 当前节点处于如下场景无需进行故障迁移:
     * 1. 当前节点为主节点
     * 2. 当前节点对应的主节点正常 且 没有手工触发迁移
     * 3. 当前配置备节点不进行故障迁移 且 没有手工触发迁移
     * 4. 当前节点对应的主节点负责的slot为空
     */
    if (nodeIsMaster(myself) ||
        myself->slaveof == NULL ||
        (!nodeFailed(myself->slaveof) && !manual_failover) ||
        (server.cluster_slave_no_failover && !manual_failover) ||
        myself->slaveof->numslots == 0)
    {
        /* There are no reasons to failover, so we set the reason why we
         * are returning without failing over to NONE. */
        server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
        return;
    }

    /* Set data_age to the number of seconds we are disconnected from
     * the master. */
    // 计算本备节点数据的年龄
    if (server.repl_state == REPL_STATE_CONNECTED) {
        // 对于在复制中,计算最近一次交互到现在的时间差
        data_age = (mstime_t)(server.unixtime - server.master->lastinteraction)
                   * 1000;
    } else {
        // 除此之外,计算跟主节点连接断开至现在的时间差
        data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
    }

    /* Remove the node timeout from the data age as it is fine that we are
     * disconnected from our master at least for the time it was down to be
     * flagged as FAIL, that's the baseline. */
    // 扣减掉 集群节点间交互允许的最大的超时时间
    if (data_age > server.cluster_node_timeout)
        data_age -= server.cluster_node_timeout;

    /* Check if our data is recent enough according to the slave validity
     * factor configured by the user.
     *
     * Check bypassed for manual failovers. */
    // 计算当前备节点的数据是否足够的新,如果过于陈旧,则不进行自身不会进行故障迁移工作
    if (server.cluster_slave_validity_factor &&
        data_age >
        (((mstime_t)server.repl_ping_slave_period * 1000) +
         (server.cluster_node_timeout * server.cluster_slave_validity_factor)))
    {
        if (!manual_failover) {
            clusterLogCantFailover(CLUSTER_CANT_FAILOVER_DATA_AGE);
            return;
        }
    }

    /* If the previous failover attempt timedout and the retry time has
     * elapsed, we can setup a new one. */
    // 本轮故障迁移耗时已经超过了单次故障迁移所允许的最长时长,则重新开始新的一次故障迁移
    if (auth_age > auth_retry_time) {
        // 重新设置新的故障迁移开始时间
        server.cluster->failover_auth_time = mstime() +
            500 + /* Fixed delay of 500 milliseconds, let FAIL msg propagate. */
            random() % 500; /* Random delay between 0 and 500 milliseconds. */
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_sent = 0;
        server.cluster->failover_auth_rank = clusterGetSlaveRank();
        /* We add another delay that is proportional to the slave rank.
         * Specifically 1 second * rank. This way slaves that have a probably
         * less updated replication offset, are penalized. */
        // 对故障迁移的开始时间做适当的延迟,延迟值是跟rank值有关,对于那些复制数据越少的备节点,这样延迟迁移开始时间对其进行惩罚
        server.cluster->failover_auth_time +=
            server.cluster->failover_auth_rank * 1000;
        /* However if this is a manual failover, no delay is needed. */
        // 如果时手工故障迁移,则立即进行
        if (server.cluster->mf_end) {
            server.cluster->failover_auth_time = mstime();
            server.cluster->failover_auth_rank = 0;
	        clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
        serverLog(LL_WARNING,
            "Start of election delayed for %lld milliseconds "
            "(rank #%d, offset %lld).",
            server.cluster->failover_auth_time - mstime(),
            server.cluster->failover_auth_rank,
            replicationGetSlaveOffset());
        /* Now that we have a scheduled election, broadcast our offset
         * to all the other slaves so that they'll updated their offsets
         * if our offset is better. */
        // 将本备节点的复制偏移量信息广播到小群里的所有兄弟备节点or直系主节点
        clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
        return;
    }

    /* It is possible that we received more updated offsets from other
     * slaves for the same master since we computed our election delay.
     * Update the delay if our rank changed.
     *
     * Not performed if this is a manual failover. */
    // 只要还未发出投票,那么可能因为这期间收到兄弟节点的cluster交互信息,引起本节点rank值的最新变化
    // 所以需要再次计算对应的rank值与故障迁移开始时间,如果rank值变大,则更新延迟时间进行更多的惩罚
    if (server.cluster->failover_auth_sent == 0 &&
        server.cluster->mf_end == 0)
    {
        // 计算新的rank值
        int newrank = clusterGetSlaveRank();
        // 新rank值表示本节点更不适合
        if (newrank > server.cluster->failover_auth_rank) {
            long long added_delay =
                (newrank - server.cluster->failover_auth_rank) * 1000;
            // 将数值添加到故障迁移开始时刻
            server.cluster->failover_auth_time += added_delay;
            server.cluster->failover_auth_rank = newrank;
            serverLog(LL_WARNING,
                "Replica rank updated to #%d, added %lld milliseconds of delay.",
                newrank, added_delay);
        }
    }

    /* Return ASAP if we can't still start the election. */
    // 当前时刻还未到故障迁移的开始时间,结合之前的惩罚方法,也就是说对于rank值越大,即复制数据越少的节点,越推迟选举
    if (mstime() < server.cluster->failover_auth_time) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_DELAY);
        return;
    }

    /* Return ASAP if the election is too old to be valid. */
    // 本轮故障切换已经超时,不适宜继续,等待下一轮
    if (auth_age > auth_timeout) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_EXPIRED);
        return;
    }

    /* Ask for votes if needed. */
    // 本轮还未发过选举申请,则调用函数发送选举申请消息
    if (server.cluster->failover_auth_sent == 0) {
        // 将集群纪元增1
        server.cluster->currentEpoch++;
        server.cluster->failover_auth_epoch = server.cluster->currentEpoch;
        serverLog(LL_WARNING,"Starting a failover election for epoch %llu.",
            (unsigned long long) server.cluster->currentEpoch);
        // 发送选举申请并携带本节点的信息
        clusterRequestFailoverAuth();
        server.cluster->failover_auth_sent = 1;
        // 因为有集群纪元的更新,所以需要异步保存配置文件并更新集群state
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
        return; /* Wait for replies. */
    }

    /* Check if we reached the quorum. */
    if (server.cluster->failover_auth_count >= needed_quorum) {
        // 本机收到足够多的主节点票数,本机成为所在子集群的主节点
        /* We have the quorum, we can finally failover the master. */

        serverLog(LL_WARNING,
            "Failover election won: I'm the new master.");

        /* Update my configEpoch to the epoch of the election. */
        // 将本机的节点纪元设置为集群纪元,表示本机成为主节点是集群里的最新事件
        if (myself->configEpoch < server.cluster->failover_auth_epoch) {
            myself->configEpoch = server.cluster->failover_auth_epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu after successful failover",
                (unsigned long long) myself->configEpoch);
        }

        /* Take responsibility for the cluster slots. */
        clusterFailoverReplaceYourMaster();
    } else {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_VOTES);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER slave migration
 *
 * Slave migration is the process that allows a slave of a master that is
 * already covered by at least another slave, to "migrate" to a master that
 * is orpaned, that is, left with no working slaves.
 * ------------------------------------------------------------------------- */

/* This function is responsible to decide if this replica should be migrated
 * to a different (orphaned) master. It is called by the clusterCron() function
 * only if:
 *
 * 1) We are a slave node.
 * 2) It was detected that there is at least one orphaned master in
 *    the cluster.
 * 3) We are a slave of one of the masters with the greatest number of
 *    slaves.
 *
 * This checks are performed by the caller since it requires to iterate
 * the nodes anyway, so we spend time into clusterHandleSlaveMigration()
 * if definitely needed.
 *
 * The fuction is called with a pre-computed max_slaves, that is the max
 * number of working (not in FAIL state) slaves for a single master.
 *
 * Additional conditions for migration are examined inside the function.
 */
// 此函数只会在备节点 且 当前备节点所在主节点下面有最多的ok备节点个数
// 如果合适的话,当前备节点会迁移至孤儿主节点下面
void clusterHandleSlaveMigration(int max_slaves) {
    int j, okslaves = 0;
    clusterNode *mymaster = myself->slaveof, *target = NULL, *candidate = NULL;
    dictIterator *di;
    dictEntry *de;

    /* Step 1: Don't migrate if the cluster state is not ok. */
    if (server.cluster->state != CLUSTER_OK) return;

    /* Step 2: Don't migrate if my master will not be left with at least
     *         'migration-barrier' slaves after my migration. */
    if (mymaster == NULL) return;
    // 再次统计本小群里的状态正常的备节点个数
    for (j = 0; j < mymaster->numslaves; j++)
        if (!nodeFailed(mymaster->slaves[j]) &&
            !nodeTimedOut(mymaster->slaves[j])) okslaves++;
    // 如果状态正常的备节点个数低于阈值,则中止漂移操作
    if (okslaves <= server.cluster_migration_barrier) return;

    /* Step 3: Identify a candidate for migration, and check if among the
     * masters with the greatest number of ok slaves, I'm the one with the
     * smallest node ID (the "candidate slave").
     *
     * Note: this means that eventually a replica migration will occur
     * since slaves that are reachable again always have their FAIL flag
     * cleared, so eventually there must be a candidate. At the same time
     * this does not mean that there are no race conditions possible (two
     * slaves migrating at the same time), but this is unlikely to
     * happen, and harmless when happens. */
    // 假定当前节点为候选人,后续会选择nodeid最小值为候选人
    candidate = myself;
    // 从整个集群里统计最合适进行漂移操作的备节点id
    di = dictGetSafeIterator(server.cluster->nodes);
    // 寻找孤儿主节点target
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int okslaves = 0, is_orphaned = 1;

        /* We want to migrate only if this master is working, orphaned, and
         * used to have slaves or if failed over a master that had slaves
         * (MIGRATE_TO flag). This way we only migrate to instances that were
         * supposed to have replicas. */
        // 逐个遍历主节点,找到第一个无备的主节点,该节点就是target,是要被挂载的主节点
        if (nodeIsSlave(node) || nodeFailed(node)) is_orphaned = 0;
        // 本节点之前就标记为不适合做主节点,那么本节点就不符合孤儿节点
        if (!(node->flags & CLUSTER_NODE_MIGRATE_TO)) is_orphaned = 0;

        /* Check number of working slaves. */
        if (nodeIsMaster(node)) okslaves = clusterCountNonFailingSlaves(node);
        if (okslaves > 0) is_orphaned = 0;

        if (is_orphaned) {
            if (!target && node->numslots > 0) target = node;

            /* Track the starting time of the orphaned condition for this
             * master. */
            if (!node->orphaned_time) node->orphaned_time = mstime();
        } else {
            // 重置此节点的孤儿状态时刻为0,表示非孤儿主节点
            node->orphaned_time = 0;
        }

        /* Check if I'm the slave candidate for the migration: attached
         * to a master with the maximum number of slaves and with the smallest
         * node ID. */
        // 从所有含有最多的备节点子集群里选择id最小的节点,这个节点就是要漂移走的节点
        if (okslaves == max_slaves) {
            // 确认开始选择正式候选人操作,选择nodeid最小值
            for (j = 0; j < node->numslaves; j++) {
                if (memcmp(node->slaves[j]->name,
                           candidate->name,
                           CLUSTER_NAMELEN) < 0)
                {
                    candidate = node->slaves[j];
                }
            }
        }
    }
    dictReleaseIterator(di);

    /* Step 4: perform the migration if there is a target, and if I'm the
     * candidate, but only if the master is continuously orphaned for a
     * couple of seconds, so that during failovers, we give some time to
     * the natural slaves of this instance to advertise their switch from
     * the old master to the new one. */
    // 再次确认当前备节点是否可以迁移到孤儿主节点下面
    if (target && candidate == myself &&
        (mstime()-target->orphaned_time) > CLUSTER_SLAVE_MIGRATION_DELAY &&
       !(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
    {
        serverLog(LL_WARNING,"Migrating to orphaned master %.40s",
            target->name);
        clusterSetMaster(target);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by slaves during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Slave sends a MFSTART message to the master requesting to pause clients
 *    for two times the manual failover timeout CLUSTER_MF_TIMEOUT.
 *    When master is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Slave waits for master to send its replication offset flagged as PAUSED.
 * 4) If slave received the offset from the master, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleSlaveFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force masters to vote for a slave that has a
 *    working master.
 *
 * From the point of view of the master things are simpler: when a
 * PAUSE_CLIENTS packet is received the master sets mf_end as well and
 * the sender in mf_slave. During the time limit for the manual failover
 * the master will just send PINGs more often to this slave, flagged with
 * the PAUSED flag, so that the slave will set mf_master_offset when receiving
 * a packet from the master with this flag set.
 *
 * The gaol of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous master-slave replication.
 * -------------------------------------------------------------------------- */

/* Reset the manual failover state. This works for both masters and slavesa
 * as all the state about manual failover is cleared.
 *
 * The function can be used both to initialize the manual failover state at
 * startup or to abort a manual failover in progress. */
// 重置手工迁移的相关字段数据
void resetManualFailover(void) {
    if (server.cluster->mf_end && clientsArePaused()) {
        // 表示目前还依旧有处于暂停的client,所以如下需要将暂停状态撤销
        server.clients_pause_end_time = 0;
        clientsArePaused(); /* Just use the side effect of the function. */
    }
    server.cluster->mf_end = 0; /* No manual failover in progress. */
    server.cluster->mf_can_start = 0;
    server.cluster->mf_slave = NULL;
    server.cluster->mf_master_offset = 0;
}

/* If a manual failover timed out, abort it. */
// 如果手工迁移时刻过期,则撤销手工迁移数据
void manualFailoverCheckTimeout(void) {
    if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
        serverLog(LL_WARNING,"Manual failover timed out.");
        resetManualFailover();
    }
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. */
// 确认手工迁移条件是否开始
void clusterHandleManualFailover(void) {
    /* Return ASAP if no manual failover is in progress. */
    // 当前没有手工迁移
    if (server.cluster->mf_end == 0) return;

    /* If mf_can_start is non-zero, the failover was already triggered so the
     * next steps are performed by clusterHandleSlaveFailover(). */
    // 手工迁移已经开始
    if (server.cluster->mf_can_start) return;

    if (server.cluster->mf_master_offset == 0) return; /* Wait for offset... */

    if (server.cluster->mf_master_offset == replicationGetSlaveOffset()) {
        /* Our replication offset matches the master replication offset
         * announced after clients were paused. We can start the failover. */
        server.cluster->mf_can_start = 1;
        serverLog(LL_WARNING,
            "All master replication stream processed, "
            "manual failover can start.");
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

/* This is executed 10 times every second */
// 循环定时执行入口, 目前是间隔100ms执行一次
void clusterCron(void) {
    dictIterator *di;
    dictEntry *de;
    int update_state = 0;
    // 当前集群里,孤儿主节点的个数
    int orphaned_masters; /* How many masters there are without ok slaves. */
    // 当前各个子集群中,含有最多的备节点(非失败)数目
    int max_slaves; /* Max number of ok slaves for a single master. */
    // 本机所隶属于的主节点下面备节点(非失败)的个数
    int this_slaves; /* Number of ok slaves for our master (if we are slave). */
    mstime_t min_pong = 0, now = mstime();
    clusterNode *min_pong_node = NULL;
    // 函数内可见的长生命周期变量
    static unsigned long long iteration = 0;
    mstime_t handshake_timeout;

    iteration++; /* Number of times this function was called so far. */

    /* We want to take myself->ip in sync with the cluster-announce-ip option.
     * The option can be set at runtime via CONFIG SET, so we periodically check
     * if the option changed to reflect this into myself->ip. */
    {
        // cluster_announce_ip 在进程运行后,可以手动指定本机的ip地址,所以需要周期性确认此值是否有变化,
        // 如有变化,则在此时进行替换更新
        static char *prev_ip = NULL; // 局部可见的长生命周期变量,单独内存存储当前在使用的本机ip地址
        char *curr_ip = server.cluster_announce_ip; //配置的本机ip地址
        int changed = 0;

        // 校验ip前后是否有变化
        if (prev_ip == NULL && curr_ip != NULL) changed = 1;
        else if (prev_ip != NULL && curr_ip == NULL) changed = 1;
        else if (prev_ip && curr_ip && strcmp(prev_ip,curr_ip)) changed = 1;

        if (changed) {
            // 此时需要使用最新的announceip进行更新替换
            // 更新前值为当前值,备后续使用
            if (prev_ip) zfree(prev_ip);
            prev_ip = curr_ip;

            if (curr_ip) {
                /* We always take a copy of the previous IP address, by
                 * duplicating the string. This way later we can check if
                 * the address really changed. */
                // 单独分配内存存储最新的ip地址
                prev_ip = zstrdup(prev_ip);
                // 使用现值更新ip地址
                strncpy(myself->ip,server.cluster_announce_ip,NET_IP_STR_LEN);
                myself->ip[NET_IP_STR_LEN-1] = '\0';
            } else {
                // 将ip地址置为空,由系统来决定
                myself->ip[0] = '\0'; /* Force autodetection. */
            }
        }
    }

    /* The handshake timeout is the time after which a handshake node that was
     * not turned into a normal node is removed from the nodes. Usually it is
     * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
     * the value of 1 second. */
    // 配置的握手超时数值,如果小于1s则使用1s
    // 后续在遍历每个节点时,对于处于握手状态的节点会根据此阈值进行摘除
    handshake_timeout = server.cluster_node_timeout;
    if (handshake_timeout < 1000) handshake_timeout = 1000;

    /* Update myself flags. */
    clusterUpdateMyselfFlags();

    /* Check if we have disconnected nodes and re-establish the connection.
     * Also update a few stats while we are here, that can be used to make
     * better decisions in other part of the code. */
    di = dictGetSafeIterator(server.cluster->nodes);
    // 本轮统计的处于pfail状态的计数器清为0
    server.cluster->stats_pfail_nodes = 0;
    /*
    * 本处while循环实现的功能如下
    * 1. 以当前节点的视角统计集群中处于pfail的节点个数
    * 2. 针对依旧处于握手连接中的节点检查是否超时,如果超时则主动删除node
    * 3. 对于node->link为空的,表示需要主动connect建立TCP连接, 创建link, 
    *    并将ping/meet消息放到该link的待发送缓冲区内,一旦tcp建立完毕则立即发出
    */
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        /* Not interested in reconnecting the link with myself or nodes
         * for which we have no address. */
        // 跳过本机节点以及无地址的节点
        if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR)) continue;

        // 节点处于pfail状态时计数器增1
        if (node->flags & CLUSTER_NODE_PFAIL)
            server.cluster->stats_pfail_nodes++;

        /* A Node in HANDSHAKE state has a limited lifespan equal to the
         * configured node timeout. */
        // 此节点处于握手状态,且已经超时则直接删除此节点
        if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
            clusterDelNode(node);
            continue;
        }

        /**
         * 节点里的link为空,表示需要建立主动外呼的tcp连接
         */
        if (node->link == NULL) {
            int fd;
            mstime_t old_ping_sent;
            clusterLink *link;
            // 创建新的非阻塞式本地套接字,以第一个已设定的配置地址作为源地址,连接指定的ip+port,供总线通信使用
            fd = anetTcpNonBlockBindConnect(server.neterr, node->ip,
                node->cport, NET_FIRST_BIND_ADDR);
            if (fd == -1) {
                /* We got a synchronous error from connect before
                 * clusterSendPing() had a chance to be called.
                 * If node->ping_sent is zero, failure detection can't work,
                 * so we claim we actually sent a ping now (that will
                 * be really sent as soon as the link is obtained). */
                /**
                 * 业务执行到此处,有几种可能:
                 * 1. 握手状态的node初次一直尝试建立连接失败;
                 * 2. 握手状态的node主动断开link进行重连失败;
                 * 2. 非握手状态的node主动断开link(例如ip地址变化或者网络抖动)进行重连失败.
                 * 
                 * 这两种场景均需要有超时机制进行监测:
                 * 1. 握手状态的node初次超时监测由node->ctime判定,
                 *    超时直接删除node节点;
                 * 
                 * 2. 握手状态的node主动断开link进行重连的超时监测会由node->ping_sent与node->ctime各自判定;
                 * 
                 * 3. 非握手状态的node主动断开link进行重连的超时检测由node->ping_sent兜底,
                 *    超时则将node置为PFAIL状态;
                 * 
                 * 这里就需要对node->ping_sent字段进行初始化赋值,确保其开启超时计时功能.
                 */ 
                if (node->ping_sent == 0) node->ping_sent = mstime();
                serverLog(LL_DEBUG, "Unable to connect to "
                    "Cluster Node [%s]:%d -> %s", node->ip,
                    node->cport, server.neterr);
                // 未建立成功外呼tcp连接,等待下一轮重新尝试创建,直至握手超时or创建成功
                continue;
            }
            // 当前是主动外呼,所以设置了link里的node指针
            // node 与 link相互指向 已经建立完毕
            link = createClusterLink(node);
            link->fd = fd;
            node->link = link;

            // 注册读取事件,长期接收对端发送来的数据
            aeCreateFileEvent(server.el,link->fd,AE_READABLE,
                    clusterReadHandler,link);
            /* Queue a PING in the new connection ASAP: this is crucial
             * to avoid false positives in failure detection.
             *
             * If the node is flagged as MEET, we send a MEET message instead
             * of a PING one, to force the receiver to add us in its node
             * table. */
            // 如果node->ping_sent为0，表示之前的ping都正常收到了pong消息。
            // 如不为0，表示依旧在等待pong消息,那么可能会重发ping消息,但是ping_sent需要维持旧值,以支持超时检测.
            /**
             * 刚刚建立外呼TCP连接后,需要将自身数据及早通知对方
             * 根据节点标识,发送meet消息或者ping消息
             * 注意:发送ping消息的函数内部会设置node->ping_sent时间戳用于超时检测
             * 而当node->ping_sent已经有值的情况下,表示之前已经发送过一个ping消息,在等待pong消息
             * 需要保持原有的node->ping_sent数值表示继续以原值作为起点考虑超时.
             */ 
            old_ping_sent = node->ping_sent;
            clusterSendPing(link, node->flags & CLUSTER_NODE_MEET ?
                    CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
            if (old_ping_sent) {
                /* If there was an active ping before the link was
                 * disconnected, we want to restore the ping time, otherwise
                 * replaced by the clusterSendPing() call. */
                node->ping_sent = old_ping_sent;
            }
            /* We can clear the flag after the first packet is sent.
             * If we'll never receive a PONG, we'll never send new packets
             * to this node. Instead after the PONG is received and we
             * are no longer in meet/handshake status, we want to send
             * normal PING packets. */
            // 已经创建了link与node的关系,需清除MEET标记,后续以ping消息进行沟通
            node->flags &= ~CLUSTER_NODE_MEET;

            serverLog(LL_DEBUG,"Connecting with Node %.40s at %s:%d",
                    node->name, node->ip, node->cport);
        }
    }
    dictReleaseIterator(di);

    /* Ping some random node 1 time every 10 iterations, so that we usually ping
     * one random node every second. */
    /**
     * 每执行10次,主动向集群中的一个已连接且没有处于等待ping回复的节点发送ping消息;
     * 发送过程是随机采样5的节点,以其中pong_received最小的一个节点为目的地
     * 对其发送ping消息
     */ 
    if (!(iteration % 10)) {
        int j;

        /* Check a few random nodes and ping the one with the oldest
         * pong_received time. */
        // 随机5次采样,取出其中最旧的pong_received, 之后对其进行ping
        for (j = 0; j < 5; j++) {
            de = dictGetRandomKey(server.cluster->nodes);
            clusterNode *this = dictGetVal(de);

            /* Don't ping nodes disconnected or with a ping currently active. */
            // 暂时还不知道对方节点地址或者等待ping回复的情况,需要跳过
            if (this->link == NULL || this->ping_sent != 0) continue;
            // 跳过本节点以及处于握手中的节点
            if (this->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
                continue;
            // 计算pong_received值最小的节点
            if (min_pong_node == NULL || min_pong > this->pong_received) {
                min_pong_node = this;
                min_pong = this->pong_received;
            }
        }
        if (min_pong_node) {
            serverLog(LL_DEBUG,"Pinging node %.40s", min_pong_node->name);
            clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
        }
    }

    /* Iterate nodes to check if we need to flag something as failing.
     * This loop is also responsible to:
     * 1) Check if there are orphaned masters (masters without non failing
     *    slaves).
     * 2) Count the max number of non failing slaves for a single master.
     * 3) Count the number of slaves for our master, if we are a slave. */
    orphaned_masters = 0;
    max_slaves = 0;// 遍历所有的主节点,记录一个主节点下面含有最多正常备节点个数
    this_slaves = 0;//当前备节点所在主节点下面正常备节点的个数
    /**
     * 孤儿主节点是指:
     * a. 主节点
     * b. 该节点有负责处理的槽位
     * c. 该节点标记字段flags里含有CLUSTER_NODE_MIGRATE_TO:表示有资格进行数据复制
     * d. 所在子集群里的状态为非FAIL的备节点个数为0;
     * 
     * 遍历所有的集群节点,跳过自身节点/握手中的节点/无地址节点,计算如下事项:
     * 1. 只在本机为备节点时,才会统计孤儿主节点个数orphaned_masters;
     * 2. 只在本机为备节点时,统计所有子集群里,状态处于非FAIL状态的备节点个数的最大值max_slaves;
     * 3. 只在本机为备节点时,记录本机所在子集群里,状态处于非FAIL状态的备节点个数this_slaves;
     * 4. 对于遍历到的node无论主备,如果已发出ping但是未收到pong的时长超过node-timeout一半,则主动断开link准备重连;
     * 5. 对于遍历到的node无论主备,如果未发出过ping,而且当前时间距离最后一次收到pong的时间超过node-timeout一半,则主动发送ping;
     * 6. 集群处于手动故障迁移中, 本机为主节点, 且遍历到的node恰好就是手动故障迁移的备节点,则主动发送一次ping;
     * 7. 对于遍历到的节点无论主备,有超过node-timeout时间未收到ping对应的pong时,将其标记为PFAIL;
     */
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) { 
        clusterNode *node = dictGetVal(de);
        now = mstime(); /* Use an updated time at every iteration. */
        mstime_t delay;

        // 依旧跳过本机;无地址节点;处于握手中的节点
        if (node->flags &
            (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE))
                continue;

        /* Orphaned master check, useful only if the current instance
         * is a slave that may migrate to another master. */
        // 本机为备节点,且遍历到的节点为非失败的主节点
        if (nodeIsSlave(myself) && nodeIsMaster(node) && !nodeFailed(node)) {
            // 计算遍历到的主节点下面处于非失败状态的备节点个数
            int okslaves = clusterCountNonFailingSlaves(node);

            /* A master is orphaned if it is serving a non-zero number of
             * slots, have no working slaves, but used to have at least one
             * slave, or failed over a master that used to have slaves. */
            if (okslaves == 0 && node->numslots > 0 &&
                node->flags & CLUSTER_NODE_MIGRATE_TO)
            {
                // 孤儿主节点:没有备节点但却有负责的槽位
                orphaned_masters++;
            }
            // 从当前备节点的眼中,统计集群中主节点有非失败备节点数最多个数
            if (okslaves > max_slaves) max_slaves = okslaves;
            if (nodeIsSlave(myself) && myself->slaveof == node)
                // 本机所在子集群的主节点为遍历到的节点, 记录本机子集群的非FAIL状态备节点个数
                this_slaves = okslaves;
        }

        /* If we are waiting for the PONG more than half the cluster
         * timeout, reconnect the link: maybe there is a connection
         * issue even if the node is alive. */
        // 对于已经发出ping,在等待pong响应的节点,等待时长超过一半阈值时,释放link结构,以便重新建立TCP外呼连接
        if (node->link && /* is connected */
            /**
             * 很关键的一个壁障
             * 此时node已经存在,而且node->ping_sent一直维持最初始值
             * 
             * 某个时刻如果一旦满足重连link的条件,在没有该壁障的情况下,则每次都会满足条件,导致均会free link
             * 所以需要一个壁障,让新link留有足够的时间进行tcp 3次握手交互
             */ 
            now - node->link->ctime > 
            server.cluster_node_timeout && /* was not already reconnected */
            node->ping_sent && /* we already sent a ping */
            node->pong_received < node->ping_sent && /* still waiting pong */
            /* and we are waiting for the pong more than timeout/2 */
            now - node->ping_sent > server.cluster_node_timeout/2)
        {
            /* Disconnect the link, it will be reconnected automatically. */
            // link为null时,后续会重连
            freeClusterLink(node->link);
        }

        /* If we have currently no active ping in this instance, and the
         * received PONG is older than half the cluster timeout, send
         * a new ping now, to ensure all the nodes are pinged without
         * a too big delay. */
        // 对于长时间没有发送过ping消息的节点主动触发ping消息
        if (node->link &&
            node->ping_sent == 0 &&
            (now - node->pong_received) > server.cluster_node_timeout/2)
        {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* If we are a master and one of the slaves requested a manual
         * failover, ping it continuously. */
        // 当前节点为主节点,而备节点处于手动迁移状态,则发送ping消息
        if (server.cluster->mf_end &&
            nodeIsMaster(myself) &&
            server.cluster->mf_slave == node &&
            node->link)
        {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* Check only if we have an active ping for this instance. */
        // 至此表示无ping等待回复
        if (node->ping_sent == 0) continue;

        /* Compute the delay of the PONG. Note that if we already received
         * the PONG, then node->ping_sent is zero, so can't reach this
         * code at all. */
        // 从本机视角检查该node是否疑似失败
        delay = now - node->ping_sent;

        if (delay > server.cluster_node_timeout) {
            // 当前节点判定超时
            /* Timeout reached. Set the node as possibly failing if it is
             * not already in this state. */
            if (!(node->flags & (CLUSTER_NODE_PFAIL|CLUSTER_NODE_FAIL))) {
                serverLog(LL_DEBUG,"*** NODE %.40s possibly failing",
                    node->name);
                // 节点标记为PFAIL
                node->flags |= CLUSTER_NODE_PFAIL;
                update_state = 1;
            }
        }
    }
    dictReleaseIterator(di);

    /* If we are a slave node but the replication is still turned off,
     * enable it if we know the address of our master and it appears to
     * be up. */
    // 本机为备节点,但是未与对应的主节点建立复制连接,则与主节点准备建立复制连接
    if (nodeIsSlave(myself) &&
        server.masterhost == NULL &&
        myself->slaveof &&
        nodeHasAddr(myself->slaveof))
    {
        replicationSetMaster(myself->slaveof->ip, myself->slaveof->port);
    }

    /* Abourt a manual failover if the timeout is reached. */
    // 如果手工迁移时刻过期,则撤销手工迁移数据
    manualFailoverCheckTimeout();

    if (nodeIsSlave(myself)) {
        clusterHandleManualFailover();
        if (!(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
            clusterHandleSlaveFailover();
        /* If there are orphaned slaves, and we are a slave among the masters
         * with the max number of non-failing slaves, consider migrating to
         * the orphaned masters. Note that it does not make sense to try
         * a migration if there is no master with at least *two* working
         * slaves. */
        // 本机作为备节点是否具备迁移至其他孤儿主节点下面
        // 确保集群中存有孤儿主节点,而且集群中存有多于2个备节点的主节点,
        // 且本机备节点所在的子集群正好是含有最多备节点的子集群
        if (orphaned_masters && max_slaves >= 2 && this_slaves == max_slaves)
            clusterHandleSlaveMigration(max_slaves);
    }

    if (update_state || server.cluster->state == CLUSTER_FAIL)
        clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. */
void clusterBeforeSleep(void) {
    /* Handle failover, this is needed when it is likely that there is already
     * the quorum from masters in order to react fast. */
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_HANDLE_FAILOVER)
        clusterHandleSlaveFailover();

    /* Update the cluster state. */
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_UPDATE_STATE)
        clusterUpdateState();

    /* Save the config, possibly using fsync. */
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_SAVE_CONFIG) {
        int fsync = server.cluster->todo_before_sleep &
                    CLUSTER_TODO_FSYNC_CONFIG;
        clusterSaveConfigOrDie(fsync);
    }

    /* Reset our flags (not strictly needed since every single function
     * called for flags set should be able to clear its flag). */
    server.cluster->todo_before_sleep = 0;
}

void clusterDoBeforeSleep(int flags) {
    server.cluster->todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * -------------------------------------------------------------------------- */

/* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
 * otherwise 0. */
// 探测某一位的bit数值是否为1
int bitmapTestBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;//定位到某一个字节
    int bit = pos&7;//定位到单字节中的某一位
    return (bitmap[byte] & (1<<bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. */
void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] |= 1<<bit;
}

/* Clear the bit at position 'pos' in a bitmap. */
// 清除位图里的某一位
void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;//定位到某一个字节
    int bit = pos&7;//定位到单字节中的某一位
    bitmap[byte] &= ~(1<<bit); //将此位置为0
}

/* Return non-zero if there is at least one master with slaves in the cluster.
 * Otherwise zero is returned. Used by clusterNodeSetSlotBit() to set the
 * MIGRATE_TO flag the when a master gets the first slot. */
// 本机记录的集群节点中,至少有一个主节点名下含有备节点,则本函数返回1,否则返回0
int clusterMastersHaveSlaves(void) {
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    int slaves = 0;
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (nodeIsSlave(node)) continue;
        slaves += node->numslaves;
    }
    dictReleaseIterator(di);
    return slaves != 0;
}

/* Set the slot bit and return the old value. */
// 将指定slot槽位分配给指定的node节点,返回值表示该节点node的之前是否负责该slot槽位
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots,slot);
    bitmapSetBit(n->slots,slot);
    if (!old) {
        n->numslots++;
        /* When a master gets its first slot, even if it has no slaves,
         * it gets flagged with MIGRATE_TO, that is, the master is a valid
         * target for replicas migration, if and only if at least one of
         * the other masters has slaves right now.
         *
         * Normally masters are valid targerts of replica migration if:
         * 1. The used to have slaves (but no longer have).
         * 2. They are slaves failing over a master that used to have slaves.
         *
         * However new masters with slots assigned are considered valid
         * migration tagets if the rest of the cluster is not a slave-less.
         *
         * See https://github.com/antirez/redis/issues/3043 for more info. */
        if (n->numslots == 1 && clusterMastersHaveSlaves())
            // 该节点有负责的slot且有备节点,则表示该节点有资格进行复制
            n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    return old;
}

/* Clear the slot bit and return the old value. */
// 清除指定节点里的某一组node-->槽位映射关系
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    // 探测指定位上的旧值
    int old = bitmapTestBit(n->slots,slot);
    // 清除指定位上的数值
    bitmapClearBit(n->slots,slot);
    // 如果旧值为1, 那么需要将此节点负责的槽位个数减1
    if (old) n->numslots--;
    return old;
}

/* Return the slot bit from the cluster node structure. */
// 判定此槽位是否由指定节点负责处理, 内部依据指定节点的槽位图来判定
int clusterNodeGetSlotBit(clusterNode *n, int slot) {
    return bitmapTestBit(n->slots,slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return C_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and C_ERR is returned. */
// 设定slot槽位由指定node节点负责
int clusterAddSlot(clusterNode *n, int slot) {
    if (server.cluster->slots[slot]) return C_ERR;
    clusterNodeSetSlotBit(n,slot);
    server.cluster->slots[slot] = n;
    return C_OK;
}

/* Delete the specified slot marking it as unassigned.
 * Returns C_OK if the slot was assigned, otherwise if the slot was
 * already unassigned C_ERR is returned. */
// 将指定槽位的 槽位与节点互为映射关系删除
// 注意: 槽位-->node的映射关系在全局的clusterState结构体中;
//      node-->槽位的映射关系在各自的clusterNode结构体中;
int clusterDelSlot(int slot) {
    clusterNode *n = server.cluster->slots[slot];

    if (!n) return C_ERR;
    // 清除node-->槽位的映射关系
    serverAssert(clusterNodeClearSlotBit(n,slot) == 1);
    // 清除槽位-->node的映射关系
    server.cluster->slots[slot] = NULL;
    return C_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. */
// 清除指定节点里的 所有槽位与节点映射关系
int clusterDelNodeSlots(clusterNode *node) {
    int deleted = 0, j;

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeGetSlotBit(node,j)) {
            // 清除指定槽位与节点的映射关系数据
            clusterDelSlot(j);
            deleted++;
        }
    }
    return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a master into slave. */
// 清除所有槽位的迁入迁出信息,主要是在初始化以及主变为备时使用此函数
void clusterCloseAllSlots(void) {
    memset(server.cluster->migrating_slots_to,0,
        sizeof(server.cluster->migrating_slots_to));
    memset(server.cluster->importing_slots_from,0,
        sizeof(server.cluster->importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function
 * -------------------------------------------------------------------------- */

/* The following are defines that are only used in the evaluation function
 * and are based on heuristics. Actually the main point about the rejoin and
 * writable delay is that they should be a few orders of magnitude larger
 * than the network latency. */
#define CLUSTER_MAX_REJOIN_DELAY 5000
#define CLUSTER_MIN_REJOIN_DELAY 500
#define CLUSTER_WRITABLE_DELAY 2000

// 更新集群的state状态
// 1. 周期性的在beforesleep标记为TODO时被调用;
// 2. 本机在故障切换中被选为主节点后,调用本函数;
void clusterUpdateState(void) {
    int j, new_state;
    int reachable_masters = 0; // 非FAIL与非PFAIL的主节点个数
    static mstime_t among_minority_time;//本机处于分裂集群中的小群的最新时间戳
    static mstime_t first_call_time = 0;

    // 本函数开始执行,所以清除相应的todo标记位
    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

    /* If this is a master node, wait some time before turning the state
     * into OK, since it is not a good idea to rejoin the cluster as a writable
     * master, after a reboot, without giving the cluster a chance to
     * reconfigure this node. Note that the delay is calculated starting from
     * the first call to this function and not since the server start, in order
     * to don't count the DB loading time. */
    // 在Redis服务启动后,本机作为主节点需要留有时间供内部各个节点之间相互通信沟通最新的状态
    if (first_call_time == 0) first_call_time = mstime();
    if (nodeIsMaster(myself) &&
        server.cluster->state == CLUSTER_FAIL &&
        mstime() - first_call_time < CLUSTER_WRITABLE_DELAY) return;

    /* Start assuming the state is OK. We'll turn it into FAIL if there
     * are the right conditions. */
    // 先假定正常
    new_state = CLUSTER_OK;

    /* Check if all the slots are covered. */
    // 确认所有的slot槽位是否全部由非FAIL的节点负责处理
    if (server.cluster_require_full_coverage) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            // 确保所有的slot均有节点负责处理,而且对应的节点状态不为FAIL
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->flags & (CLUSTER_NODE_FAIL))
            {
                new_state = CLUSTER_FAIL;
                break;
            }
        }
    }

    /* Compute the cluster size, that is the number of master nodes
     * serving at least a single slot.
     *
     * At the same time count the number of reachable masters having
     * at least one slot. */
    // 更新本机视角下,集群里依旧负责slot处理的主节点个数
    // 并统计这些负责slot处理的状态正常(非fail 且 非pfail)的主节点个数
    {
        dictIterator *di;
        dictEntry *de;
        server.cluster->size = 0;
        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);

            if (nodeIsMaster(node) && node->numslots) {
                server.cluster->size++;
                if ((node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) == 0)
                    reachable_masters++;
            }
        }
        dictReleaseIterator(di);
    }

    /* If we are in a minority partition, change the cluster state
     * to FAIL. */
    // 检查本机是否处于集群分裂后的小群体中,如果处于小群里则将本机视角的集群置为FAIL
    {
        // 集群主节点总数的一半+1作为集群正常的底线
        int needed_quorum = (server.cluster->size / 2) + 1;

        if (reachable_masters < needed_quorum) {
            // 集群正常的主节点个数不足
            new_state = CLUSTER_FAIL;
            among_minority_time = mstime();
        }
    }

    /* Log a state change */
    if (new_state != server.cluster->state) {
        // 本机记录的集群状态有变化
        mstime_t rejoin_delay = server.cluster_node_timeout;

        /* If the instance is a master and was partitioned away with the
         * minority, don't let it accept queries for some time after the
         * partition heals, to make sure there is enough time to receive
         * a configuration update. */
        // 对于之前异常目前已经变为正常的节点,重新变为正常时需要延后指定的时间,以留有足够的时间便于交换更新数据
        if (rejoin_delay > CLUSTER_MAX_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MAX_REJOIN_DELAY;
        if (rejoin_delay < CLUSTER_MIN_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MIN_REJOIN_DELAY;

        // 本机作为主节点,之前处于小群里,现在由失败转为正常时,需要留有足够的时间才能加入集群
        if (new_state == CLUSTER_OK &&
            nodeIsMaster(myself) &&
            mstime() - among_minority_time < rejoin_delay)
        {
            return;
        }

        /* Change the state and log the event. */
        serverLog(LL_WARNING,"Cluster state changed: %s",
            new_state == CLUSTER_OK ? "ok" : "fail");
        server.cluster->state = new_state;
    }
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for, the
 *    following happens:
 *    A) If no other node is in charge according to the current cluster
 *       configuration, we add these slots to our node.
 *    B) If according to our config other nodes are already in charge for
 *       this lots, we set the slots as IMPORTING from our point of view
 *       in order to justify we have those slots, and in order to make
 *       redis-trib aware of the issue, so that it can try to fix it.
 * 2) If we find data in a DB different than DB0 we return C_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns C_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, C_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. */
// 服务启动后,基于库里的数据校验集群配置数据是否正确,如果有不一致则尽可能基于数据来生成新的配置文件
int verifyClusterConfigWithData(void) {
    int j;
    int update_config = 0;

    /* Return ASAP if a module disabled cluster redirections. In that case
     * every master can store keys about every possible hash slot. */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return C_OK;

    /* If this node is a slave, don't perform the check at all as we
     * completely depend on the replication stream. */
    // 只有主节点才需要校验,备节点无需校验
    if (nodeIsSlave(myself)) return C_OK;

    /* Make sure we only have keys in DB0. */
    // 确保cluster模式下只有db0有数据
    for (j = 1; j < server.dbnum; j++) {
        if (dictSize(server.db[j].dict)) return C_ERR;
    }

    /* Check that all the slots we see populated memory have a corresponding
     * entry in the cluster table. Otherwise fix the table. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (!countKeysInSlot(j)) continue; /* No keys in this slot. */
        /* Check if we are assigned to this slot or if we are importing it.
         * In both cases check the next slot as the configuration makes
         * sense. */
        // 一般而言, 节点里记录的数据均应该是由本节点负责的槽位,或者处于从外部节点导入本节点过程中
        if (server.cluster->slots[j] == myself ||
            server.cluster->importing_slots_from[j] != NULL) continue;

        /* If we are here data and cluster config don't agree, and we have
         * slot 'j' populated even if we are not importing it, nor we are
         * assigned to this slot. Fix this condition. */
        // 说明数据跟配置文件不相符
        update_config++;
        /* Case A: slot is unassigned. Take responsibility for it. */
        if (server.cluster->slots[j] == NULL) {
            // 此slot无节点负责,则由本节点来处理
            serverLog(LL_WARNING, "I have keys for unassigned slot %d. "
                                    "Taking responsibility for it.",j);
            clusterAddSlot(myself,j);
        } else {
            serverLog(LL_WARNING, "I have keys for slot %d, but the slot is "
                                    "assigned to another node. "
                                    "Setting it to importing state.",j);
            // 有槽位目前由其他节点负责,但是数据存在本节点中,所以准备将其迁入本节点
            server.cluster->importing_slots_from[j] = server.cluster->slots[j];
        }
    }
    if (update_config) clusterSaveConfigOrDie(1);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * SLAVE nodes handling
 * -------------------------------------------------------------------------- */

/* Set the specified node 'n' as master for this node.
 * If this node is currently a master, it is turned into a slave. */
// 将指定节点n作为本节点的主节点
void clusterSetMaster(clusterNode *n) {
    serverAssert(n != myself);
    serverAssert(myself->numslots == 0);

    if (nodeIsMaster(myself)) {
        // 本机目前是主,则降为备节点
        myself->flags &= ~(CLUSTER_NODE_MASTER|CLUSTER_NODE_MIGRATE_TO);
        myself->flags |= CLUSTER_NODE_SLAVE;
        // 清除所有迁入迁出槽位信息
        clusterCloseAllSlots();
    } else {
        // 本机是个备节点
        if (myself->slaveof)
            // 将本机从目前的主节点中移除
            clusterNodeRemoveSlave(myself->slaveof,myself);
    }
    // 设置节点n为当前节点的主节点
    myself->slaveof = n;
    // 将本节点加入到主节点的备节点数组里
    clusterNodeAddSlave(n,myself);
    // 设置本节点的复制源地址
    replicationSetMaster(n->ip, n->port);
    resetManualFailover();
}

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions.
 * -------------------------------------------------------------------------- */

struct redisNodeFlags {
    uint16_t flag;
    char *name;
};

static struct redisNodeFlags redisNodeFlagsTable[] = {
    {CLUSTER_NODE_MYSELF,       "myself,"},
    {CLUSTER_NODE_MASTER,       "master,"},
    {CLUSTER_NODE_SLAVE,        "slave,"},
    {CLUSTER_NODE_PFAIL,        "fail?,"},
    {CLUSTER_NODE_FAIL,         "fail,"},
    {CLUSTER_NODE_HANDSHAKE,    "handshake,"},
    {CLUSTER_NODE_NOADDR,       "noaddr,"},
    {CLUSTER_NODE_NOFAILOVER,   "nofailover,"}
};

/* Concatenate the comma separated list of node flags to the given SDS
 * string 'ci'. */
// 根据入参flags标记拼接出的可读性的字符串数据, 中间以逗号分割
sds representClusterNodeFlags(sds ci, uint16_t flags) {
    size_t orig_len = sdslen(ci);
    int i, size = sizeof(redisNodeFlagsTable)/sizeof(struct redisNodeFlags);
    // 逐个遍历数组里的各个元素,跟入参flags进行比较
    for (i = 0; i < size; i++) {
        struct redisNodeFlags *nodeflag = redisNodeFlagsTable + i;
        if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
    }
    /* If no flag was added, add the "noflags" special flag. */
    if (sdslen(ci) == orig_len) ci = sdscat(ci,"noflags,");
    // 删除掉最末尾的逗号
    sdsIncrLen(ci,-1); /* Remove trailing comma. */
    return ci;
}

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * The function returns the string representation as an SDS string. */
// 根据指定node节点信息生成csv样式的配置数据,分隔符是空格
sds clusterGenNodeDescription(clusterNode *node) {
    int j, start;
    sds ci;

    /* Node coordinates */
    // %.40s 表示最多40个字符
    ci = sdscatprintf(sdsempty(),"%.40s %s:%d@%d ",
        node->name,
        node->ip,
        node->port,
        node->cport);

    /* Flags */
    ci = representClusterNodeFlags(ci, node->flags);

    /* Slave of... or just "-" */
    if (node->slaveof)
        // 当前节点是备节点,拼接其对应的主节点名称
        // For s: this is the maximum number of characters to be printed. 
        // By default all characters are printed until the ending null character is encountered.
        ci = sdscatprintf(ci," %.40s ",node->slaveof->name);
    else
        // 当前节点为主节点
        ci = sdscatlen(ci," - ",3);

    /* Latency from the POV of this node, config epoch, link status */
    ci = sdscatprintf(ci,"%lld %lld %llu %s",
        (long long) node->ping_sent,
        (long long) node->pong_received,
        (unsigned long long) node->configEpoch,
        (node->link || node->flags & CLUSTER_NODE_MYSELF) ?
                    "connected" : "disconnected");

    /* Slots served by this instance */
    start = -1;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        int bit;

        if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
            if (start == -1) start = j;
        }
        if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
            if (bit && j == CLUSTER_SLOTS-1) j++;

            if (start == j-1) {
                ci = sdscatprintf(ci," %d",start);
            } else {
                ci = sdscatprintf(ci," %d-%d",start,j-1);
            }
            start = -1;
        }
    }

    /* Just for MYSELF node we also dump info about slots that
     * we are migrating to other instances or importing from other
     * instances. */
    if (node->flags & CLUSTER_NODE_MYSELF) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->migrating_slots_to[j]) {
                ci = sdscatprintf(ci," [%d->-%.40s]",j,
                    server.cluster->migrating_slots_to[j]->name);
            } else if (server.cluster->importing_slots_from[j]) {
                ci = sdscatprintf(ci," [%d-<-%.40s]",j,
                    server.cluster->importing_slots_from[j]->name);
            }
        }
    }
    return ci;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. */
// 依据节点过滤标识,将集群余下的节点生成类似csv样式的配置字符串数据
sds clusterGenNodesDescription(int filter) {
    sds ci = sdsempty(), ni;
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node->flags & filter) continue;
        ni = clusterGenNodeDescription(node);
        ci = sdscatsds(ci,ni);
        sdsfree(ni);
        ci = sdscatlen(ci,"\n",1);
    }
    dictReleaseIterator(di);
    return ci;
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */

// 交互消息类型,根据type值解析成字符串型数据
const char *clusterGetMessageTypeString(int type) {
    switch(type) {
    case CLUSTERMSG_TYPE_PING: return "ping";
    case CLUSTERMSG_TYPE_PONG: return "pong";
    case CLUSTERMSG_TYPE_MEET: return "meet";
    case CLUSTERMSG_TYPE_FAIL: return "fail";
    case CLUSTERMSG_TYPE_PUBLISH: return "publish";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST: return "auth-req";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK: return "auth-ack";
    case CLUSTERMSG_TYPE_UPDATE: return "update";
    case CLUSTERMSG_TYPE_MFSTART: return "mfstart";
    case CLUSTERMSG_TYPE_MODULE: return "module";
    }
    return "unknown";
}

// 将robj类型的slot数据解析成int型,如果出错,则直接构造错误信息发送给client
int getSlotOrReply(client *c, robj *o) {
    long long slot;

    if (getLongLongFromObject(o,&slot) != C_OK ||
        slot < 0 || slot >= CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

void clusterReplyMultiBulkSlots(client *c) {
    /* Format: 1) 1) start slot
     *            2) end slot
     *            3) 1) master IP
     *               2) master port
     *               3) node ID
     *            4) 1) replica IP
     *               2) replica port
     *               3) node ID
     *           ... continued until done
     */

    int num_masters = 0;
    void *slot_replylen = addDeferredMultiBulkLength(c);

    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int j = 0, start = -1;

        /* Skip slaves (that are iterated when producing the output of their
         * master) and  masters not serving any slot. */
        // 只构造有效槽位的主节点配置
        if (!nodeIsMaster(node) || node->numslots == 0) continue;

        for (j = 0; j < CLUSTER_SLOTS; j++) {
            int bit, i;

            if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
                if (start == -1) start = j;
            }
            if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
                int nested_elements = 3; /* slots (2) + master addr (1). */
                void *nested_replylen = addDeferredMultiBulkLength(c);

                if (bit && j == CLUSTER_SLOTS-1) j++;

                /* If slot exists in output map, add to it's list.
                 * else, create a new output map for this slot */
                if (start == j-1) {
                    addReplyLongLong(c, start); /* only one slot; low==high */
                    addReplyLongLong(c, start);
                } else {
                    addReplyLongLong(c, start); /* low */
                    addReplyLongLong(c, j-1);   /* high */
                }
                start = -1;

                /* First node reply position is always the master */
                addReplyMultiBulkLen(c, 3);
                addReplyBulkCString(c, node->ip);
                addReplyLongLong(c, node->port);
                addReplyBulkCBuffer(c, node->name, CLUSTER_NAMELEN);

                /* Remaining nodes in reply are replicas for slot range */
                for (i = 0; i < node->numslaves; i++) {
                    /* This loop is copy/pasted from clusterGenNodeDescription()
                     * with modifications for per-slot node aggregation */
                    if (nodeFailed(node->slaves[i])) continue;
                    addReplyMultiBulkLen(c, 3);
                    addReplyBulkCString(c, node->slaves[i]->ip);
                    addReplyLongLong(c, node->slaves[i]->port);
                    addReplyBulkCBuffer(c, node->slaves[i]->name, CLUSTER_NAMELEN);
                    nested_elements++;
                }
                setDeferredMultiBulkLength(c, nested_replylen, nested_elements);
                num_masters++;
            }
        }
    }
    dictReleaseIterator(di);
    setDeferredMultiBulkLength(c, slot_replylen, num_masters);
}

// cluster命令总入口
void clusterCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ADDSLOTS <slot> [slot ...] -- Assign slots to current node.",
"BUMPEPOCH -- Advance the cluster config epoch.",
"COUNT-failure-reports <node-id> -- Return number of failure reports for <node-id>.",
"COUNTKEYSINSLOT <slot> - Return the number of keys in <slot>.",
"DELSLOTS <slot> [slot ...] -- Delete slots information from current node.",
"FAILOVER [force|takeover] -- Promote current replica node to being a master.",
"FORGET <node-id> -- Remove a node from the cluster.",
"GETKEYSINSLOT <slot> <count> -- Return key names stored by current node in a slot.",
"FLUSHSLOTS -- Delete current node own slots information.",
"INFO - Return onformation about the cluster.",
"KEYSLOT <key> -- Return the hash slot for <key>.",
"MEET <ip> <port> [bus-port] -- Connect nodes into a working cluster.",
"MYID -- Return the node id.",
"NODES -- Return cluster configuration seen by node. Output format:",
"    <id> <ip:port> <flags> <master> <pings> <pongs> <epoch> <link> <slot> ... <slot>",
"REPLICATE <node-id> -- Configure current node as replica to <node-id>.",
"RESET [hard|soft] -- Reset current node (default: soft).",
"SET-config-epoch <epoch> - Set config epoch of current node.",
"SETSLOT <slot> (importing|migrating|stable|node <node-id>) -- Set slot state.",
"REPLICAS <node-id> -- Return <node-id> replicas.",
"SLOTS -- Return information about slots range mappings. Each range is made of:",
"    start, end, master and replicas IP addresses, ports and ids",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"meet") && (c->argc == 4 || c->argc == 5)) {
        /* CLUSTER MEET <ip> <port> [cport] */
        long long port, cport;

        if (getLongLongFromObject(c->argv[3], &port) != C_OK) {
            addReplyErrorFormat(c,"Invalid TCP base port specified: %s",
                                (char*)c->argv[3]->ptr);
            return;
        }

        if (c->argc == 5) {
            if (getLongLongFromObject(c->argv[4], &cport) != C_OK) {
                addReplyErrorFormat(c,"Invalid TCP bus port specified: %s",
                                    (char*)c->argv[4]->ptr);
                return;
            }
        } else {
            cport = port + CLUSTER_PORT_INCR;
        }

        if (clusterStartHandshake(c->argv[2]->ptr,port,cport) == 0 &&
            errno == EINVAL)
        {
            addReplyErrorFormat(c,"Invalid node address specified: %s:%s",
                            (char*)c->argv[2]->ptr, (char*)c->argv[3]->ptr);
        } else {
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"nodes") && c->argc == 2) {
        /* CLUSTER NODES */
        robj *o;
        sds ci = clusterGenNodesDescription(0);

        o = createObject(OBJ_STRING,ci);
        addReplyBulk(c,o);
        decrRefCount(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"myid") && c->argc == 2) {
        /* CLUSTER MYID */
        addReplyBulkCBuffer(c,myself->name, CLUSTER_NAMELEN);
    } else if (!strcasecmp(c->argv[1]->ptr,"slots") && c->argc == 2) {
        /* CLUSTER SLOTS */
        clusterReplyMultiBulkSlots(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"flushslots") && c->argc == 2) {
        /* CLUSTER FLUSHSLOTS */
        if (dictSize(server.db[0].dict) != 0) {
            addReplyError(c,"DB must be empty to perform CLUSTER FLUSHSLOTS.");
            return;
        }
        clusterDelNodeSlots(myself);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr,"addslots") ||
               !strcasecmp(c->argv[1]->ptr,"delslots")) && c->argc >= 3)
    {
        /* CLUSTER ADDSLOTS <slot> [slot] ... */
        /* CLUSTER DELSLOTS <slot> [slot] ... */
        int j, slot;
        unsigned char *slots = zmalloc(CLUSTER_SLOTS);
        int del = !strcasecmp(c->argv[1]->ptr,"delslots");

        memset(slots,0,CLUSTER_SLOTS);
        /* Check that all the arguments are parseable and that all the
         * slots are not already busy. */
        for (j = 2; j < c->argc; j++) {
            if ((slot = getSlotOrReply(c,c->argv[j])) == -1) {
                zfree(slots);
                return;
            }
            if (del && server.cluster->slots[slot] == NULL) {
                addReplyErrorFormat(c,"Slot %d is already unassigned", slot);
                zfree(slots);
                return;
            } else if (!del && server.cluster->slots[slot]) {
                addReplyErrorFormat(c,"Slot %d is already busy", slot);
                zfree(slots);
                return;
            }
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",
                    (int)slot);
                zfree(slots);
                return;
            }
        }
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (slots[j]) {
                int retval;

                /* If this slot was set as importing we can clear this
                 * state as now we are the real owner of the slot. */
                if (server.cluster->importing_slots_from[j])
                    server.cluster->importing_slots_from[j] = NULL;

                retval = del ? clusterDelSlot(j) :
                               clusterAddSlot(myself,j);
                serverAssertWithInfo(c,NULL,retval == C_OK);
            }
        }
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"setslot") && c->argc >= 4) {
        /* SETSLOT 10 MIGRATING <node ID> */
        /* SETSLOT 10 IMPORTING <node ID> */
        /* SETSLOT 10 STABLE */
        /* SETSLOT 10 NODE <node ID> */
        int slot;
        clusterNode *n;

        if (nodeIsSlave(myself)) {
            addReplyError(c,"Please use SETSLOT only with masters.");
            return;
        }

        if ((slot = getSlotOrReply(c,c->argv[2])) == -1) return;

        if (!strcasecmp(c->argv[3]->ptr,"migrating") && c->argc == 5) {
            if (server.cluster->slots[slot] != myself) {
                addReplyErrorFormat(c,"I'm not the owner of hash slot %u",slot);
                return;
            }
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            server.cluster->migrating_slots_to[slot] = n;
        } else if (!strcasecmp(c->argv[3]->ptr,"importing") && c->argc == 5) {
            if (server.cluster->slots[slot] == myself) {
                addReplyErrorFormat(c,
                    "I'm already the owner of hash slot %u",slot);
                return;
            }
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            server.cluster->importing_slots_from[slot] = n;
        } else if (!strcasecmp(c->argv[3]->ptr,"stable") && c->argc == 4) {
            /* CLUSTER SETSLOT <SLOT> STABLE */
            server.cluster->importing_slots_from[slot] = NULL;
            server.cluster->migrating_slots_to[slot] = NULL;
        } else if (!strcasecmp(c->argv[3]->ptr,"node") && c->argc == 5) {
            /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
            clusterNode *n = clusterLookupNode(c->argv[4]->ptr);

            if (!n) {
                addReplyErrorFormat(c,"Unknown node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            /* If this hash slot was served by 'myself' before to switch
             * make sure there are no longer local keys for this hash slot. */
            if (server.cluster->slots[slot] == myself && n != myself) {
                if (countKeysInSlot(slot) != 0) {
                    addReplyErrorFormat(c,
                        "Can't assign hashslot %d to a different node "
                        "while I still hold keys for this hash slot.", slot);
                    return;
                }
            }
            /* If this slot is in migrating status but we have no keys
             * for it assigning the slot to another node will clear
             * the migratig status. */
            if (countKeysInSlot(slot) == 0 &&
                server.cluster->migrating_slots_to[slot])
                server.cluster->migrating_slots_to[slot] = NULL;

            /* If this node was importing this slot, assigning the slot to
             * itself also clears the importing status. */
            if (n == myself &&
                server.cluster->importing_slots_from[slot])
            {
                /* This slot was manually migrated, set this node configEpoch
                 * to a new epoch so that the new version can be propagated
                 * by the cluster.
                 *
                 * Note that if this ever results in a collision with another
                 * node getting the same configEpoch, for example because a
                 * failover happens at the same time we close the slot, the
                 * configEpoch collision resolution will fix it assigning
                 * a different epoch to each node. */
                if (clusterBumpConfigEpochWithoutConsensus() == C_OK) {
                    serverLog(LL_WARNING,
                        "configEpoch updated after importing slot %d", slot);
                }
                server.cluster->importing_slots_from[slot] = NULL;
            }
            clusterDelSlot(slot);
            clusterAddSlot(n,slot);
        } else {
            addReplyError(c,
                "Invalid CLUSTER SETSLOT action or number of arguments. Try CLUSTER HELP");
            return;
        }
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_UPDATE_STATE);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"bumpepoch") && c->argc == 2) {
        /* CLUSTER BUMPEPOCH */
        int retval = clusterBumpConfigEpochWithoutConsensus();
        sds reply = sdscatprintf(sdsempty(),"+%s %llu\r\n",
                (retval == C_OK) ? "BUMPED" : "STILL",
                (unsigned long long) myself->configEpoch);
        addReplySds(c,reply);
    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLUSTER INFO */
        char *statestr[] = {"ok","fail","needhelp"};
        int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
        uint64_t myepoch;
        int j;

        for (j = 0; j < CLUSTER_SLOTS; j++) {
            clusterNode *n = server.cluster->slots[j];

            if (n == NULL) continue;
            slots_assigned++;
            if (nodeFailed(n)) {
                slots_fail++;
            } else if (nodeTimedOut(n)) {
                slots_pfail++;
            } else {
                slots_ok++;
            }
        }

        myepoch = (nodeIsSlave(myself) && myself->slaveof) ?
                  myself->slaveof->configEpoch : myself->configEpoch;

        sds info = sdscatprintf(sdsempty(),
            "cluster_state:%s\r\n"
            "cluster_slots_assigned:%d\r\n"
            "cluster_slots_ok:%d\r\n"
            "cluster_slots_pfail:%d\r\n"
            "cluster_slots_fail:%d\r\n"
            "cluster_known_nodes:%lu\r\n"
            "cluster_size:%d\r\n"
            "cluster_current_epoch:%llu\r\n"
            "cluster_my_epoch:%llu\r\n"
            , statestr[server.cluster->state],
            slots_assigned,
            slots_ok,
            slots_pfail,
            slots_fail,
            dictSize(server.cluster->nodes),
            server.cluster->size,
            (unsigned long long) server.cluster->currentEpoch,
            (unsigned long long) myepoch
        );

        /* Show stats about messages sent and received. */
        long long tot_msg_sent = 0;
        long long tot_msg_received = 0;

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->stats_bus_messages_sent[i] == 0) continue;
            tot_msg_sent += server.cluster->stats_bus_messages_sent[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_sent:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->stats_bus_messages_sent[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_sent:%lld\r\n", tot_msg_sent);

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->stats_bus_messages_received[i] == 0) continue;
            tot_msg_received += server.cluster->stats_bus_messages_received[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_received:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->stats_bus_messages_received[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_received:%lld\r\n", tot_msg_received);

        /* Produce the reply protocol. */
        addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
            (unsigned long)sdslen(info)));
        addReplySds(c,info);
        addReply(c,shared.crlf);
    } else if (!strcasecmp(c->argv[1]->ptr,"saveconfig") && c->argc == 2) {
        int retval = clusterSaveConfig(1);

        if (retval == 0)
            addReply(c,shared.ok);
        else
            addReplyErrorFormat(c,"error saving the cluster node config: %s",
                strerror(errno));
    } else if (!strcasecmp(c->argv[1]->ptr,"keyslot") && c->argc == 3) {
        /* CLUSTER KEYSLOT <key> */
        sds key = c->argv[2]->ptr;

        addReplyLongLong(c,keyHashSlot(key,sdslen(key)));
    } else if (!strcasecmp(c->argv[1]->ptr,"countkeysinslot") && c->argc == 3) {
        /* CLUSTER COUNTKEYSINSLOT <slot> */
        long long slot;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS) {
            addReplyError(c,"Invalid slot");
            return;
        }
        addReplyLongLong(c,countKeysInSlot(slot));
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeysinslot") && c->argc == 4) {
        /* CLUSTER GETKEYSINSLOT <slot> <count> */
        long long maxkeys, slot;
        unsigned int numkeys, j;
        robj **keys;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (getLongLongFromObjectOrReply(c,c->argv[3],&maxkeys,NULL)
            != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS || maxkeys < 0) {
            addReplyError(c,"Invalid slot or number of keys");
            return;
        }

        /* Avoid allocating more than needed in case of large COUNT argument
         * and smaller actual number of keys. */
        unsigned int keys_in_slot = countKeysInSlot(slot);
        if (maxkeys > keys_in_slot) maxkeys = keys_in_slot;

        keys = zmalloc(sizeof(robj*)*maxkeys);
        numkeys = getKeysInSlot(slot, keys, maxkeys);
        addReplyMultiBulkLen(c,numkeys);
        for (j = 0; j < numkeys; j++) {
            addReplyBulk(c,keys[j]);
            decrRefCount(keys[j]);
        }
        zfree(keys);
    } else if (!strcasecmp(c->argv[1]->ptr,"forget") && c->argc == 3) {
        /* CLUSTER FORGET <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else if (n == myself) {
            addReplyError(c,"I tried hard but I can't forget myself...");
            return;
        } else if (nodeIsSlave(myself) && myself->slaveof == n) {
            addReplyError(c,"Can't forget my master!");
            return;
        }
        clusterBlacklistAddNode(n);
        clusterDelNode(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"replicate") && c->argc == 3) {
        /* CLUSTER REPLICATE <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        /* Lookup the specified node in our table. */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        /* I can't replicate myself. */
        if (n == myself) {
            addReplyError(c,"Can't replicate myself");
            return;
        }

        /* Can't replicate a slave. */
        if (nodeIsSlave(n)) {
            addReplyError(c,"I can only replicate a master, not a replica.");
            return;
        }

        /* If the instance is currently a master, it should have no assigned
         * slots nor keys to accept to replicate some other node.
         * Slaves can switch to another master without issues. */
        if (nodeIsMaster(myself) &&
            (myself->numslots != 0 || dictSize(server.db[0].dict) != 0)) {
            addReplyError(c,
                "To set a master the node must be empty and "
                "without assigned slots.");
            return;
        }

        /* Set the master. */
        clusterSetMaster(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr,"slaves") ||
                !strcasecmp(c->argv[1]->ptr,"replicas")) && c->argc == 3) {
        /* CLUSTER SLAVES <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
        int j;

        /* Lookup the specified node in our table. */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        if (nodeIsSlave(n)) {
            addReplyError(c,"The specified node is not a master");
            return;
        }

        addReplyMultiBulkLen(c,n->numslaves);
        for (j = 0; j < n->numslaves; j++) {
            sds ni = clusterGenNodeDescription(n->slaves[j]);
            addReplyBulkCString(c,ni);
            sdsfree(ni);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"count-failure-reports") &&
               c->argc == 3)
    {
        /* CLUSTER COUNT-FAILURE-REPORTS <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else {
            addReplyLongLong(c,clusterNodeFailureReportsCount(n));
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"failover") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER FAILOVER [FORCE|TAKEOVER] */
        int force = 0, takeover = 0;

        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"force")) {
                force = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"takeover")) {
                takeover = 1;
                force = 1; /* Takeover also implies force. */
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Check preconditions. */
        if (nodeIsMaster(myself)) {
            addReplyError(c,"You should send CLUSTER FAILOVER to a replica");
            return;
        } else if (myself->slaveof == NULL) {
            addReplyError(c,"I'm a replica but my master is unknown to me");
            return;
        } else if (!force &&
                   (nodeFailed(myself->slaveof) ||
                    myself->slaveof->link == NULL))
        {
            addReplyError(c,"Master is down or failed, "
                            "please use CLUSTER FAILOVER FORCE");
            return;
        }
        resetManualFailover();
        server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;

        if (takeover) {
            /* A takeover does not perform any initial check. It just
             * generates a new configuration epoch for this node without
             * consensus, claims the master's slots, and broadcast the new
             * configuration. */
            serverLog(LL_WARNING,"Taking over the master (user request).");
            clusterBumpConfigEpochWithoutConsensus();
            clusterFailoverReplaceYourMaster();
        } else if (force) {
            /* If this is a forced failover, we don't need to talk with our
             * master to agree about the offset. We just failover taking over
             * it without coordination. */
            serverLog(LL_WARNING,"Forced failover user request accepted.");
            server.cluster->mf_can_start = 1;
        } else {
            serverLog(LL_WARNING,"Manual failover user request accepted.");
            clusterSendMFStart(myself->slaveof);
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"set-config-epoch") && c->argc == 3)
    {
        /* CLUSTER SET-CONFIG-EPOCH <epoch>
         *
         * The user is allowed to set the config epoch only when a node is
         * totally fresh: no config epoch, no other known node, and so forth.
         * This happens at cluster creation time to start with a cluster where
         * every node has a different node ID, without to rely on the conflicts
         * resolution system which is too slow when a big cluster is created. */
        long long epoch;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&epoch,NULL) != C_OK)
            return;

        if (epoch < 0) {
            addReplyErrorFormat(c,"Invalid config epoch specified: %lld",epoch);
        } else if (dictSize(server.cluster->nodes) > 1) {
            addReplyError(c,"The user can assign a config epoch only when the "
                            "node does not know any other node.");
        } else if (myself->configEpoch != 0) {
            addReplyError(c,"Node config epoch is already non-zero");
        } else {
            myself->configEpoch = epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
                (unsigned long long) myself->configEpoch);

            if (server.cluster->currentEpoch < (uint64_t)epoch)
                server.cluster->currentEpoch = epoch;
            /* No need to fsync the config here since in the unlucky event
             * of a failure to persist the config, the conflict resolution code
             * will assign an unique config to this node. */
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                                 CLUSTER_TODO_SAVE_CONFIG);
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"reset") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER RESET [SOFT|HARD] */
        int hard = 0;

        /* Parse soft/hard argument. Default is soft. */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"hard")) {
                hard = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"soft")) {
                hard = 0;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Slaves can be reset while containing data, but not master nodes
         * that must be empty. */
        if (nodeIsMaster(myself) && dictSize(c->db->dict) != 0) {
            addReplyError(c,"CLUSTER RESET can't be called with "
                            "master nodes containing keys");
            return;
        }
        clusterReset(hard);
        addReply(c,shared.ok);
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

/* Generates a DUMP-format representation of the object 'o', adding it to the
 * io stream pointed by 'rio'. This function can't fail. */
void createDumpPayload(rio *payload, robj *o, robj *key) {
    unsigned char buf[2];
    uint64_t crc;

    /* Serialize the object in a RDB-like format. It consist of an object type
     * byte followed by the serialized object. This is understood by RESTORE. */
    rioInitWithBuffer(payload,sdsempty());
    serverAssert(rdbSaveObjectType(payload,o));
    serverAssert(rdbSaveObject(payload,o,key));

    /* Write the footer, this is how it looks like:
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     */

    /* RDB version */
    buf[0] = RDB_VERSION & 0xff;
    buf[1] = (RDB_VERSION >> 8) & 0xff;
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,buf,2);

    /* CRC64 */
    crc = crc64(0,(unsigned char*)payload->io.buffer.ptr,
                sdslen(payload->io.buffer.ptr));
    memrev64ifbe(&crc);
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,&crc,8);
}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 * If the DUMP payload looks valid C_OK is returned, otherwise C_ERR
 * is returned. */
int verifyDumpPayload(unsigned char *p, size_t len) {
    unsigned char *footer;
    uint16_t rdbver;
    uint64_t crc;

    /* At least 2 bytes of RDB version and 8 of CRC64 should be present. */
    if (len < 10) return C_ERR;
    footer = p+(len-10);

    /* Verify RDB version */
    rdbver = (footer[1] << 8) | footer[0];
    if (rdbver > RDB_VERSION) return C_ERR;

    /* Verify CRC64 */
    crc = crc64(0,p,len-8);
    memrev64ifbe(&crc);
    return (memcmp(&crc,footer+2,8) == 0) ? C_OK : C_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
void dumpCommand(client *c) {
    robj *o, *dumpobj;
    rio payload;

    /* Check if the key is here. */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    /* Create the DUMP encoded representation. */
    createDumpPayload(&payload,o,c->argv[1]);

    /* Transfer to the client */
    dumpobj = createObject(OBJ_STRING,payload.io.buffer.ptr);
    addReplyBulk(c,dumpobj);
    decrRefCount(dumpobj);
    return;
}

/* RESTORE key ttl serialized-value [REPLACE] */
void restoreCommand(client *c) {
    long long ttl, lfu_freq = -1, lru_idle = -1, lru_clock = -1;
    rio payload;
    int j, type, replace = 0, absttl = 0;
    robj *obj;

    /* Parse additional options */
    for (j = 4; j < c->argc; j++) {
        int additional = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"absttl")) {
            absttl = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"idletime") && additional >= 1 &&
                   lfu_freq == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lru_idle,NULL)
                    != C_OK) return;
            if (lru_idle < 0) {
                addReplyError(c,"Invalid IDLETIME value, must be >= 0");
                return;
            }
            lru_clock = LRU_CLOCK();
            j++; /* Consume additional arg. */
        } else if (!strcasecmp(c->argv[j]->ptr,"freq") && additional >= 1 &&
                   lru_idle == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lfu_freq,NULL)
                    != C_OK) return;
            if (lfu_freq < 0 || lfu_freq > 255) {
                addReplyError(c,"Invalid FREQ value, must be >= 0 and <= 255");
                return;
            }
            j++; /* Consume additional arg. */
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Make sure this key does not already exist here... */
    if (!replace && lookupKeyWrite(c->db,c->argv[1]) != NULL) {
        addReply(c,shared.busykeyerr);
        return;
    }

    /* Check if the TTL value makes sense */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != C_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* Verify RDB version and data checksum. */
    if (verifyDumpPayload(c->argv[3]->ptr,sdslen(c->argv[3]->ptr)) == C_ERR)
    {
        addReplyError(c,"DUMP payload version or checksum are wrong");
        return;
    }

    rioInitWithBuffer(&payload,c->argv[3]->ptr);
    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((obj = rdbLoadObject(type,&payload,c->argv[1])) == NULL))
    {
        addReplyError(c,"Bad data format");
        return;
    }

    /* Remove the old key if needed. */
    if (replace) dbDelete(c->db,c->argv[1]);

    /* Create the key and set the TTL if any */
    dbAdd(c->db,c->argv[1],obj);
    if (ttl) {
        if (!absttl) ttl+=mstime();
        setExpire(c,c->db,c->argv[1],ttl);
    }
    objectSetLRUOrLFU(obj,lfu_freq,lru_idle,lru_clock);
    signalModifiedKey(c->db,c->argv[1]);
    addReply(c,shared.ok);
    server.dirty++;
}

/* MIGRATE socket cache implementation.
 *
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. */
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. */
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached sockets after 10 sec. */

typedef struct migrateCachedSocket {
    int fd;
    long last_dbid;
    time_t last_use_time;
} migrateCachedSocket;

/* Return a migrateCachedSocket containing a TCP socket connected with the
 * target instance, possibly returning a cached one.
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be created from scratch
 * the next time. */
migrateCachedSocket* migrateGetSocket(client *c, robj *host, robj *port, long timeout) {
    int fd;
    sds name = sdsempty();
    migrateCachedSocket *cs;

    /* Check if we have an already cached socket for this ip:port pair. */
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (cs) {
        sdsfree(name);
        cs->last_use_time = server.unixtime;
        return cs;
    }

    /* No cached socket, create one. */
    if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
        /* Too many items, drop one at random. */
        dictEntry *de = dictGetRandomKey(server.migrate_cached_sockets);
        cs = dictGetVal(de);
        close(cs->fd);
        zfree(cs);
        dictDelete(server.migrate_cached_sockets,dictGetKey(de));
    }

    /* Create the socket */
    fd = anetTcpNonBlockConnect(server.neterr,c->argv[1]->ptr,
                                atoi(c->argv[2]->ptr));
    if (fd == -1) {
        sdsfree(name);
        addReplyErrorFormat(c,"Can't connect to target node: %s",
            server.neterr);
        return NULL;
    }
    anetEnableTcpNoDelay(server.neterr,fd);

    /* Check if it connects within the specified timeout. */
    if ((aeWait(fd,AE_WRITABLE,timeout) & AE_WRITABLE) == 0) {
        sdsfree(name);
        addReplySds(c,
            sdsnew("-IOERR error or timeout connecting to the client\r\n"));
        close(fd);
        return NULL;
    }

    /* Add to the cache and return it to the caller. */
    cs = zmalloc(sizeof(*cs));
    cs->fd = fd;
    cs->last_dbid = -1;
    cs->last_use_time = server.unixtime;
    dictAdd(server.migrate_cached_sockets,name,cs);
    return cs;
}

/* Free a migrate cached connection. */
void migrateCloseSocket(robj *host, robj *port) {
    sds name = sdsempty();
    migrateCachedSocket *cs;

    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (!cs) {
        sdsfree(name);
        return;
    }

    close(cs->fd);
    zfree(cs);
    dictDelete(server.migrate_cached_sockets,name);
    sdsfree(name);
}

void migrateCloseTimedoutSockets(void) {
    dictIterator *di = dictGetSafeIterator(server.migrate_cached_sockets);
    dictEntry *de;

    while((de = dictNext(di)) != NULL) {
        migrateCachedSocket *cs = dictGetVal(de);

        if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
            close(cs->fd);
            zfree(cs);
            dictDelete(server.migrate_cached_sockets,dictGetKey(de));
        }
    }
    dictReleaseIterator(di);
}

/* MIGRATE host port key dbid timeout [COPY | REPLACE | AUTH password]
 *
 * On in the multiple keys form:
 *
 * MIGRATE host port "" dbid timeout [COPY | REPLACE | AUTH password] KEYS key1
 * key2 ... keyN */
void migrateCommand(client *c) {
    migrateCachedSocket *cs;
    int copy = 0, replace = 0, j;
    char *password = NULL;
    long timeout;
    long dbid;
    robj **ov = NULL; /* Objects to migrate. */
    robj **kv = NULL; /* Key names. */
    robj **newargv = NULL; /* Used to rewrite the command as DEL ... keys ... */
    rio cmd, payload;
    int may_retry = 1;
    int write_error = 0;
    int argv_rewritten = 0;

    /* To support the KEYS option we need the following additional state. */
    int first_key = 3; /* Argument index of the first key. */
    int num_keys = 1;  /* By default only migrate the 'key' argument. */

    /* Parse additional options */
    for (j = 6; j < c->argc; j++) {
        int moreargs = j < c->argc-1;
        if (!strcasecmp(c->argv[j]->ptr,"copy")) {
            copy = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"auth")) {
            if (!moreargs) {
                addReply(c,shared.syntaxerr);
                return;
            }
            j++;
            password = c->argv[j]->ptr;
        } else if (!strcasecmp(c->argv[j]->ptr,"keys")) {
            if (sdslen(c->argv[3]->ptr) != 0) {
                addReplyError(c,
                    "When using MIGRATE KEYS option, the key argument"
                    " must be set to the empty string");
                return;
            }
            first_key = j+1;
            num_keys = c->argc - j - 1;
            break; /* All the remaining args are keys. */
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Sanity check */
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != C_OK ||
        getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != C_OK)
    {
        return;
    }
    if (timeout <= 0) timeout = 1000;

    /* Check if the keys are here. If at least one key is to migrate, do it
     * otherwise if all the keys are missing reply with "NOKEY" to signal
     * the caller there was nothing to migrate. We don't return an error in
     * this case, since often this is due to a normal condition like the key
     * expiring in the meantime. */
    ov = zrealloc(ov,sizeof(robj*)*num_keys);
    kv = zrealloc(kv,sizeof(robj*)*num_keys);
    int oi = 0;

    for (j = 0; j < num_keys; j++) {
        if ((ov[oi] = lookupKeyRead(c->db,c->argv[first_key+j])) != NULL) {
            kv[oi] = c->argv[first_key+j];
            oi++;
        }
    }
    num_keys = oi;
    if (num_keys == 0) {
        zfree(ov); zfree(kv);
        addReplySds(c,sdsnew("+NOKEY\r\n"));
        return;
    }

try_again:
    write_error = 0;

    /* Connect */
    cs = migrateGetSocket(c,c->argv[1],c->argv[2],timeout);
    if (cs == NULL) {
        zfree(ov); zfree(kv);
        return; /* error sent to the client by migrateGetSocket() */
    }

    rioInitWithBuffer(&cmd,sdsempty());

    /* Authentication */
    if (password) {
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',2));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"AUTH",4));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,password,
            sdslen(password)));
    }

    /* Send the SELECT command if the current DB is not already selected. */
    int select = cs->last_dbid != dbid; /* Should we emit SELECT? */
    if (select) {
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',2));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"SELECT",6));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,dbid));
    }

    int non_expired = 0; /* Number of keys that we'll find non expired.
                            Note that serializing large keys may take some time
                            so certain keys that were found non expired by the
                            lookupKey() function, may be expired later. */

    /* Create RESTORE payload and generate the protocol to call the command. */
    for (j = 0; j < num_keys; j++) {
        long long ttl = 0;
        long long expireat = getExpire(c->db,kv[j]);

        if (expireat != -1) {
            ttl = expireat-mstime();
            if (ttl < 0) {
                continue;
            }
            if (ttl < 1) ttl = 1;
        }

        /* Relocate valid (non expired) keys into the array in successive
         * positions to remove holes created by the keys that were present
         * in the first lookup but are now expired after the second lookup. */
        kv[non_expired++] = kv[j];

        serverAssertWithInfo(c,NULL,
            rioWriteBulkCount(&cmd,'*',replace ? 5 : 4));

        if (server.cluster_enabled)
            serverAssertWithInfo(c,NULL,
                rioWriteBulkString(&cmd,"RESTORE-ASKING",14));
        else
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"RESTORE",7));
        serverAssertWithInfo(c,NULL,sdsEncodedObject(kv[j]));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,kv[j]->ptr,
                sdslen(kv[j]->ptr)));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,ttl));

        /* Emit the payload argument, that is the serialized object using
         * the DUMP format. */
        createDumpPayload(&payload,ov[j],kv[j]);
        serverAssertWithInfo(c,NULL,
            rioWriteBulkString(&cmd,payload.io.buffer.ptr,
                               sdslen(payload.io.buffer.ptr)));
        sdsfree(payload.io.buffer.ptr);

        /* Add the REPLACE option to the RESTORE command if it was specified
         * as a MIGRATE option. */
        if (replace)
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"REPLACE",7));
    }

    /* Fix the actual number of keys we are migrating. */
    num_keys = non_expired;

    /* Transfer the query to the other node in 64K chunks. */
    errno = 0;
    {
        sds buf = cmd.io.buffer.ptr;
        size_t pos = 0, towrite;
        int nwritten = 0;

        while ((towrite = sdslen(buf)-pos) > 0) {
            towrite = (towrite > (64*1024) ? (64*1024) : towrite);
            nwritten = syncWrite(cs->fd,buf+pos,towrite,timeout);
            if (nwritten != (signed)towrite) {
                write_error = 1;
                goto socket_err;
            }
            pos += nwritten;
        }
    }

    char buf0[1024]; /* Auth reply. */
    char buf1[1024]; /* Select reply. */
    char buf2[1024]; /* Restore reply. */

    /* Read the AUTH reply if needed. */
    if (password && syncReadLine(cs->fd, buf0, sizeof(buf0), timeout) <= 0)
        goto socket_err;

    /* Read the SELECT reply if needed. */
    if (select && syncReadLine(cs->fd, buf1, sizeof(buf1), timeout) <= 0)
        goto socket_err;

    /* Read the RESTORE replies. */
    int error_from_target = 0;
    int socket_error = 0;
    int del_idx = 1; /* Index of the key argument for the replicated DEL op. */

    /* Allocate the new argument vector that will replace the current command,
     * to propagate the MIGRATE as a DEL command (if no COPY option was given).
     * We allocate num_keys+1 because the additional argument is for "DEL"
     * command name itself. */
    if (!copy) newargv = zmalloc(sizeof(robj*)*(num_keys+1));

    for (j = 0; j < num_keys; j++) {
        if (syncReadLine(cs->fd, buf2, sizeof(buf2), timeout) <= 0) {
            socket_error = 1;
            break;
        }
        if ((password && buf0[0] == '-') ||
            (select && buf1[0] == '-') ||
            buf2[0] == '-')
        {
            /* On error assume that last_dbid is no longer valid. */
            if (!error_from_target) {
                cs->last_dbid = -1;
                char *errbuf;
                if (password && buf0[0] == '-') errbuf = buf0;
                else if (select && buf1[0] == '-') errbuf = buf1;
                else errbuf = buf2;

                error_from_target = 1;
                addReplyErrorFormat(c,"Target instance replied with error: %s",
                    errbuf+1);
            }
        } else {
            if (!copy) {
                /* No COPY option: remove the local key, signal the change. */
                dbDelete(c->db,kv[j]);
                signalModifiedKey(c->db,kv[j]);
                server.dirty++;

                /* Populate the argument vector to replace the old one. */
                newargv[del_idx++] = kv[j];
                incrRefCount(kv[j]);
            }
        }
    }

    /* On socket error, if we want to retry, do it now before rewriting the
     * command vector. We only retry if we are sure nothing was processed
     * and we failed to read the first reply (j == 0 test). */
    if (!error_from_target && socket_error && j == 0 && may_retry &&
        errno != ETIMEDOUT)
    {
        goto socket_err; /* A retry is guaranteed because of tested conditions.*/
    }

    /* On socket errors, close the migration socket now that we still have
     * the original host/port in the ARGV. Later the original command may be
     * rewritten to DEL and will be too later. */
    if (socket_error) migrateCloseSocket(c->argv[1],c->argv[2]);

    if (!copy) {
        /* Translate MIGRATE as DEL for replication/AOF. Note that we do
         * this only for the keys for which we received an acknowledgement
         * from the receiving Redis server, by using the del_idx index. */
        if (del_idx > 1) {
            newargv[0] = createStringObject("DEL",3);
            /* Note that the following call takes ownership of newargv. */
            replaceClientCommandVector(c,del_idx,newargv);
            argv_rewritten = 1;
        } else {
            /* No key transfer acknowledged, no need to rewrite as DEL. */
            zfree(newargv);
        }
        newargv = NULL; /* Make it safe to call zfree() on it in the future. */
    }

    /* If we are here and a socket error happened, we don't want to retry.
     * Just signal the problem to the client, but only do it if we did not
     * already queue a different error reported by the destination server. */
    if (!error_from_target && socket_error) {
        may_retry = 0;
        goto socket_err;
    }

    if (!error_from_target) {
        /* Success! Update the last_dbid in migrateCachedSocket, so that we can
         * avoid SELECT the next time if the target DB is the same. Reply +OK.
         *
         * Note: If we reached this point, even if socket_error is true
         * still the SELECT command succeeded (otherwise the code jumps to
         * socket_err label. */
        cs->last_dbid = dbid;
        addReply(c,shared.ok);
    } else {
        /* On error we already sent it in the for loop above, and set
         * the currently selected socket to -1 to force SELECT the next time. */
    }

    sdsfree(cmd.io.buffer.ptr);
    zfree(ov); zfree(kv); zfree(newargv);
    return;

/* On socket errors we try to close the cached socket and try again.
 * It is very common for the cached socket to get closed, if just reopening
 * it works it's a shame to notify the error to the caller. */
socket_err:
    /* Cleanup we want to perform in both the retry and no retry case.
     * Note: Closing the migrate socket will also force SELECT next time. */
    sdsfree(cmd.io.buffer.ptr);

    /* If the command was rewritten as DEL and there was a socket error,
     * we already closed the socket earlier. While migrateCloseSocket()
     * is idempotent, the host/port arguments are now gone, so don't do it
     * again. */
    if (!argv_rewritten) migrateCloseSocket(c->argv[1],c->argv[2]);
    zfree(newargv);
    newargv = NULL; /* This will get reallocated on retry. */

    /* Retry only if it's not a timeout and we never attempted a retry
     * (or the code jumping here did not set may_retry to zero). */
    if (errno != ETIMEDOUT && may_retry) {
        may_retry = 0;
        goto try_again;
    }

    /* Cleanup we want to do if no retry is attempted. */
    zfree(ov); zfree(kv);
    addReplySds(c,
        sdscatprintf(sdsempty(),
            "-IOERR error or timeout %s to target instance\r\n",
            write_error ? "writing" : "reading"));
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * -------------------------------------------------------------------------- */

/* The ASKING command is required after a -ASK redirection.
 * The client should issue ASKING before to actually send the command to
 * the target instance. See the Redis Cluster specification for more
 * information. */
void askingCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_ASKING;
    addReply(c,shared.ok);
}

/* The READONLY command is used by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. */
// 将一个client置为readonly模式
// 只要该client表示自己使用只读命令,那么就可以连接集群中的备节点,而不会被重定向到主节点
void readonlyCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* The READWRITE command just clears the READONLY command state. */
// 关闭client的readonly模式
void readwriteCommand(client *c) {
    c->flags &= ~CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like LPOPRPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be perfomed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to CLUSTER_REDIR_ASK or
 * CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * CLUSTER_REDIR_UNSTABLE if the request contains multiple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress).
 *
 * CLUSTER_REDIR_DOWN_UNBOUND if the request addresses a slot which is
 * not bound to any node. In this case the cluster global state should be
 * already "down" but it is fragile to rely on the update of the global state,
 * so we also handle it here.
 *
 * CLUSTER_REDIR_DOWN_STATE if the cluster is down but the user attempts to
 * execute a command that addresses one or more keys. */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *error_code) {
    clusterNode *n = NULL;
    robj *firstkey = NULL;
    int multiple_keys = 0; // 是否有多个不同值的key
    multiState *ms, _ms;
    multiCmd mc;
    int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0;

    /* Allow any key to be set if a module disabled cluster redirections. */
    // 在关闭集群重定向的时候,由本节点来处理此命令
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return myself;

    /* Set error code optimistically for the base case. */
    // 设置默认的初始化值,重定向正常
    if (error_code) *error_code = CLUSTER_REDIR_NONE;

    /* Modules can turn off Redis Cluster redirection: this is useful
     * when writing a module that implements a completely different
     * distributed system. */

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything */
    if (cmd->proc == execCommand) {
        /* If CLIENT_MULTI flag is not set EXEC is just going to return an
         * error. */
        // 当前命令属于执行命令,但是当前client并没有设置事务开启标记,则返回本节点
        if (!(c->flags & CLIENT_MULTI)) return myself;
        ms = &c->mstate;
    } else {
        /* In order to have a single codepath create a fake Multi State
         * structure if the client is not in MULTI/EXEC state, this way
         * we have a single codepath below. */
        ms = &_ms;
        _ms.commands = &mc;
        _ms.count = 1;
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
    }

    // 至此, 需要对待执行命令的key判断是否属于同一个槽位,即使是multi/exec的事务数据也需要进行判定

    /* Check that all the keys are in the same hash slot, and obtain this
     * slot and the node associated. */
    for (i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, *keyindex, numkeys, j;

        mcmd = ms->commands[i].cmd;
        margc = ms->commands[i].argc;
        margv = ms->commands[i].argv;

        keyindex = getKeysFromCommand(mcmd,margv,margc,&numkeys);
        for (j = 0; j < numkeys; j++) {
            robj *thiskey = margv[keyindex[j]];
            int thisslot = keyHashSlot((char*)thiskey->ptr,
                                       sdslen(thiskey->ptr));

            if (firstkey == NULL) {
                /* This is the first key we see. Check what is the slot
                 * and node. */
                firstkey = thiskey;
                slot = thisslot;
                n = server.cluster->slots[slot];

                /* Error: If a slot is not served, we are in "cluster down"
                 * state. However the state is yet to be updated, so this was
                 * not trapped earlier in processCommand(). Report the same
                 * error to the client. */
                // 对于当前槽位没有节点负责时,返回此槽位没有节点负责
                if (n == NULL) {
                    getKeysFreeResult(keyindex);
                    if (error_code)
                        *error_code = CLUSTER_REDIR_DOWN_UNBOUND;
                    return NULL;
                }

                /* If we are migrating or importing this slot, we need to check
                 * if we have all the keys in the request (the only way we
                 * can safely serve the request, otherwise we return a TRYAGAIN
                 * error). To do so we set the importing/migrating state and
                 * increment a counter for every missing key. */
                // 判断当前slot是处于迁移外部?还是导入?
                if (n == myself &&
                    server.cluster->migrating_slots_to[slot] != NULL)
                {
                    migrating_slot = 1;
                } else if (server.cluster->importing_slots_from[slot] != NULL) {
                    importing_slot = 1;
                }
            } else {
                /* If it is not the first key, make sure it is exactly
                 * the same key as the first we saw. */
                // key去重,只判定不同的key
                if (!equalStringObjects(firstkey,thiskey)) {
                    if (slot != thisslot) {
                        // 至少有两个key处于不同的slot中
                        /* Error: multiple keys from different slots. */
                        getKeysFreeResult(keyindex);
                        if (error_code)
                            *error_code = CLUSTER_REDIR_CROSS_SLOT;
                        return NULL;
                    } else {
                        /* Flag this request as one with multiple different
                         * keys. */
                        multiple_keys = 1;
                    }
                }
            }

            // 对于key处于迁移走 or 导入中的情况,再次确认此key当前是否在本字典中
            /* Migarting / Improrting slot? Count keys we don't have. */
            if ((migrating_slot || importing_slot) &&
                lookupKeyRead(&server.db[0],thiskey) == NULL)
            {
                missing_keys++;
            }
        }
        getKeysFreeResult(keyindex);
    }

    /* No key at all in command? then we can serve the request
     * without redirections or errors in all the cases. */
    if (n == NULL) return myself;

    /* Cluster is globally down but we got keys? We can't serve the request. */
    // 本节点记录的集群状态异常,则直接返回异常信息提示
    if (server.cluster->state != CLUSTER_OK) {
        if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
        return NULL;
    }

    /* Return the hashslot by reference. */
    if (hashslot) *hashslot = slot;

    /* MIGRATE always works in the context of the local node if the slot
     * is open (migrating or importing state). We need to be able to freely
     * move keys among instances in this case. */
    // 有槽位处于移动中,而且时迁移命令,那么有本节点负责
    if ((migrating_slot || importing_slot) && cmd->proc == migrateCommand)
        return myself;

    /* If we don't have all the keys and we are migrating the slot, send
     * an ASK redirection. */
    // 此槽位在迁移中,而且当前节点已无此槽位,那么返回ASK标记,并返回此槽位所在的节点
    if (migrating_slot && missing_keys) {
        if (error_code) *error_code = CLUSTER_REDIR_ASK;
        return server.cluster->migrating_slots_to[slot];
    }

    /* If we are receiving the slot, and the client correctly flagged the
     * request as "ASKING", we can serve the request. However if the request
     * involves multiple keys and we don't have them all, the only option is
     * to send a TRYAGAIN error. */
    // 槽位目前处于导入中, 而且客户端标记为asking的话,尽可能由本节点处理
    // 只有当处理的命令涉及到多个key,而且目前还有key没有导入的场景,表示后续可以再试
    if (importing_slot &&
        (c->flags & CLIENT_ASKING || cmd->flags & CMD_ASKING))
    {
        if (multiple_keys && missing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            return myself;
        }
    }

    /* Handle the read-only client case reading from a slave: if this
     * node is a slave and the request is about an hash slot our master
     * is serving, we can reply without redirection. */
    // 对于处于只读模式的client 或者是eval sha命令,且当前实例是备节点,而且此备节点的主节点负责slot
    if (c->flags & CLIENT_READONLY &&
        (cmd->flags & CMD_READONLY || cmd->proc == evalCommand ||
         cmd->proc == evalShaCommand) &&
        nodeIsSlave(myself) &&
        myself->slaveof == n)
    {
        return myself;
    }

    /* Base case: just return the right node. However if this node is not
     * myself, set error_code to MOVED since we need to issue a rediretion. */
    // 非当前节点负责,返回moved命令并提供正确的节点地址
    if (n != myself && error_code) *error_code = CLUSTER_REDIR_MOVED;
    return n;
}

/* Send the client the right redirection code, according to error_code
 * that should be set to one of CLUSTER_REDIR_* macros.
 *
 * If CLUSTER_REDIR_ASK or CLUSTER_REDIR_MOVED error codes
 * are used, then the node 'n' should not be NULL, but should be the
 * node we want to mention in the redirection. Moreover hashslot should
 * be set to the hash slot that caused the redirection. */
// 发送指定的重定向结果给client
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code) {
    if (error_code == CLUSTER_REDIR_CROSS_SLOT) {
        addReplySds(c,sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_UNSTABLE) {
        /* The request spawns multiple keys in the same slot,
         * but the slot is not "stable" currently as there is
         * a migration or import in progress. */
        addReplySds(c,sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
        addReplySds(c,sdsnew("-CLUSTERDOWN The cluster is down\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_UNBOUND) {
        addReplySds(c,sdsnew("-CLUSTERDOWN Hash slot not served\r\n"));
    } else if (error_code == CLUSTER_REDIR_MOVED ||
               error_code == CLUSTER_REDIR_ASK)
    {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-%s %d %s:%d\r\n",
            (error_code == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
            hashslot,n->ip,n->port));
    } else {
        serverPanic("getNodeByQuery() unknown error.");
    }
}

/* This function is called by the function processing clients incrementally
 * to detect timeouts, in order to handle the following case:
 *
 * 1) A client blocks with BLPOP or similar blocking operation.
 * 2) The master migrates the hash slot elsewhere or turns into a slave.
 * 3) The client may remain blocked forever (or up to the max timeout time)
 *    waiting for a key change that will never happen.
 *
 * If the client is found to be blocked into an hash slot this node no
 * longer handles, the client is sent a redirection error, and the function
 * returns 1. Otherwise 0 is returned and no operation is performed. */
// 对于处于阻塞中的client,判断此client所用的slot是否依旧由本节点负责, 如果此slot已由其他节点负责,则发送重定向命令
int clusterRedirectBlockedClientIfNeeded(client *c) {
    if (c->flags & CLIENT_BLOCKED &&
        (c->btype == BLOCKED_LIST ||
         c->btype == BLOCKED_ZSET ||
         c->btype == BLOCKED_STREAM))
    {
        dictEntry *de;
        dictIterator *di;

        /* If the cluster is down, unblock the client with the right error. */
        // 集群不正常
        if (server.cluster->state == CLUSTER_FAIL) {
            clusterRedirectClient(c,NULL,0,CLUSTER_REDIR_DOWN_STATE);
            return 1;
        }

        // 遍历该阻塞client所关注的所有key
        /* All keys must belong to the same slot, so check first key only. */
        di = dictGetIterator(c->bpop.keys);
        if ((de = dictNext(di)) != NULL) {
            robj *key = dictGetKey(de);
            int slot = keyHashSlot((char*)key->ptr, sdslen(key->ptr));
            clusterNode *node = server.cluster->slots[slot];

            /* We send an error and unblock the client if:
             * 1) The slot is unassigned, emitting a cluster down error.
             * 2) The slot is not handled by this node, nor being imported. */
            // 槽位并不是由本节点负责,而且此槽位也没有导入中
            if (node != myself &&
                server.cluster->importing_slots_from[slot] == NULL)
            {
                if (node == NULL) {
                    clusterRedirectClient(c,NULL,0,
                        CLUSTER_REDIR_DOWN_UNBOUND);
                } else {
                    clusterRedirectClient(c,node,slot,
                        CLUSTER_REDIR_MOVED);
                }
                dictReleaseIterator(di);
                return 1;
            }
        }
        dictReleaseIterator(di);
    }
    return 0;
}
