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
### dict字典
### intset整型集合
