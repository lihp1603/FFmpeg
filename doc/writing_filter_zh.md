本文的主要目的是梳理，记录自己在学习开发ffmpeg视频滤镜的笔记。参考的主要内容是根据ffmpeg中doc下的writing_filter.txt文件以及ffmpeg的源码。

author:lihaiping1603@aliyun.com

date:2019-12-19

## 1. Contex 定义自己私有的上下文结构

跳过头文件往下走，我们一般在滤镜的文件中会首先给出和定义一个自己的私有上下文结构体对象，例如FoobarContext。这个结构体对象他是我们存储我们后续所需要的所有全部内容，例如我们可以用它记录你本地的状态，自己需要用到的相关对象，变量，以及一些可变的用户option参数信息等内容。

这个上下文的结构对象，他的分配是由ffmpeg底层分配，同时它在分配以后，ffmpeg会自动的对他进行零值初始化，所以当我们第一次获取到他的时候，他的内容是已经被初始化为零值的状态。

可能你在阅读其他的滤镜代码的时候，你会注意到第一个字段“const AVClass *class”;假设你有一个上下文，这是你唯一需要保持的字段。而这个字段，它所具有的魔法，我们不需要做过多的关心，我们只需要将它放在contex内容中的第一个位置就OK了。

例如：

```c
typedef struct FoobarContext {
const AVClass *class;//这个地方我们需要放在第一个位置
int x,y;
}
```



## 2. Options 用户可访问的可选参数项数组内容

接下来介绍一下options，它主要记录操作滤镜的时候用户可访问的选项。

例如: -vf foobar=mode=colormix:high=0.4:low=0.1.

在大多数的options内容中会有以下的模式:

​	name, description, offset, type, default value, minimum value, maximum value, flags

这些参数的我们做个大概的介绍和使用：

​	-name 它是options的名字，一般我们使用简短宿写的小写就OK了

​	-description 它主要用于描述这个选项的作用是干啥的，例如：set the foo of the bar

​	-offset:  它主要是表示在我们本地的context结构体中它对应的变量字段的偏移值，通常我们需要定义OFFSET()宏来表示，这个选项的值，它会根据用户输入通过选项解析器解析以后使用该信息来填充这个字段。

​	-type:类型,它表示用户输入的类型参数，对应为任何在libavutil/opt.h中定义的AV_OPT_TYPE_*

​	-min,max: 定义可用值的一个可变范围

​	-flags：定义AVOption generic flags， See AV_OPT_FLAG_* definitions



如果对这个部分有什么疑惑的话，我们可以多看下其他滤镜中的定义，然后我们参考一下就会了。



## 3. Class 

AVFILTER_DEFINE_CLASS(foobar)将定义一个惟一的foobar_class，它具有引用选项等的某种签名，这些选项将在AVFilter的定义中引用。



## 4. Filter 滤镜定义

AVFilter，它主要定义filter包含的pads以及用于与filter交互的所有回调函数.

一般在文件的结尾部分，我们都将看到类似 foobar_inputs，foobar_outputs 和 AVFilter ff_vf_foobar 这些内容的对象定义。

### 4.1 Callbacks :Filter中的回调函数

#### 4.1.1 init()  

这个初始化函数是会被第一个调用到的。

顾名思义，init()是您最终初始化和分配缓冲区、预先计算数据等的地方。注意，此时，您的本地上下文已经初始化了用户选项，但是您仍然不知道将获得的数据类型，因此这个函数通常主要用于对用户选项采取一些措施的。

一些init()还将根据用户选项动态定义输入或输出的数量。一个很好的例子是分割过滤器split滤镜，但是我们不在这里介绍它，因为vf_foobar只是一个简单的1:1过滤器。



#### 4.1.2 uninit()

主要用于释放你分配的内存对象。

#### 4.1.3 query_formats()



#### 4.1.4 config_props()

这个回调不是必须的，但是您可能会有一个或多个config_props()。它不是对过滤器本身的回调，而是对其输入inputs或输出outputs的回调(在libavfilter的字典中，它们被称为“pad”——AVFilterPad).

在输入config_props()中，您可以了解在query_formats()之后选择了哪种像素格式，以及诸如视频宽度和高度等更多信息(inlink->{w,h})。如果你需要根据你的输入更新你的内部上下文状态你可以在这里做。在edgedetect中，可以看到这个回调用于根据这些信息分配缓冲区。它们将在uninit()中被销毁.

在输出config_props()中，可以定义希望在输出中更改的内容。通常，如果你的过滤器要加倍的大小的视频，你会更新outlink->w和outlink->h.



#### 4.1.5 filter_frame()

这个filter_frame回调函数:它是我们处理接收帧的地方。伴随着frame，我们还能获取到一个input link，而我们这个frame就是来自这个input link的。

函数原型： static int filter_frame(AVFilterLink *inlink, AVFrame *in) { ... }

我们可以从这个函数中的input link中获取到filter  context，例如这样：AVFilterContext *ctx = inlink->dst;

然后我们还可以获取到我们之前自己定义的私有上下文context结构对象，例如这样：FoobarContext *foobar = ctx->priv;

同时我们可以通过获取到的filter context对象，可以获取到out link对象，（例如这样：AVFilterLink *outlink = ctx->outputs[0];这里我们选择第一个输出对象。当然你也可以有多个输出对象，例如split滤镜。）他将告诉我们处理完这个frame以后，这个frame会被送到哪里。

如果你想定义一个简单的过滤器滤镜，它仅仅啥都不做，只是传递frame的话，我们可以直接这样干：

​	return ff_filter_frame(outlink, in);

当然，我们也可以改变这个frame的数据，当我们可能想要改变这个frame中的数据的时候。

这里需要注意一个概念，我们可以访问frame->data[]和frame->linesize[],但是frame的width和frame的linesize并不是完全匹配的，而且linesize>=width,一个很典型的应用就是frame有padding数据的时候，我们不应该改变padding的数据甚至read都是不应该的。通常，请记住，链中的前一个过滤器可能改变了frame的尺寸，但没有改变linesize大小.例如crop滤镜，它的linesize没有被改变，仅仅改变了width参数。

    <-------------- linesize ------------------------>
    +-------------------------------+----------------+ ^
    |                               |                | |
    |                               |                | |
    |           picture             |    padding     | | height
    |                               |                | |
    |                               |                | |
    +-------------------------------+----------------+ v
    <----------- width ------------->


还需要注意几个点，在修改输入in frame之前，我们需要先确定它(the frame)是否是可写的(writable)，或者我们通过获取一个新的frame。这个应用场景，需要我们自己判断。

假设您希望根据输入的多个像素(通常是周围的像素)更改一个像素。在这种情况下，您无法直接对输入frame进行就地处理，因此需要分配一个与输入frame具有相同属性的新帧frame，并将新帧frame发送给下一个filter.例如这样干：

```c
AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
if (!out) {
    av_frame_free(&in);
    return AVERROR(ENOMEM);
}
av_frame_copy_props(out, in);

// out->data[...] = foobar(in->data[...])

av_frame_free(&in);
return ff_filter_frame(outlink, out);
```
再来看一下直接对input frame处理的例子,

```c
av_frame_make_writable(in);
// in->data[...] = foobar(in->data[...])
return ff_filter_frame(outlink, in);
```
这个时候，你可能会有疑问，为什么会出现一个frame是不可写的？答案是假如前一个filter依然拥有这个frame数据，我们可以想象一个filter在你的filter之前，它需要缓存帧frame.这个时候，我们就不能改变这个frame，否则它会让前一个filter出现bug.  这就是av_frame_make_writable()的作用(如果frame已经是可写的，那么它不会有任何作用)。

使用av_frame_make_writable()的问题是，在最坏的情况下，它会在您使用filter再次更改整个输入frame之前复制它.如果frame不可写，av_frame_make_writable()将分配新的缓冲区，并复制这个输入input frame数据。您不希望这样，如果需要的话，您可以通过分配一个新的缓冲区来避免这种情况，并在您的过滤器filter中处理这种从in 到out的情况，从而保存memcpy。一般来说，这是按照以下方案进行的:


```c
int direct = 0;
AVFrame *out;
if (av_frame_is_writable(in)) {
    direct = 1;
    out = in;
} else {
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);
}

// out->data[...] = foobar(in->data[...])

if (!direct)
    av_frame_free(&in);
return ff_filter_frame(outlink, out);
```


#### 4.1.6 Threading  线程

libavfilter不支持frame帧线程，但是你可以添加slice threading 到你的过滤器filter中。

假如foobar filter具有如下的帧frame处理功能：

```c
dst = out->data[0];
src = in ->data[0];

for (y = 0; y < inlink->h; y++) {
    for (x = 0; x < inlink->w; x++)
        dst[x] = foobar(src[x]);
    dst += out->linesize[0];
    src += in ->linesize[0];
}
```
首先需要做的是将这个函数的处理过程转变为slice的片处理过程。那么新的代码将变成：

```c
for (y = slice_start; y < slice_end; y++) {
    for (x = 0; x < inlink->w; x++)
        dst[x] = foobar(src[x]);
    dst += out->linesize[0];
    src += in ->linesize[0];
}
```
其中源src和目的dst的指针，以及slice_start，slice_end将被根据jobs的数量来确定。通常，它看起来像是这样的：

```c
const int slice_start = (in->height *  jobnr   ) / nb_jobs;
const int slice_end   = (in->height * (jobnr+1)) / nb_jobs;
uint8_t       *dst = out->data[0] + slice_start * out->linesize[0];
const uint8_t *src =  in->data[0] + slice_start *  in->linesize[0];
```
这个新代码将被隔离在一个新的filter_slice()中： 

​	static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs) { ... }

注意，我们需要input frame和output frame来定义slice_{start,end}和dst/src，这在回调中是不可用的。它们将通过一个void* arg参数来传递。所以你必须定义一个包含所有你需要信息的结构，例如：

```c
typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;
```
如果你需要更多的信息从你自定义的私有context中获取，那么你也可以将他们放在这个结构体中。

于是在filter_sclice函数中，你就可以像这样访问它：const ThreadData *td = arg;

然后在filter_frame()回调中，需要使用类似这样的东西调用线程分发器：


```c
ThreadData td;
// ...
td.in  = in;
td.out = out;
ctx->internal->execute(ctx, filter_slice, &td, NULL, FFMIN(outlink->h, ctx->graph->nb_threads));

// ...

return ff_filter_frame(outlink, out);
```
最后一步是将AVFILTER_FLAG_SLICE_THREADS标记添加到AVFilter.flags

