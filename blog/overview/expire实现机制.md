- [1. 带有过期属性的key在Redis中是如何存储](#1-带有过期属性的key在redis中是如何存储)
- [2. 定时失效机制是如何实现](#2-定时失效机制是如何实现)
  - [2.1 过期移除](#21-过期移除)
  - [2.2 未过期移除](#22-未过期移除)
- [3. 特别的实现细节](#3-特别的实现细节)
---

Redis中有一个非常有用的功能，支持key的定时失效，特别适合热点数据缓存的业务，例如：至多缓存1分钟，1分钟后缓存里的数据自动清除，如有用户请求，则从MySQL数据库里查询一遍最新结果并缓存起来，同时避免了过多冷数据占用宝贵的缓存资源。

定时功能支持秒级、毫秒级，支持取消定时失效属性直接变为正常永久key，也支持逆向。

可以对任何Redis类型的key设置定时失效属性。

# 1. 带有过期属性的key在Redis中是如何存储

虽然对外接口中有若干种不同的expire功能，但是Redis作者内部实现时采用了统一方式。每一个db库结构体里，由主字典存储key-value数据，同时还有一个expire字典存储key-失效时刻，存储有定时失效属性的key。需注意的是内存里的失效时刻均为绝对时刻，以便于使用统一的机制管理不同的失效时刻。

# 2. 定时失效机制是如何实现

Redis的所有数据均会存于内存中，所以对于内存的消耗特别敏感。介于此，Redis对于删除有定时失效属性的key，整体而言属于积极删除，以尽早释放内存。

## 2.1 过期移除

  Redis为实现过期移除，采用了两种方式：被动方式与主动方式。

  * 被动过期移除方式

    这项机制类似于懒惰式移除，即在用户访问key时，首先执行检查该key是否过期的逻辑，如果当前服务器时刻已经超过expire字典里该key所存的绝对过期时刻，那么将此key从expire字典以及主字典中移除。

  * 主动过期移除方式

    被动过期移除方式特别适合高频热点key，而对于不常访问的key则不太适合，会导致无用数据占用过多内存问题。所以Redis在被动过期移除的同时，也实现了主动过期移除机制。
    
    主动过期移除机制会周期性执行，逐个扫描db库里的expire字典，随机抽取并检查是否超时，超时则进行移除。需注意Redis只在主进程里执行过期数据移除动作，即需要保证快速性，避免造成延缓后续其他业务的执行。

    主动过期移除机制目前支持两种模式，二者主要从启动时机以及运行耗时两个层面进行不同的考虑。

    + 快速主动过期移除模式
      
      入口地址：主进程的核心循环第一步，在处理TCP套接字以及定时任务之前，所以需要尽可能的快速执行。
      启动时机：1) 上一次主动过期移除动作，因过期数据过多未在指定时间内执行完毕的；2) 前后两次快速模式的执行间隔必须在2秒以上。上述2个条件均需满足才能开启快速模式。

      运行耗时：每轮至多运行1毫秒。

    + 慢速主动过期移除模式
  
      入口地址：主进程的定时任务里，相比较而言可以有比较富余的时间处理。
      运行耗时：每轮至多运行hz的25%毫秒。
    
    整体来看，慢速模式是一个日常策略，只有在紧急情况下投入机动部队：快速模式，以便更好的平衡`耗时与内存消耗`之间的关系。实现方式是内部有一个标记字段`timelimit_exit`，记录本轮移除是否因达到耗时上限而终止。如此值为1，那么下一轮在日常运行慢速移除模式之前，会启动快速移除模式。但是快速模式也不能过于频繁，前后需要间隔2秒以上，避免延迟处理其他关键任务。

## 2.2 未过期移除

  此策略其实是属于`淘汰策略`的一部分，即当Redis内存使用量已经达到配置上限时，开启淘汰策略，只在expire字典里淘汰数据时，就会将某些已经定时但是暂未过期的数据主动移除。这一部分细节会在`淘汰策略`文章中介绍。

# 3. 特别的实现细节

   主动过期移除方式里，Redis使用了`static`db库游标，这样每轮均会针对不同的db库检查并移除过期数据，保证从概率上而言每个db库的待处理过期数据规模差不多。

   移除过程是从每个db库的expire字典里随机抽取数据判断是否过期。expire字典的疏密程度会影响随机抽取的耗时，所以Redis只会在expire字典的已存占比>=1%时才会对本db库执行移除过程，否则跳过本db库。

   判断耗时是否达到上限值也有一个小技巧，既然是计算耗时，不可避免的需要获取当前时间戳计算差值，那么频繁的获取当前时间戳会影响性能，所以Redis这里借鉴了批处理的方式：每批次至多随机抽取20次，每16批次再计算耗时。