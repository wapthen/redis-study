# dict字典里的安全迭代器与非安全迭代器分析

* 需要安全迭代器的原因

    - 场景:一边遍历元素，一边增加/删除字典里的元素。
    - 上述场景在字典处于渐进式数据迁移时，如不暂停数据迁移会导致遍历过程出现元素重复or丢失异常。
    - 在遍历时需要对字典进行增删的场景下只能使用安全迭代器。

* 既然已经有了安全迭代器，那为什么还要有一个非安全迭代器呢？
    - 原因： 这个跟多进程内存cow机制有关。

      为了支持安全迭代器可以暂停当前字典的渐进式数据迁移过程，内部实现是在当前字典dict句柄里维护一个安全迭代器计数器，在第一次使用迭代器遍历时，对此计数器+1，这样在调用字典的增删改查接口时判断此计数器是否为0以决定是否暂停数据迁移。

      在释放安全迭代器时，又会对当前字典dict句柄里的安全迭代器计数器-1，以允许数据迁移。
      上述过程，本质上是对字典dict结构体一个元素的修改。
      
      而Redis在进行内存数据rdb持久化 或者 aof-rewrite时，是通过`fork()`子进程，由子进程将自身"静止的"内存数据进行持久化，\*inux系统为提升`fork()`性能，普遍采用内存cow写时拷贝机制，即`fork()`出的子进程共享父进程的内存空间，只有当出现修改此共享内存空间时，才会拷贝出相关涉及到的内存页，对其进行修改。

    - 基于上述情况，如果在子进程的数据持久化时使用安全迭代器，必然会导致dict里的计数器字段改动,进行导致不必要的内存页cow。

    - 所以对于只读式的遍历场景，可以使用非安全迭代器，以避免不必要的内存写时拷贝。