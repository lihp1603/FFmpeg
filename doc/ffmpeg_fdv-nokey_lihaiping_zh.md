### ffmpeg截图

#### 1. ffmpeg的源码编译

​	1. 安装编译环境: apt-get install build-essential (这里以ubuntu为例)

	2. 快速拉取项目源码: git clone https://github.com/lihp1603/FFmpeg.git --depth=1 dev-r4.1 
 	3. cd FFmpeg ,然后执行 configure --prefix=/usr/local --extra-cflags=-I/usr/local/include --extra-ldflags=-L/usr/local/lib --bindir=/usr/local/bin --disable-doc --disable-filter=gltransition   （这里的gltransition滤镜使用了gl的能力，这里暂时用不到的，可以先关闭掉，否则会出现依赖的问题。）
 	4. make && make install



####	2.  拉流截图

​	ffmpeg -threads 2 -fdv_nokey 1 -i rtmp://127.0.0.1/live/livestream -vf scale=w=1280:h=720 -f image2 -an -y -q:v 2 -vsync 0 -threads 2 /work/image/2020_04_23/70/%04d.jpg

​	其中fdv_nokey  参数指定为1 表示强制丢弃非关键帧(force discard video non-keyframes),

​								参数指定为0,-1的时候，表示不启用该功能

​	**注意1，如果这里设置启用了-fdv_nokey 1 功能，那么需要再输出编码参数设置-vsync 0，才能保证正确截图关键帧。**

​	这里说明一下fdv_nokey这个option，其实ffmpeg的官方源码中也有discard这个option，当设置-discard nokey的时候，和fdv-nokey的功能是一样的，但-discard nokey这个功能只针对部分文件格式有用(例如mp4文件可以使用)，而对rtmp流协议，并不支持。

-discard nokey和-fdv_nokey 1在实现上的区别是，前者在底层的read_packet中丢弃数据，后者是在av_read_frame后送入decode前，对packet数据包进行丢弃。



​	**注意2，暂时未对解码器部分做吐帧优化，可能需要连续送入几个关键帧后，才开始吐第一个关键帧的数据图，后续会持续优化一下这个部分。**



​	 

​	 

​	  

​	







