# redis-study
## 介绍
* 本repo分析的redis版本为5.0.5版本
* 希望以图的形式勾画出redis的内存组织结构,异步数据处理以及集群管理方式.

* redis涉及到 数据结构/算法/网络编程/信号/多进程通信/多线程同步/文件锁/磁盘io等相关技术知识.
* 上述技术可以参考《APUE》《UNP》书籍中的详细讲解.

## 基础数据结构
### adlist双向链表
![adlist](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/adlist.png)  
### sds字符串
**SDS字符串类型说明**
- *sds类型其实是 char指针: typedef char \*sds ;其直接指向sdshdr句柄后部的载体数据;*
- *sds类型可以存储text or binary;*
- *sdshdr句柄采用内存压缩,非内存对齐模式,以压缩内存使用量;*
- *sdshdr句柄跟实际载体数据前后紧挨着,内存空间开辟时,句柄与载体数据一同开辟;*
- *sds = sdshdr句柄里的末尾成员char buf[], 注意此处为0长度数组;*
- *sds在开辟/设值时会统一在尾部追加一个/0, 但在sdshdr句柄里的任何字段均不会体现出该1个字节;*

![adlist](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/sds.png)  
### dict字典

 **字典 安全迭代器 与 非安全迭代器 介绍**
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
   
![adlist](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/dict.png)  
### intset整型集合
![adlist](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/intset.png)  
