#ifndef __GEO_H__
#define __GEO_H__

#include "server.h"

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
// geo节点，存储经纬度+分数+成员数据
typedef struct geoPoint {
    double longitude;//经度
    double latitude;//纬度
    double dist;//距离
    double score;//经纬度geohash后的数值
    char *member;//该经纬度点上对应的预存数据
} geoPoint;

typedef struct geoArray {
    struct geoPoint *array;//数组指针
    size_t buckets;//容量
    size_t used;//已用个数
} geoArray;

#endif
