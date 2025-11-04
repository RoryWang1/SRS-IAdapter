# LSQUIC库集成指南

## 概述

本文档描述如何将lsquic库集成到SRS构建系统中，以支持QUIC协议。

## 步骤

### 1. 下载lsquic源码

```bash
cd srs-source/trunk/3rdparty
git clone https://github.com/litespeedtech/lsquic.git lsquic-fit
cd lsquic-fit
git checkout v4.0.0  # 或使用最新稳定版本
```

lsquic需要BoringSSL或OpenSSL作为依赖，SRS已经有OpenSSL，可以复用。

### 2. 修改构建系统

#### 2.1 在`auto/options.sh`中添加选项

```bash
SRS_QUIC=NO  # 默认禁用
```

#### 2.2 在`auto/depends.sh`中添加构建逻辑

```bash
#####################################################################################
# lsquic (QUIC库)
#####################################################################################
if [[ $SRS_QUIC == YES ]]; then
    if [[ -f ${SRS_OBJS}/${SRS_PLATFORM}/3rdparty/lsquic/lib/liblѕquic.a ]]; then
        rm -rf ${SRS_OBJS}/lsquic && cp -rf ${SRS_OBJS}/${SRS_PLATFORM}/3rdparty/lsquic ${SRS_OBJS}/ &&
        echo "lsquic is ok."
    else
        if [[ $SRS_USE_SYS_SSL != YES && ! -d ${SRS_OBJS}/openssl/lib ]]; then
            echo "OpenSSL not found, build lsquic failed."
            exit -1
        fi
        echo "Build lsquic" &&
        rm -rf ${SRS_OBJS}/${SRS_PLATFORM}/lsquic-fit ${SRS_OBJS}/${SRS_PLATFORM}/3rdparty/lsquic ${SRS_OBJS}/lsquic &&
        cp -rf ${SRS_WORKDIR}/3rdparty/lsquic-fit ${SRS_OBJS}/${SRS_PLATFORM}/ &&
        (
            cd ${SRS_OBJS}/${SRS_PLATFORM}/lsquic-fit &&
            cmake -DCMAKE_INSTALL_PREFIX=${SRS_DEPENDS_LIBS}/${SRS_PLATFORM}/3rdparty/lsquic \
                  -DLSQUIC_SHARED_LIB=0 \
                  -DLSQUIC_BIN=0 \
                  -DOPENSSL_ROOT_DIR=${SRS_DEPENDS_LIBS}/openssl \
                  .
        ) &&
        make -C ${SRS_OBJS}/${SRS_PLATFORM}/lsquic-fit ${SRS_JOBS} &&
        make -C ${SRS_OBJS}/${SRS_PLATFORM}/lsquic-fit install &&
        cp -rf ${SRS_OBJS}/${SRS_PLATFORM}/3rdparty/lsquic ${SRS_OBJS}/ &&
        echo "lsquic is ok."
    fi
    ret=$?; if [[ $ret -ne 0 ]]; then echo "Build lsquic failed, ret=$ret"; exit $ret; fi
fi
```

#### 2.3 在`auto/setup_variables.sh`中添加目录创建

在目录创建部分添加：
```bash
mkdir -p ${SRS_OBJS}/lsquic &&
mkdir -p ${SRS_OBJS}/${SRS_PLATFORM}/3rdparty/lsquic
```

#### 2.4 在`configure`中添加链接选项

参考SRT的集成方式，添加lsquic的链接选项。

## 3. 使用lsquic

在`quic_session_wrapper.cpp`中：

```cpp
#if SRS_QUIC_ENABLED
#include <lsquic.h>
// 实现真实的lsquic API调用
#endif
```

## 注意事项

1. lsquic需要C++11或更高版本，但SRS默认使用C++98，可能需要特殊处理
2. lsquic需要BoringSSL或OpenSSL，确保OpenSSL已正确构建
3. lsquic使用CMake构建系统，需要cmake工具

## 当前状态

目前实现了占位框架，编译通过。真实lsquic集成需要：
1. 下载lsquic源码
2. 修改构建系统
3. 实现真实的lsquic API调用

