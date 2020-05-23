# redis-study
## 介绍
* 本repo分析的redis版本为5.0.5版本
* 希望以图的形式勾画出redis的内存组织结构,异步数据处理以及集群管理方式.

* redis涉及到 数据结构/算法/网络编程/信号/多进程通信/多线程同步/文件锁/磁盘io等相关技术知识.
* 上述技术可以参考《APUE》《UNP》书籍中的详细讲解.

## 文章

1. [Redis用途](https://wapthen.github.io/2020/03/06/Redis%E7%94%A8%E9%80%94)
2. [高性能原因](https://wapthen.github.io/2020/03/05/Redis%E9%AB%98%E6%80%A7%E8%83%BD%E5%8E%9F%E5%9B%A0) 
3. [sds字符串](https://wapthen.github.io/2020/03/08/Redis-sds%E5%AD%97%E7%AC%A6%E4%B8%B2) 
4. [expire实现机制](https://wapthen.github.io/2020/03/09/Redis-expire%E5%AE%9E%E7%8E%B0%E6%9C%BA%E5%88%B6) 
5. [数据淘汰机制](https://wapthen.github.io/2020/03/13/Redis%E6%95%B0%E6%8D%AE%E6%B7%98%E6%B1%B0%E6%9C%BA%E5%88%B6) 
6. [文件锁](https://wapthen.github.io/2020/03/12/Redis%E6%96%87%E4%BB%B6%E9%94%81)
7. [数据持久化机制](https://wapthen.github.io/2020/03/11/Redis%E6%95%B0%E6%8D%AE%E6%8C%81%E4%B9%85%E5%8C%96%E6%9C%BA%E5%88%B6) 
8. [dict字典里的迭代器分析](https://wapthen.github.io/2020/03/07/Redis%E5%AD%97%E5%85%B8%E8%BF%AD%E4%BB%A3%E5%99%A8%E5%88%86%E6%9E%90) 
9. [事务实现机制](https://wapthen.github.io/2020/03/10/Redis%E4%BA%8B%E5%8A%A1%E5%AE%9E%E7%8E%B0%E6%9C%BA%E5%88%B6) 

---

## 基础数据结构

### adlist双向链表

![adlist](https://wapthen.github.io/assets/img/2020/adlist.png)  

### sds字符串

- sds类型其实是 char指针: typedef char \*sds ;其直接指向sdshdr句柄后部的载体数据;
- sds类型可以存储text or binary;
- sdshdr句柄采用内存压缩,非内存对齐模式,以压缩内存使用量;
- sdshdr句柄跟实际载体数据前后紧挨着,内存空间开辟时,句柄与载体数据一同开辟;
- sds = sdshdr句柄里的末尾成员char buf[], 注意此处为0长度数组;
- sds在开辟/设值时会统一在尾部追加一个/0, 但在sdshdr句柄里的任何字段均不会体现出该1个字节;

#### **SDS_TYPE_5说明**

- 此类型不会出现在创建 或者 扩容 sds阶段, 这两个阶段所用的最短类别为SDS_TYPE_8;
- 此类型只会出现在压缩sds阶段,即可以压缩为SDS_TYPE_5类别;

![sds5](https://wapthen.github.io/assets/img/2020/sds5.png)

![sds8](https://wapthen.github.io/assets/img/2020/sds8.png)

![sds16](https://wapthen.github.io/assets/img/2020/sds16.png)

![sds32](https://wapthen.github.io/assets/img/2020/sds32.png)

![sds64](https://wapthen.github.io/assets/img/2020/sds64.png)

### dict字典

  - 需要安全迭代器的原因
    - 场景:一边遍历一边增加/删除字典里的元素.
    - 上述场景在字典处于渐进式数据迁移时,如不暂停数据迁移会导致遍历过程出现元素重复or丢失异常.
    - 在遍历时需要对字典进行增删的场景下只能使用安全迭代器.

  - 既然已经有了安全迭代器,那为什么还要有一个非安全迭代器呢?
    - 原因: 这个跟多进程内存cow机制有关. 
      - 为了支持安全迭代器可以暂停当前字典的渐进式数据迁移过程,内部实现是在当前字典dict句柄里维护一个安全迭代器计数器,在第一次使用迭代器遍历时,对此计数器+1,这样在调用字典的增删改查接口时判断此计数器是否为0以决定是否暂停数据迁移.
      - 在释放安全迭代器时,又会对当前字典dict句柄里的安全迭代器计数器-1,以允许数据迁移.
      - 上述过程, 本质上是对字典dict结构体一个元素的修改.
      - 而redis在进行内存数据rdb持久化 或者 aof-rewrite时,是通过fork子进程, 由子进程将自身"静止的"内存数据进行持久化,\*inux系统为提升fork性能,普遍采用内存cow写时拷贝机制,即fork出的子进程共享父进程的内存空间,只有当出现修改此共享内存空间时,才会拷贝出相关涉及到的内存页,对其进行修改.
    - 基于上述情况, 如果在子进程的数据持久化时使用安全迭代器,必然会导致dict里的计数器字段改动,进行导致不必要的内存页cow.
    - 所以对于只读式的遍历场景,可以使用非安全迭代器,以避免不必要的内存写时拷贝.
   
![dict](https://wapthen.github.io/assets/img/2020/)  

### intset整型集合

- intset里的载体数据是以升序存储;
- intset结构体与载体数据在内存里均为little-endian编码方式,在使用时会根据系统情况进行适配转换;
- intset结构体与contents内存为一整体,一同开辟与释放;
- contents字节数组的内存大小 = length * encoding;
- 插入数据时,优先根据新数的编码决定是否需要将现有的intset编码进行升级;
- 插入一个新数据时,内存空间realloc为((length+1) * 新encoding + sizeof(struct intset)),如果涉及编码升级则将contents旧数据逐个遍历设置到新空间合适的位置进而完成每个元素的编码升级;
- 删除数据时不会进行编码方式调整,但是会重新realloc内存((length+1) * encoding + sizeof(struct intset);
- 编码方式只升不降,因为如要判断编码降级需要遍历现有成员引入性能问题;

![intset](https://wapthen.github.io/assets/img/2020/intset_unit.png)

![intset](https://wapthen.github.io/assets/img/2020/intset_upgrade.png)  

### skiplit跳表

- 本node 与 forward-node之间横跨的节点数目,数目的计算区间(本node, forward-node]前开后闭
- span字段主要是支持sortedset类型的按照rank获取数据功能

![skiplist_node](https://wapthen.github.io/assets/img/2020/skiplist_unit.png)

![skiplist](https://wapthen.github.io/assets/img/2020/skiplist_all.png)  

### ziplit压缩链表

- 本结构体里记录的数据除非特别说明,则默认为little-endian编码方式

![ziplist](https://wapthen.github.io/assets/img/2020/ziplist.png)

### zipmap压缩map

- 采用little-endian编码方式存储数据;
- zmlen只有一个byte长度,可以表示[0, 0XFD], 但如果数值为0XFE, 那么就需要遍历整个zipmap来计算出总长度;
- \<free\> is always an unsigned 8 bit number, because if after an update operation there are more than a few free bytes, the zipmap will be reallocated to make sure it is as small as possible.

![zipmap](https://wapthen.github.io/assets/img/2020/zipmap.png)
