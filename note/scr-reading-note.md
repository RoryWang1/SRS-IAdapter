1. RTMP
    实际应用过程中，RTMP协议的整体工作逻辑如下：
    1、软硬件编码器以帧为单位生成音视频数据流
    2、RTMP协议先将每一帧数据封装成一个Message数据包
    3、再根据本端设置的trunk size（默认128字节）对Message数据包分片封装成trunk数据包，最终通过TCP协议实现网络传输。
    4、反之，RTMP协议将接收的TCP数据包以chunk为单位进行组装，将多个chunk包还原成一个Message数据包，最终上层协议处理Message数据包。
RTMP协议向上的处理逻辑是总是基于Message报文，向下的处理逻辑则基于Chunk报文，所以，接下来首先分析用户层Message报文的各种含义。

State Thread 协程，通过状态来阻塞线程。