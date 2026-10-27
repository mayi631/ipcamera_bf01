本组件复制自同级 `rtsp` 仓库，源版本为 `ff7c7ac141e25444704b667b7dc82379651d2a45`。公开接口见 `include/rtsp.h`；内部 OSAL 和环形缓冲调用已适配本工程的 `cvi_osal`、`ringbuffer`。

应用的 `make ipcamera` 会自动构建 `lib/libcomp_rtsp.a` 和 `lib/libcomp_rtsp.so`。也可独立交叉编译：

```bash
make -C components/rtsp CROSS_COMPILE=arm-linux-gnueabihf-
```

独立 CMake 构建使用相同的依赖目录，依赖库须已按目标架构编译：

```bash
cmake -S components/rtsp -B components/rtsp/build -DGCC_CROSS_COMPILE=arm-linux-gnueabihf-
cmake --build components/rtsp/build
```

接收客户端默认开启，CMake 支持 `-DRTSP_ENABLE_RECV=OFF`。随库示例可在 Makefile 生成组件库后，用 `cmake -S components/rtsp/samples -B components/rtsp/samples/build -DGCC_CROSS_COMPILE=arm-linux-gnueabihf-` 配置，再执行 `cmake --build components/rtsp/samples/build`。
