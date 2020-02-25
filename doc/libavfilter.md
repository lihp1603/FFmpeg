##ffmpeg - libavfilter 学习笔记1

对于初学libavfilter的朋友，可以先参考这篇文章，他里面介绍的很详细，链接地址:https://blog.csdn.net/newchenxf/article/details/51364105 个人感觉这篇文章是对libavfilter不可多得的文章。

其中这个图很关键:

![img](https://img-blog.csdn.net/20160512160533458)

结合这个图的还有这几句话也很关键，可以帮助你后面阅读源码。

AVFilterLink是干嘛的？它是建立AVFilterContext之间的联系。所以，若有4个AVFilterContext，那就需要3个AVFilterLink。 
AVFilterLink的src指针，指向上一个AVFilterContext，dst指针，指向下一个AVFilterContext。 
AVFilterPad干嘛的？它用于AVFilterContext之间的callback（回调）。 
怎么个回调法？ 
很简单，第一个AVFilterContext的outputs[0]指针，指向第一个AVFilterLink，这个AVFilterLink的dst指针，指向第二个AVFilterContext。 
如果我在前一个AVFilterContext调用 
outputs[0]->dstpad->filter_frame(Frame* input_frame1), 那其实就意味着，第一个过滤器，可以把处理好的一个frame（名字为input_frame1），可以通过这个调用，传递给第二个过滤器的input_pads的filter_frame函数。而我们实现的vf_transform.c，就是我说的第二个过滤器，里面就实现了filter_frame().



在libavfilter中默认提供了buffer和buffersink滤镜，他们分别担当filter链路上的src,sink，用于链接其他的所有效果滤镜，滤镜的所有帧数据全部源自发起点buffer，最后目的去到sink中。

我们来简单描述一下这个大概的链路:

buffer(buffersrc.c)----->效果滤镜1------->效果滤镜2------>buffersink(buffersink.c)

你buffer滤镜的数据从哪里来的呢？肯定是解码以后送到这个滤镜来的。

那buffersink滤镜中的数据，最后又何去何从了呢？当然他可能被送编码器或者送显示了。

我们看一下源码中的流程。

这里以ffmpeg源码中的filtering_video.c来看，可能看的比较清晰，因为ffmpeg.c封装了一次，看起来没有那么明显。

```c
 while (ret >= 0) {
				//接收解码器吐出来的数据
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {                  
                    goto end;
                }
                frame->pts = frame->best_effort_timestamp;
				//对解码的帧数据，送入buffer滤镜
                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {                  
                    break;
                }

                /* pull filtered frames from the filtergraph */
                while (1) {
					//从buffersink获取帧数据，然后送显示或者送编码器都是可以的
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        goto end;
                    display_frame(filt_frame, buffersink_ctx->inputs[0]->time_base);
                    av_frame_unref(filt_frame);
                }
                av_frame_unref(frame);
            }
```

然后我们可以看下所有滤镜的入口buffer这个filter，其实他很简单，就是一个FIFO，主要是做缓冲数据用的。

接着跟踪一下上面的源码:

av_buffersrc_add_frame_internal:(这里我们只看两个最重要的函数)

```c
	//写入缓存队列中
    if ((ret = av_fifo_generic_write(s->fifo, &copy, sizeof(copy), NULL)) < 0) {
        if (refcounted)
            av_frame_move_ref(frame, copy);
        av_frame_free(&copy);
        return ret;
    }
	//调用自己的request_frame函数,
	//同时注意这里的link，调用的是buffer这个avfilterContext的output的link
    if ((ret = ctx->output_pads[0].request_frame(ctx->outputs[0])) < 0)
        return ret;
```

这个里面干了两件事:写队列，调用自己的request_frame函数。

接着看下:

```c
static int request_frame(AVFilterLink *link)
{
    BufferSourceContext *c = link->src->priv;
    AVFrame *frame;
	//从缓冲队列中读取帧数据
    av_fifo_generic_read(c->fifo, &frame, sizeof(frame), NULL);
	//开始调用filter_frame函数,接触第一个link，介于buffer这个avfiltercontext和下一个之间的link
    ret = ff_filter_frame(link, frame);
    return ret;
}
```

流程暂时先分析到第一步。

