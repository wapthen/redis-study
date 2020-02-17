#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOTS 16384 // 槽的总数,2的14次方
#define CLUSTER_OK 0          /* Everything looks ok */
#define CLUSTER_FAIL 1        /* The cluster can't work */
// cluster node id标示长度
#define CLUSTER_NAMELEN 40    /* sha1 hex length */
// cluster bus端口=base port+如下值
#define CLUSTER_PORT_INCR 10000 /* Cluster port = baseport + PORT_INCR */

/* The following defines are amount of time, sometimes expressed as
 * multiplicators of the node timeout value (when ending with MULT). */
#define CLUSTER_DEFAULT_NODE_TIMEOUT 15000
#define CLUSTER_DEFAULT_SLAVE_VALIDITY 10 /* Slave max data age factor. */
#define CLUSTER_DEFAULT_REQUIRE_FULL_COVERAGE 1
#define CLUSTER_DEFAULT_SLAVE_NO_FAILOVER 0 /* Failover by default. */
// 失败报告过期乘数因子
#define CLUSTER_FAIL_REPORT_VALIDITY_MULT 2 /* Fail report validity. */
#define CLUSTER_FAIL_UNDO_TIME_MULT 2 /* Undo fail if master is back. */
#define CLUSTER_FAIL_UNDO_TIME_ADD 10 /* Some additional time. */
#define CLUSTER_FAILOVER_DELAY 5 /* Seconds */
#define CLUSTER_DEFAULT_MIGRATION_BARRIER 1
#define CLUSTER_MF_TIMEOUT 5000 /* Milliseconds to do a manual failover. */
#define CLUSTER_MF_PAUSE_MULT 2 /* Master pause manual failover mult. */
#define CLUSTER_SLAVE_MIGRATION_DELAY 5000 /* Delay for slave migration. */

/* Redirection errors returned by getNodeByQuery(). */
// 重定向结果类别
// 正常
#define CLUSTER_REDIR_NONE 0          /* Node can serve the request. */
// 不同的key处于不同的slot中
#define CLUSTER_REDIR_CROSS_SLOT 1    /* -CROSSSLOT request. */
// 多key在导入中,其中有的key还未导入,不稳定,可以后续再试.
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required */
// 需到其他节点询问,表示目前处于迁移外部节点中,而且此key已经迁移走
#define CLUSTER_REDIR_ASK 3           /* -ASK redirection required. */
// 该slot是由其他主节点负责
#define CLUSTER_REDIR_MOVED 4         /* -MOVED redirection required. */
// 集群状态处于down中
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. */
// 此槽位没有主节点负责
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, unbound slot. */

struct clusterNode;

/* clusterLink encapsulates everything needed to talk with a remote node. */
// cluster link结构体代表了本节点跟集群其他节点通信所用的client信息
typedef struct clusterLink {
    // tcp 长连接创建时刻
    mstime_t ctime;             /* Link creation time */
    // 用于bus通信的tcp 套接字句柄
    int fd;                     /* TCP socket file descriptor */
    // tcp 发送缓冲区
    sds sndbuf;                 /* Packet send buffer */
    // tcp 接收缓冲区             
    sds rcvbuf;                 /* Packet reception buffer */
    // 指向本link代表的对端node节点,实际的node节点存在与clusterState里的node字典中
    // 注意此node可能为null
    // 为null,表示此link是当前tcp启动期间里是被动呼起
    // 不为null,表示此link时当前tcp启动期间时主动发起
    struct clusterNode *node;   /* Node related to this link if any, or NULL */
} clusterLink;

/* Cluster node flags and macros. */
// 集群中单个节点状态
// 如无配置文件,节点自启动时默认为主节点
#define CLUSTER_NODE_MASTER 1     /* The node is a master */
#define CLUSTER_NODE_SLAVE 2      /* The node is a slave */
#define CLUSTER_NODE_PFAIL 4      /* Failure? Need acknowledge */
#define CLUSTER_NODE_FAIL 8       /* The node is believed to be malfunctioning */
// node字典里标注当前节点的标志
#define CLUSTER_NODE_MYSELF 16    /* This node is myself */
// 需要跟该node节点进行握手识别
#define CLUSTER_NODE_HANDSHAKE 32 /* We have still to exchange the first ping */
// 节点目前还不知道ip地址
#define CLUSTER_NODE_NOADDR   64  /* We don't know the address of this node */
// 需要向该节点发送一个MEET消息, 收到MEET消息的这一方会将发送消息方的信息加入到本节点记录的集群节点字典里
#define CLUSTER_NODE_MEET 128     /* Send a MEET message to this node */
// 主节点有备节点,处于可以数据复制操作的状态
#define CLUSTER_NODE_MIGRATE_TO 256 /* Master eligible for replica migration. */
// 备节点不允许进行故障迁移
#define CLUSTER_NODE_NOFAILOVER 512 /* Slave will not try to failover. */
#define CLUSTER_NODE_NULL_NAME "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"

#define nodeIsMaster(n) ((n)->flags & CLUSTER_NODE_MASTER)
#define nodeIsSlave(n) ((n)->flags & CLUSTER_NODE_SLAVE)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeWithoutAddr(n) ((n)->flags & CLUSTER_NODE_NOADDR)
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)
#define nodeCantFailover(n) ((n)->flags & CLUSTER_NODE_NOFAILOVER)

/* Reasons why a slave is not able to failover. */
// 备节点不能完成故障迁移的原因
#define CLUSTER_CANT_FAILOVER_NONE 0
#define CLUSTER_CANT_FAILOVER_DATA_AGE 1
#define CLUSTER_CANT_FAILOVER_WAITING_DELAY 2
#define CLUSTER_CANT_FAILOVER_EXPIRED 3
#define CLUSTER_CANT_FAILOVER_WAITING_VOTES 4
#define CLUSTER_CANT_FAILOVER_RELOG_PERIOD (60*5) /* seconds. */

/* clusterState todo_before_sleep flags. */
// 集群待处理的事项
// 需异步处理故障迁移工作
#define CLUSTER_TODO_HANDLE_FAILOVER (1<<0)
// 需异步重新计算并更新内存里的集群状态
#define CLUSTER_TODO_UPDATE_STATE (1<<1)
// 需异步保存配置文件
#define CLUSTER_TODO_SAVE_CONFIG (1<<2)
// 需异步刷盘配置文件
#define CLUSTER_TODO_FSYNC_CONFIG (1<<3)

/* Message types.
 *
 * Note that the PING, PONG and MEET messages are actually the same exact
 * kind of packet. PONG is the reply to ping, in the exact format as a PING,
 * while MEET is a special PING that forces the receiver to add the sender
 * as a node (if it is not already in the list). */
// 用于集群消息交换的消息类别
// ping,pong,meet是相同格式的数据,只有type类型的差别,另外收到meet消息需要无条件的将发送方节点添加到集群中
#define CLUSTERMSG_TYPE_PING 0          /* Ping */
#define CLUSTERMSG_TYPE_PONG 1          /* Pong (reply to Ping) */
#define CLUSTERMSG_TYPE_MEET 2          /* Meet "let's join" message */
#define CLUSTERMSG_TYPE_FAIL 3          /* Mark node xxx as failing */
#define CLUSTERMSG_TYPE_PUBLISH 4       /* Pub/Sub Publish propagation */
// 备节点发送的故障迁移投票消息,此类型消息只含有消息头
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5 /* May I failover? */
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK 6     /* Yes, you have my vote */
#define CLUSTERMSG_TYPE_UPDATE 7        /* Another node slots configuration */
#define CLUSTERMSG_TYPE_MFSTART 8       /* Pause clients for manual failover */
#define CLUSTERMSG_TYPE_MODULE 9        /* Module cluster API message. */
// 作为消息类别的边界使用
#define CLUSTERMSG_TYPE_COUNT 10        /* Total number of message types. */

/* Flags that a module can set in order to prevent certain Redis Cluster
 * features to be enabled. Useful when implementing a different distributed
 * system on top of Redis Cluster message bus, using modules. */
#define CLUSTER_MODULE_FLAG_NONE 0
// 阻止故障迁移的标记
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1<<1)
// 阻止重定向的标记
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1<<2)

/**
 * 如下结构体的关系是 
 * clusterstate全局只有一个实例, 用于当前实例统一维护集群的所有信息
 * 其中含有一个clusternode字典结构,存有集群内所有的节点信息
 * clusternode内部有一个clusterlink结构体,存有当前节点跟此node通信用的tcp长连接信息;
 * 同时clusternode内部含有一个clusternodefailreport链表,存有兄弟节点报告此node节点为FAIL or PFAIL的信息
 *  clusterState ---> clusterNode array
 *  clusterNode <---> clusterLink
 *  clusterNode -----> clusterNodeFailReport list
 */
/* This structure represent elements of node->fail_reports. */
// 失败报告信息结构体
typedef struct clusterNodeFailReport {
    // 指向是由哪个node节点发出的失败报告, 实际node节点存于clusterstate中的node字典
    // 表示发送此消息的节点
    struct clusterNode *node;  /* Node reporting the failure condition. */
    // 失败报告的最新时刻
    mstime_t time;             /* Time of the last report from this node. */
} clusterNodeFailReport;

// 集群节点结构体
typedef struct clusterNode {
    // node节点创建的时刻,单位毫秒
    mstime_t ctime; /* Node object creation time. */
    // node id标示
    char name[CLUSTER_NAMELEN]; /* Node name, hex string, sha1-size */
    // 节点状态字段
    int flags;      /* CLUSTER_NODE_... */
    // 节点的最新配置纪元,集群里的每一个节点的configEpoch数值均不相同
    uint64_t configEpoch; /* Last configEpoch observed for this node */
    // 节点记录的槽位图, 位图中对应bit位为1标示由本node节点负责处理
    unsigned char slots[CLUSTER_SLOTS/8]; /* slots handled by this node */
    // 节点上负责处理的槽位个数
    int numslots;   /* Number of slots handled by this node */
    // 如果该节点信息是主节点,则此值为该主节点下的备节点个数
    int numslaves;  /* Number of slave nodes, if this is a master */
    // 如果该节点信息是主节点,则此值为该主节点下的备节点指针数组,具体个数由numslaves确定
    struct clusterNode **slaves; /* pointers to slave nodes */
    // 如果该节点为备节点,则此值指向主节点
    struct clusterNode *slaveof; /* pointer to the master node. Note that it
                                    may be NULL even if the node is a slave
                                    if we don't have the master node in our
                                    tables. */
    // 节点最新一次发送ping的时刻,单位毫秒, 在收到对应的pong后会对此值清为0
    mstime_t ping_sent;      /* Unix time we sent latest ping */
    // 节点最新一次接收pong的时刻,单位毫秒, 此值不会备清0,因为需要使用此字段作为随机选取数值最小的节点进行ping
    mstime_t pong_received;  /* Unix time we received the pong */
    mstime_t fail_time;      /* Unix time when FAIL flag was set */
    mstime_t voted_time;     /* Last time we voted for a slave of this master */
    // 当前节点收到最新复制偏移量数据的时刻
    mstime_t repl_offset_time;  /* Unix time we received offset for this node */
    // 主节点判定为孤儿主节点的时刻
    mstime_t orphaned_time;     /* Starting time of orphaned master condition */
    // 当前节点最新的复制偏移量
    long long repl_offset;      /* Last known repl offset for this node. */
    // 节点的ip地址
    char ip[NET_IP_STR_LEN];  /* Latest known IP address of this node */
    // 节点的监听端口
    int port;                   /* Latest known clients port of this node */
    // 节点用于cluster bus通信的监听端口
    int cport;                  /* Latest known cluster port of this node. */
    // 当前节点对应的link信息
    // 此字段为null时,会通过clusterCron函数来建立socket连接
    // 此字段非null时,是一个写通道连接,当前运行实例写入数据,通过此link发送给该node节点
    clusterLink *link;          /* TCP/IP link with this node */
    // 周边那些主节点标注此节点为失败or疑似失败的报告链表, 只记录sender为主节点发出的通知
    list *fail_reports;         /* List of nodes signaling this as failing */
} clusterNode;

// 集群信息结构体
typedef struct clusterState {
    // 当前进程所在的node节点
    clusterNode *myself;  /* This node */
    // 集群当前的配置纪元,稳定情况下集群里的所有主备节点的currentEpoch都应为同一个值
    uint64_t currentEpoch;
    // 集群状态
    int state;            /* CLUSTER_OK, CLUSTER_FAIL, ... */
    // 集群里的主节点个数
    int size;             /* Num of master nodes with at least one slot */
    // 集群里的节点字典, 记录了 nodeid-->node的映射关系
    dict *nodes;          /* Hash table of name -> clusterNode structures */
    // 集群里处在黑名单中的节点字典,在此字典中的节点短期内时不会被加入到集群中
    dict *nodes_black_list; /* Nodes we don't re-add for a few seconds. */
    // 集群中处于正在迁出的槽位以及对应迁出的节点指针数组
    clusterNode *migrating_slots_to[CLUSTER_SLOTS];
    // 集群中处于正在迁入的槽位以及对应迁入的节点指针数组
    clusterNode *importing_slots_from[CLUSTER_SLOTS];
    // 集群槽位图对应的各个node节点指针数组,表示 槽id-->node节点 映射关系
    clusterNode *slots[CLUSTER_SLOTS];
    // 集群里每个槽位里所包含的key主键个数
    uint64_t slots_keys_count[CLUSTER_SLOTS];
    // 集群里 槽id-->key 映射关系
    rax *slots_to_keys;
    /* The following fields are used to take the slave state on elections. */
    // 故障迁移的开始时间
    mstime_t failover_auth_time; /* Time of previous or next election. */
    // 截止当前,本节点收到的投票数
    int failover_auth_count;    /* Number of votes received so far. */
    // 截止当前,本节点是否已经发送出投票
    int failover_auth_sent;     /* True if we already asked for votes. */
    // 本节点的rank值,rank值越小越有可能升为主
    int failover_auth_rank;     /* This slave rank for current auth request. */
    // 备节点发出迁移投票的集群纪元
    uint64_t failover_auth_epoch; /* Epoch of the current election. */
    int cant_failover_reason;   /* Why a slave is currently not able to
                                   failover. See the CANT_FAILOVER_* macros. */
    /* Manual failover state in common. */
    // 手工迁移的截止时刻
    mstime_t mf_end;            /* Manual failover time limit (ms unixtime).
                                   It is zero if there is no MF in progress. */
    /* Manual failover state of master. */
    // 执行手动迁移的备节点
    clusterNode *mf_slave;      /* Slave performing the manual failover. */
    /* Manual failover state of slave. */
    // 手工迁移时主节点的偏移量
    long long mf_master_offset; /* Master offset the slave needs to start MF
                                   or zero if stil not received. */
    // 手工迁移符合条件,可以开始
    int mf_can_start;           /* If non-zero signal that the manual failover
                                   can start requesting masters vote. */
    /* The followign fields are used by masters to take state on elections. */
    uint64_t lastVoteEpoch;     /* Epoch of the last vote granted. */
    int todo_before_sleep; /* Things to do in clusterBeforeSleep(). */
    /* Messages received and sent by type. */
    long long stats_bus_messages_sent[CLUSTERMSG_TYPE_COUNT];//统计各种类型的消息发送次数
    long long stats_bus_messages_received[CLUSTERMSG_TYPE_COUNT];//统计各种各类型的消息接收次数
    long long stats_pfail_nodes;   // 处于PFAIL状态的节点个数
                                      /* Number of nodes in PFAIL status,
                                       excluding nodes without address. */
} clusterState;

/* Redis cluster messages header */

/* Initially we don't know our "name", but we'll find it once we connect
 * to the first node, using the getsockname() function. Then we'll use this
 * address for all the next messages. */
typedef struct {
    char nodename[CLUSTER_NAMELEN];//节点nodeid
    uint32_t ping_sent;//最新一次发送ping的时刻,单位秒
    uint32_t pong_received;//最新一次收到pong的时刻,单位秒
    char ip[NET_IP_STR_LEN];  /* IP address last time it was seen */
    uint16_t port;              /* base port last time it was seen */
    uint16_t cport;             /* cluster port last time it was seen */
    uint16_t flags;             /* node->flags copy */
    uint32_t notused1;
} clusterMsgDataGossip;

// 广播某一个节点是fail的信息结构体
typedef struct {
    char nodename[CLUSTER_NAMELEN];//FAIL的节点标识
} clusterMsgDataFail;

typedef struct {
    uint32_t channel_len;
    uint32_t message_len;
    unsigned char bulk_data[8]; /* 8 bytes just as placeholder. */
} clusterMsgDataPublish;

typedef struct {
    uint64_t configEpoch; /* Config epoch of the specified instance. */
    // 节点ID
    char nodename[CLUSTER_NAMELEN]; /* Name of the slots owner. */
    // 槽位图
    unsigned char slots[CLUSTER_SLOTS/8]; /* Slots bitmap. */
} clusterMsgDataUpdate;

typedef struct {
    uint64_t module_id;     /* ID of the sender module. */
    uint32_t len;           /* ID of the sender module. */
    uint8_t type;           /* Type from 0 to 255. */
    unsigned char bulk_data[3]; /* 3 bytes just as placeholder. */
} clusterMsgModule;

union clusterMsgData {
    /* PING, MEET and PONG */
    struct {
        /* Array of N clusterMsgDataGossip structures */
        clusterMsgDataGossip gossip[1];
    } ping;

    /* FAIL */
    struct {
        clusterMsgDataFail about;
    } fail;

    /* PUBLISH */
    struct {
        clusterMsgDataPublish msg;
    } publish;

    /* UPDATE */
    struct {
        clusterMsgDataUpdate nodecfg;
    } update;

    /* MODULE */
    struct {
        clusterMsgModule msg;
    } module;
};

#define CLUSTER_PROTO_VER 1 /* Cluster bus protocol version. */

// cluster bus集群传播的消息结构体
typedef struct {
    // 消息体标记
    char sig[4];        /* Signature "RCmb" (Redis Cluster message bus). */
    // 消息体整体长度,包含本结构体
    uint32_t totlen;    /* Total length of this message */
    // 消息版本号
    uint16_t ver;       /* Protocol version, currently set to 1. */
    // redis base port
    uint16_t port;      /* TCP base port number. */
    // 消息类别
    uint16_t type;      /* Message type */
    // 消息body里存储的实际个数,只对ping pong meet类型有效
    uint16_t count;     /* Only used for some kind of messages. */
    // 发送此消息的节点所记录的集群纪元
    uint64_t currentEpoch;  /* The epoch accordingly to the sending node. */
    // 发送此消息的节点所记录的节点纪元
    uint64_t configEpoch;   /* The config epoch if it's a master, or the last
                               epoch advertised by its master if it is a
                               slave. */
    uint64_t offset;    /* Master replication offset if node is a master or
                           processed replication offset if node is a slave. */
    // 发送方的节点id
    char sender[CLUSTER_NAMELEN]; /* Name of the sender node */
    // 发送此消息的节点所记录的主节点中的槽位图
    unsigned char myslots[CLUSTER_SLOTS/8];
    // 发送方所对应的主节点id
    char slaveof[CLUSTER_NAMELEN];
    // 发送方的ip地址
    char myip[NET_IP_STR_LEN];    /* Sender IP, if not all zeroed. */
    // 填充数据
    char notused1[34];  /* 34 bytes reserved for future usage. */
    // 发送此消息的节点bus端口号
    uint16_t cport;      /* Sender TCP cluster bus port */
    // 发送此消息的节点标记字段
    uint16_t flags;      /* Sender node flags */
    // 发送此消的节点记录的集群状态
    unsigned char state; /* Cluster state from the POV of the sender */
    unsigned char mflags[3]; /* Message flags: CLUSTERMSG_FLAG[012]_... */
    // 消息body, body体里时不会含有发送者的相关信息,发送者的状态信息已经明显的在消息头里
    union clusterMsgData data;
} clusterMsg;

// 消息体头部字段长度
#define CLUSTERMSG_MIN_LEN (sizeof(clusterMsg)-sizeof(union clusterMsgData))

/* Message flags better specify the packet content or are used to
 * provide some information about the node state. */
#define CLUSTERMSG_FLAG0_PAUSED (1<<0) /* Master paused for manual failover. */
#define CLUSTERMSG_FLAG0_FORCEACK (1<<1) /* Give ACK to AUTH_REQUEST even if
                                            master is up. */

/* ---------------------- API exported outside cluster.c -------------------- */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);

#endif /* __CLUSTER_H */
