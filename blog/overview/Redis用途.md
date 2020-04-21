# Redis用途
Redis全称REmote DIctionary Server,一种高性能的内存级数据存储<span style="color:red;">[1]</span>,自有支持丰富的数据结构以及功能以满足不同的业务需求.

* 字符串
* 哈希
* 链表
* 集合
* 有序集合
* 位图
* 基于地理坐标索引
* 定时失效机制
* 支持原子性操作
* 事务机制
* 集群高可用

因其高性能的优异表现,同时也支持数据持久化,实际业务中主要用于内存缓存/No-SQL数据库/简易版消息队列应用场景.

Redis自身由ANSI C编写,遵循POSIX标准,在Linux以及OS X两大操作系统下得到很好的支持,同时也是Redis开发与测试的两大操作系统.

Redis读写性能极为优异,通过不同的配置组合可以支持:纯内存存储/持久化存储/主备集群/分布式集群,以满足不同业务对读写性能不同的需求.

如下给出一些Redis的性能数据<span style="color:red;">[2]</span>,需注意影响性能的因素有很多,例如硬件/数据已有规模/key-value数据长度/操作类别/集群搭建模式/是否开启持久化等.实际项目中还请基于自身环境进行压测以决定一些关键配置参数.

```
Intel(R) Xeon(R) CPU E5520 @ 2.27GHz (without pipelining)

$ ./redis-benchmark -r 1000000 -n 2000000 -t get,set,lpush,lpop -q
SET: 122556.53 requests per second
GET: 123601.76 requests per second
LPUSH: 136752.14 requests per second
LPOP: 132424.03 requests per second
```

***参考资料***
1. https://redis.io/topics/introduction
2. https://redis.io/topics/benchmarks
