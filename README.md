# redis-study
## 介绍
* 本repo分析的redis版本为5.0.5版本
* 希望以图的形式勾画出redis的内存组织结构,异步数据处理以及集群管理方式.

* redis涉及到 数据结构/算法/网络编程/信号/多进程通信/多线程同步/文件锁/磁盘io等相关技术知识.
* 上述技术可以参考《APUE》《UNP》书籍中的详细讲解.

## 文章
1. [Redis用途](https://github.com/wapthen/redis-study/blob/master/blog/overview/Redis用途.md)
2. [高性能原因](https://github.com/wapthen/redis-study/blob/master/blog/overview/高性能原因.md) 
3. [sds字符串](https://github.com/wapthen/redis-study/blob/master/blog/overview/sds字符串.md) 
4. [expire实现机制](https://github.com/wapthen/redis-study/blob/master/blog/overview/expire实现机制.md) 
5. [数据淘汰机制](https://github.com/wapthen/redis-study/blob/master/blog/overview/数据淘汰机制.md) 
6. [文件锁](https://github.com/wapthen/redis-study/blob/master/blog/overview/文件锁.md)
7. [数据持久化机制](https://github.com/wapthen/redis-study/blob/master/blog/overview/数据持久化机制.md) 
8. [dict字典里的安全迭代器与非安全迭代器分析](https://github.com/wapthen/redis-study/blob/master/blog/overview/dict字典里的安全迭代器与非安全迭代器分析.md) 

## 一. 基础数据结构

### 1.1 adlist双向链表
![adlist](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/adlist.png)  

### 1.2 sds字符串
1.2.1**SDS字符串类型说明**
- *sds类型其实是 char指针: typedef char \*sds ;其直接指向sdshdr句柄后部的载体数据;*
- *sds类型可以存储text or binary;*
- *sdshdr句柄采用内存压缩,非内存对齐模式,以压缩内存使用量;*
- *sdshdr句柄跟实际载体数据前后紧挨着,内存空间开辟时,句柄与载体数据一同开辟;*
- *sds = sdshdr句柄里的末尾成员char buf[], 注意此处为0长度数组;*
- *sds在开辟/设值时会统一在尾部追加一个/0, 但在sdshdr句柄里的任何字段均不会体现出该1个字节;*

1.2.2**SDS_TYPE_5说明**
- *此类型不会出现在创建 或者 扩容 sds阶段, 这两个阶段所用的最短类别为SDS_TYPE_8;*
- *此类型只会出现在压缩sds阶段,即可以压缩为SDS_TYPE_5类别;*

![sds](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/sds.png)  

### 1.3 dict字典
1.3.1**字典 安全迭代器 与 非安全迭代器 介绍**
  - *需要安全迭代器的原因*
    - 场景:一边遍历一边增加/删除字典里的元素.
    - 上述场景在字典处于渐进式数据迁移时,如不暂停数据迁移会导致遍历过程出现元素重复or丢失异常.
    - 在遍历时需要对字典进行增删的场景下只能使用安全迭代器.

  - *既然已经有了安全迭代器,那为什么还要有一个非安全迭代器呢?*
    - 原因: 这个跟多进程内存cow机制有关. 
      - 为了支持安全迭代器可以暂停当前字典的渐进式数据迁移过程,内部实现是在当前字典dict句柄里维护一个安全迭代器计数器,在第一次使用迭代器遍历时,对此计数器+1,这样在调用字典的增删改查接口时判断此计数器是否为0以决定是否暂停数据迁移.
      - 在释放安全迭代器时,又会对当前字典dict句柄里的安全迭代器计数器-1,以允许数据迁移.
      - 上述过程, 本质上是对字典dict结构体一个元素的修改.
      - 而redis在进行内存数据rdb持久化 或者 aof-rewrite时,是通过fork子进程, 由子进程将自身"静止的"内存数据进行持久化,\*inux系统为提升fork性能,普遍采用内存cow写时拷贝机制,即fork出的子进程共享父进程的内存空间,只有当出现修改此共享内存空间时,才会拷贝出相关涉及到的内存页,对其进行修改.
    - 基于上述情况, 如果在子进程的数据持久化时使用安全迭代器,必然会导致dict里的计数器字段改动,进行导致不必要的内存页cow.
    - 所以对于只读式的遍历场景,可以使用非安全迭代器,以避免不必要的内存写时拷贝.
   
![dict](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/dict.png)  

### 1.4 intset整型集合
1.4.1**intset实现说明**
- *intset里的载体数据是以升序存储;*
- *intset结构体与载体数据在内存里均为little-endian编码方式,在使用时会根据系统情况进行适配转换;*
- *intset结构体与contents内存为一整体,一同开辟与释放;*
- *contents字节数组的内存大小 = length * encoding;*
- *插入数据时,优先根据新数的编码决定是否需要将现有的intset编码进行升级;*
- *插入一个新数据时,内存空间realloc为((length+1) * 新encoding + sizeof(struct intset)),如果涉及编码升级则将contents旧数据逐个遍历设置到新空间合适的位置进而完成每个元素的编码升级;*
- *删除数据时不会进行编码方式调整,但是会重新realloc内存((length+1) * encoding + sizeof(struct intset);*
- *编码方式只升不降,因为如要判断编码降级需要遍历现有成员引入性能问题;*

![intset](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/intset.png)  

### 1.5 skiplit跳表
1.5.1**跳表结构体里的span字段的用途说明**
- *本node 与 forward-node之间横跨的节点数目,数目的计算区间(本node, forward-node]前开后闭*
- *span字段主要是支持sortedset类型的按照rank获取数据功能*

![skiplist_node](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/skiplist_node.png)

![skiplist](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/skiplist.png)  

### 1.6 ziplit压缩链表
1.6.1**特别注意**
- *本结构体里记录的数据除非特别说明,则默认为little-endian编码方式*

![ziplist](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/ziplist.png)

### 1.7 zipmap压缩map
1.7.1**特别注意**
- *采用little-endian编码方式存储数据;*
- *zmlen只有一个byte长度,可以表示[0, 0XFD], 但如果数值为0XFE, 那么就需要遍历整个zipmap来计算出总长度;*
- *\<free\> is always an unsigned 8 bit number, because if after an update operation there are more than a few free bytes, the zipmap will be reallocated to make sure it is as small as possible.*

![zipmap](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/zipmap.png)
