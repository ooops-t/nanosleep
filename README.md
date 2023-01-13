# 实时延时 nanosleep 应用

## 编译

``` shell
$ arm-linux-gnueabihf-gcc clock_nanosleep.c -lpthread -D_GNU_SOURCE
```

## RC80 测试结果

``` shell
Min:     13 Act:      15 Avg:      16 Max:      27
# /dev/cpu_dma_latency set to 0us
```
