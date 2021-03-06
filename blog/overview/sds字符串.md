- [1. sds是什么](#1-sds是什么)
- [2. sds如何使用](#2-sds如何使用)
- [3. sds如何提升性能](#3-sds如何提升性能)
- [4. sds如何节省内存](#4-sds如何节省内存)
---

字符串，在任何编程语言里都是最经常使用的基础类型，因此其性能表现至为重要。C语言并没有原生的string类型，通常都是以字符数组的形式来处理，同时基础库里提供了很多面向字符指针的库函数，对字符串进行拼接、比较、获取字符串长度等操作。

Redis内部存在大量的字符串操作，要兼顾性能与较少的内存使用量，同时还需要能够存储text与binary两种格式的字符串。Redis作者设计了sds这一自有字符串类型，并为此指定一组“编码”格式，以兼顾性能与空间。

下面介绍一下sds的设计思想

# 1. sds是什么

如下是Redis中对sds的定义
```c
  typedef char *sds
```
  
这sds不就是一个字符指针吗？跟C里的字符串指针使用方法一样嘛？！
  
没关系，我们继续往下看一个结构体
```c
  struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; /* used *///buf中已经使用的字节数，不包含null
    uint8_t alloc; /* excluding the header and null terminator *///实际可存数据的容量，不包含当前结构体+null
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
  };

  sds s = ((struct sdshdr8 *)new_mem)->buf;
```
这个结构体稍微有一些特别用法，我们这里可以简单说明一下
  
- 结构体开头的`__attribute__ ((__packed__))`表示此结构体以紧密格式存储。
  
  这里稍微展开一下，底层系统在加载内存数据时有自己特别的偏好：从固定的偏移地址加载数据，这个有个专用术语：内存对齐，每个基础类型不尽相同。结构体整体是占一整块连续的堆内存/栈内存，结构体内部又是由不同类型组成，而每个基础类型又有自己喜好的偏移地址，这导致结构体整体也表现出有自己喜好的偏移地址。内存对齐的结构体对硬件加载更加友好，但是内存对齐必然会浪费一些内存空间。

  Redis本身是将全量数据均存在内存里，所以对内存消耗量非常敏感，对内存优化方面做了很多细致的工作。这里Redis作者做了取舍，放弃了sds的结构体内存对齐，采用的是紧密相接的方式，以节省内存。

- 结构体最后一个成员`char buf[]`，怎么是长度为0的数组？

  其实这个方式在gnu领域经常见到，一般在处理可变长度的数据时，用于衔接头部定义跟载体数据使用<span style="color:red;">[1]</span>。一句话概况一下：该成员不占用任何的结构体空间，却可以使用该成员。

整体来看，该结构体`struct sdshdr8`是一个头部定义，以`flags`编码，之后跟着载体数据，载体数据已用`len`个字节，载体总容量为`alloc`个字节，sds直接指向载体数据地址。这其中正因为有`len`字段的存在，sds才可以自由的处理text与binary。

# 2. sds如何使用
  
从上述介绍可以看到，根据sds地址，我们可以快速的进行如下操作：

- sds对应结构体首地址

  ```c
    sds s；
    struct sdshdr8 *head = s - sizeof(struct sdshdr8);
  ```

- sds长度

  ```c
    sds s;
    uint32_t sds_len = (s - sizeof(struct sdshdr8))->len;
  ```

- sds拼接
  
  如果载体数据可用空间`alloc - len`足够，则直接将新数据拷贝即可，不用另行开辟内存空间。

  如果可用空间不足，那么以`2*(len + new_len)`重新`realloc()`一块新内存，以应对后续可能的再次拼接，减少频繁的内存开辟。

- 比较字符串
  
  对于sds的比较直接使用`memcmp()`函数，以避免对text/binary格式的依赖。

  这里顺带提一下查找字符串这一动作，Redis作者在项目中未提供基于sds的查找函数，并在编码里极力避免出现对sds的查找逻辑。只有在对原生`char *`字符串指针操作时，才会使用`strstr()/strchr()`家族函数。
  
# 3. sds如何提升性能

上述已经介绍过，sds对应的头部结构体内自带一个成员变量`len`，记录sds载体数据长度。这样可以避免像`strlen()`函数每次遍历数据才能得到总长度。

对于拼接过程，sds通过预留适当的尾部空间，以减少重复开辟内存的次数，提升了拼接的性能。当然，从节省内存的角度来看，sds这里会牺牲一些内存，但是在字符串有频繁拼接的应用场景下，这种以空间换时间的方案是保性能的优秀策略。

# 4. sds如何节省内存

回看sds类型可知
```
sds字符串 = struct头部定义 + 载体数据
```

既然sds为保证拼接的性能，开辟内存时总会预留一些富余空间，载体数据是没有方式做内存节省，那么只能从sds的头部定义入手。

Redis作者主要是针对`len`跟`alloc`两个成员变量的类型入手，根据载体数据的长度，对struct头部定义进行了不同的编码定义，共定义了`sdshdr5`, `sdshdr8`, `sdshdr16`, `sdshdr32`, `sdshdr64`这几种结构体，并封装了编码升级/降级方法，以便根据所存载体数据长度选择合适的编码方式。
  
```c
  //以sdshdr16举例
  struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */ //buf中已经使用的字节数，不包含null
    uint16_t alloc; /* excluding the header and null terminator *///实际可存数据的容量，不包含当前结构体+null
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
  };
```

最后以一份sds编码类别图收尾。

![sds](https://raw.githubusercontent.com/wapthen/redis-study/master/picture/sds.png) 


***参考资料***
1. https://gcc.gnu.org/onlinedocs/gcc/Zero-Length.html


