/*
 * Copyright (c) 2007 Bobby Bingham
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * video crop filter
 */

#include <stdio.h>
//添加一个json解析的，使用cjson这个开源库
#include "cJSON.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"


static const char *const var_names[] = {
    "in_w", "iw",   ///< width  of the input video
    "in_h", "ih",   ///< height of the input video
    "out_w", "ow",  ///< width  of the cropped video
    "out_h", "oh",  ///< height of the cropped video
    "a",
    "sar",
    "dar",
    "hsub",
    "vsub",
    "x",
    "y",
    "n",            ///< number of frame
    "pos",          ///< position in the file
    "t",            ///< timestamp expressed in seconds
    NULL
};

enum var_name {
    VAR_IN_W,  VAR_IW,
    VAR_IN_H,  VAR_IH,
    VAR_OUT_W, VAR_OW,
    VAR_OUT_H, VAR_OH,
    VAR_A,
    VAR_SAR,
    VAR_DAR,
    VAR_HSUB,
    VAR_VSUB,
    VAR_X,
    VAR_Y,
    VAR_N,
    VAR_POS,
    VAR_T,
    VAR_VARS_NB
};

//代表矩形框左上右下对角线坐标
typedef struct __tagBoxPoint{
	int nX1;
	int nY1;
	int nX2;
	int nY2;
	int nW;
	int nH;
}BoxPoint;

//从json文件中获取的识别帧信息内容
typedef struct __tagIdentfFrameInfo {
	int nFrameIndex;
	char*  strIdentfName;
	float   fPercentage;
	BoxPoint struBoxPnt;
}IdentfFrameInfo;

typedef struct DynamicCropContext {
    const AVClass *class;
    int  x;             ///< x offset of the non-cropped area with respect to the input area
    int  y;             ///< y offset of the non-cropped area with respect to the input area
    int  w;             ///< width of the cropped area
    int  h;             ///< height of the cropped area

    AVRational out_sar; ///< output sample aspect ratio
    int keep_aspect;    ///< keep display aspect ratio when cropping
    int exact;          ///< exact cropping, for subsampled formats

    int max_step[4];    ///< max pixel step for each plane, expressed as a number of bytes
    int hsub, vsub;     ///< chroma subsampling
    char *x_expr, *y_expr, *w_expr, *h_expr;
    AVExpr *x_pexpr, *y_pexpr;  /* parsed expressions for x and y */
    double var_values[VAR_VARS_NB];
	char *json_file;//记录用户传递进来的用于动态裁剪分析的json数据文件
	IdentfFrameInfo* identfFrame;
	int  identfFrameSize;
	long frameIndex;
	struct SwsContext *sws;
} DynamicCropContext;

//解析json数据文件格式
static int parse_json_file(DynamicCropContext* dcctx,char* file_name){
	int ret=-1;
	FILE *f=NULL; 
	long data_len=0; 
	char *json_data=NULL;
	cJSON *json_root=NULL;
	
	f = fopen(file_name, "rb"); 
	if(f==NULL){
		 av_log(dcctx, AV_LOG_ERROR,"open json file %s failed.\n",file_name);
		 return -1;
	}
	fseek(f, 0, SEEK_END); 
	data_len = ftell(f); 
	fseek(f, 0, SEEK_SET);	
	json_data = (char*)malloc(data_len + 1); 
	if(json_data==NULL){
		av_log(dcctx, AV_LOG_ERROR,"malloc json data size:%ld failed.\n",data_len);
		goto END;
	}
	fread(json_data, 1, data_len, f);
	//读完提前关闭句柄
	fclose(f);
	f=NULL;
	//开始解析数据
	do{
		json_root=cJSON_Parse(json_data);
		if(json_root==NULL){
			av_log(dcctx, AV_LOG_ERROR,"json data parse failed.\n");
			break;
		}
		/*char* out = cJSON_Print(json_root);
		if(out){
			free(out);
			out=NULL;
		}*/

		if(cJSON_IsArray(json_root)&&cJSON_GetArraySize(json_root)>0){
			int root_size=cJSON_GetArraySize(json_root);
			if(root_size>0){
				dcctx->identfFrame=av_mallocz_array(root_size+1,sizeof(IdentfFrameInfo));
				if(!dcctx->identfFrame){
					av_log(dcctx, AV_LOG_ERROR,"json identframeinfo array malloc failed.\n");
					break;
				}
				dcctx->identfFrameSize=root_size;
				av_log(dcctx, AV_LOG_TRACE,"the json root array size:%d.\n",root_size);
			}
						
			for(int i=0;i<root_size;i++){
				cJSON *jsonFrame=cJSON_GetArrayItem(json_root,i);
				if(jsonFrame==NULL){
					continue;
				}
				IdentfFrameInfo *identfFrameInfo = &dcctx->identfFrame[i];
				identfFrameInfo->nFrameIndex=i;
				if(cJSON_IsObject(jsonFrame)&&
					cJSON_HasObjectItem(jsonFrame,"frame_index")&&
					cJSON_HasObjectItem(jsonFrame,"box")){
					cJSON* jsonFrameIndexObj = cJSON_GetObjectItem(jsonFrame,"frame_index");
					cJSON* jsonCropBox = cJSON_GetObjectItem(jsonFrame,"box");
					if(jsonFrameIndexObj&&jsonCropBox
						&& i == jsonFrameIndexObj->valueint){
						//读取矩形框对角线坐标
						identfFrameInfo->struBoxPnt.nW = cJSON_GetArrayItem(jsonCropBox,0)->valueint;
						identfFrameInfo->struBoxPnt.nH = cJSON_GetArrayItem(jsonCropBox,1)->valueint;
						identfFrameInfo->struBoxPnt.nX1 = cJSON_GetArrayItem(jsonCropBox,2)->valueint;
						identfFrameInfo->struBoxPnt.nY1 = cJSON_GetArrayItem(jsonCropBox,3)->valueint;
					}else{
						av_log(dcctx, AV_LOG_ERROR,"the json frame index:%d error.\n",i);
					}
				}				
			}
			ret=0;
		}else{
			av_log(dcctx, AV_LOG_ERROR,"json data root is not array.\n");
			break;
		}	
	}while(0);
	
	END:
	//释放资源	
	if(json_data){
		free(json_data);
		json_data=NULL;
	}
	if(f){
		fclose(f);
		f=NULL;
	}
	if(json_root){
		cJSON_Delete(json_root);
		json_root=NULL;
	}
	return ret;
}


static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    int fmt, ret;

    for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
        if (!(desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM)) &&
            !((desc->log2_chroma_w || desc->log2_chroma_h) && !(desc->flags & AV_PIX_FMT_FLAG_PLANAR)) &&
            (ret = ff_add_format(&formats, fmt)) < 0)
            return ret;
    }

    return ff_set_common_formats(ctx, formats);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DynamicCropContext *s = ctx->priv;

    av_expr_free(s->x_pexpr);
    s->x_pexpr = NULL;
    av_expr_free(s->y_pexpr);
    s->y_pexpr = NULL;
	//释放资源
	if(s->identfFrame){
		for(int i=0;i<s->identfFrameSize;i++){
				IdentfFrameInfo *identfFrameInfo = &s->identfFrame[i];
				if(identfFrameInfo&&identfFrameInfo->strIdentfName){
					free(identfFrameInfo->strIdentfName);
					identfFrameInfo->strIdentfName=NULL;
				}
		}
		free(s->identfFrame);
	}
	if(s->json_file){
		av_free(s->json_file);
		s->json_file=NULL;
	}
}

static inline int normalize_double(int *n, double d)
{
    int ret = 0;

    if (isnan(d)) {
        ret = AVERROR(EINVAL);
    } else if (d > INT_MAX || d < INT_MIN) {
        *n = d > INT_MAX ? INT_MAX : INT_MIN;
        ret = AVERROR(EINVAL);
    } else
        *n = lrint(d);

    return ret;
}

static int config_input(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    DynamicCropContext *s = ctx->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(link->format);
    int ret;
    const char *expr;
    double res;

    s->var_values[VAR_IN_W]  = s->var_values[VAR_IW] = ctx->inputs[0]->w;
    s->var_values[VAR_IN_H]  = s->var_values[VAR_IH] = ctx->inputs[0]->h;
    s->var_values[VAR_A]     = (float) link->w / link->h;
    s->var_values[VAR_SAR]   = link->sample_aspect_ratio.num ? av_q2d(link->sample_aspect_ratio) : 1;
    s->var_values[VAR_DAR]   = s->var_values[VAR_A] * s->var_values[VAR_SAR];
    s->var_values[VAR_HSUB]  = 1<<pix_desc->log2_chroma_w;
    s->var_values[VAR_VSUB]  = 1<<pix_desc->log2_chroma_h;
    s->var_values[VAR_X]     = NAN;
    s->var_values[VAR_Y]     = NAN;
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = NAN;
    s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = NAN;
    s->var_values[VAR_N]     = 0;
    s->var_values[VAR_T]     = NAN;
    s->var_values[VAR_POS]   = NAN;

    av_image_fill_max_pixsteps(s->max_step, NULL, pix_desc);
    s->hsub = pix_desc->log2_chroma_w;
    s->vsub = pix_desc->log2_chroma_h;

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail_expr;
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->h_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail_expr;
    s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = res;
    /* evaluate again ow as it may depend on oh */
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail_expr;

    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = res;
    if (normalize_double(&s->w, s->var_values[VAR_OUT_W]) < 0 ||
        normalize_double(&s->h, s->var_values[VAR_OUT_H]) < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Too big value or invalid expression for out_w/ow or out_h/oh. "
               "Maybe the expression for out_w:'%s' or for out_h:'%s' is self-referencing.\n",
               s->w_expr, s->h_expr);
        return AVERROR(EINVAL);
    }

    if (!s->exact) {
        s->w &= ~((1 << s->hsub) - 1);
        s->h &= ~((1 << s->vsub) - 1);
    }

    av_expr_free(s->x_pexpr);
    av_expr_free(s->y_pexpr);
    s->x_pexpr = s->y_pexpr = NULL;
    if ((ret = av_expr_parse(&s->x_pexpr, s->x_expr, var_names,
                             NULL, NULL, NULL, NULL, 0, ctx)) < 0 ||
        (ret = av_expr_parse(&s->y_pexpr, s->y_expr, var_names,
                             NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        return AVERROR(EINVAL);

    if (s->keep_aspect) {
        AVRational dar = av_mul_q(link->sample_aspect_ratio,
                                  (AVRational){ link->w, link->h });
        av_reduce(&s->out_sar.num, &s->out_sar.den,
                  dar.num * s->h, dar.den * s->w, INT_MAX);
    } else
        s->out_sar = link->sample_aspect_ratio;

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d sar:%d/%d -> w:%d h:%d sar:%d/%d\n",
           link->w, link->h, link->sample_aspect_ratio.num, link->sample_aspect_ratio.den,
           s->w, s->h, s->out_sar.num, s->out_sar.den);

    if (s->w <= 0 || s->h <= 0 ||
        s->w > link->w || s->h > link->h) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid too big or non positive size for width '%d' or height '%d'\n",
               s->w, s->h);
        return AVERROR(EINVAL);
    }

    /* set default, required in the case the first computed value for x/y is NAN */
    s->x = (link->w - s->w) / 2;
    s->y = (link->h - s->h) / 2;
    if (!s->exact) {
        s->x &= ~((1 << s->hsub) - 1);
        s->y &= ~((1 << s->vsub) - 1);
    }
	//这里通过解析json文件的数据，获取数据信息
	if(s->json_file){
		if(parse_json_file(s,s->json_file)<0){
			goto fail_expr;
		}
	}
	
    return 0;

fail_expr:
    av_log(NULL, AV_LOG_ERROR, "Error when evaluating the expression '%s'\n", expr);
    return ret;
}

static int config_output(AVFilterLink *link)
{
    DynamicCropContext *s = link->src->priv;

    link->w = s->w;
    link->h = s->h;
    link->sample_aspect_ratio = s->out_sar;

    return 0;
}

static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    DynamicCropContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(link->format);
    int i=0;
	AVFrame *out=NULL;
	int out_w=0,out_h=0;
	int crop_w=0,crop_h=0;
	int crop_x=0,crop_y=0;
	int ret=0;

	//初始化
	out_w = s->w;
	out_h = s->h;
    crop_w  = s->w;
    crop_h  = s->h;
	crop_x = s->x;
	crop_y = s->y;
	
	//从解析到的数据结构中取对应帧数据的起始位置
	if(s->identfFrame&&s->identfFrameSize>0){
		IdentfFrameInfo *identfFrameInfo = &s->identfFrame[s->frameIndex];
		if(identfFrameInfo){
			crop_x = identfFrameInfo->struBoxPnt.nX1;
			crop_y = identfFrameInfo->struBoxPnt.nY1;
			crop_w = identfFrameInfo->struBoxPnt.nW;
			crop_h = identfFrameInfo->struBoxPnt.nH;
			s->var_values[VAR_X]=crop_x;
   			s->var_values[VAR_Y]=crop_y;			
			av_log(ctx, AV_LOG_TRACE,"the identf frame info:x:%d,y:%d,w=%d,h=%d,percent:%f,name:%s",
				crop_x,crop_y,crop_w,crop_h,identfFrameInfo->fPercentage,identfFrameInfo->strIdentfName);
		}	
	}else{
		s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
		s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, NULL);
		/* It is necessary if x is expressed from y	*/
		s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
		
	}
	s->frameIndex++;
	
    s->var_values[VAR_N] = link->frame_count_out;
    s->var_values[VAR_T] = frame->pts == AV_NOPTS_VALUE ?NAN : frame->pts * av_q2d(link->time_base);
    s->var_values[VAR_POS] = frame->pkt_pos == -1 ?NAN : frame->pkt_pos;
  

    normalize_double(&crop_x, s->var_values[VAR_X]);
    normalize_double(&crop_y, s->var_values[VAR_Y]);

    if (crop_x < 0)
        crop_x = 0;
    if (crop_y < 0)
        crop_y = 0;
    if ((unsigned)crop_x + (unsigned)crop_w > link->w)
        crop_x = link->w - crop_w;
    if ((unsigned)crop_y + (unsigned)crop_h > link->h)
        crop_y = link->h - crop_h;
    if (!s->exact) {
        crop_x &= ~((1 << s->hsub) - 1);
        crop_x &= ~((1 << s->vsub) - 1);
    }
	s->x = crop_x;
	s->y = crop_y;

    av_log(ctx, AV_LOG_TRACE, "n:%d t:%f pos:%f x:%d y:%d x+w:%d y+h:%d\n",
            (int)s->var_values[VAR_N], s->var_values[VAR_T], s->var_values[VAR_POS],
            crop_x, crop_y, crop_x+crop_w, crop_y+crop_h);
	//添加一个打印语句
	 av_log(ctx, AV_LOG_TRACE, "the frame display num:%d,pts:%f,fmt:%d\n",
            frame->display_picture_number,frame->pts == AV_NOPTS_VALUE ?NAN : frame->pts * av_q2d(link->time_base),frame->format);

    frame->data[0] += crop_y * frame->linesize[0];
    frame->data[0] += crop_x * s->max_step[0];

    if (!(desc->flags & AV_PIX_FMT_FLAG_PAL || desc->flags & FF_PSEUDOPAL)) {
        for (i = 1; i < 3; i ++) {
            if (frame->data[i]) {
                frame->data[i] += (crop_y >> s->vsub) * frame->linesize[i];
                frame->data[i] += (crop_x * s->max_step[i]) >> s->hsub;
            }
        }
    }

    /* alpha plane */
    if (frame->data[3]) {
        frame->data[3] += crop_y * frame->linesize[3];
        frame->data[3] += crop_x * s->max_step[3];
    }
	av_log(ctx, AV_LOG_TRACE, "src:w:%d,h:%d,link:w:%d,h:%d,crop:w:%d,h:%d,out:w:%d,h:%d\n",
		frame->width,frame->height,link->w,link->h,crop_w,crop_h,out_w,out_h);
	//更改一下图像的宽高
	frame->width=crop_w;
	frame->height=crop_h;
	
	//裁剪完毕以后，需要加入scale滤镜处理
	//先需要分配一帧输出的数据缓存
	out = ff_get_video_buffer(link, out_w, out_h);
    if (!out) {
        ret = AVERROR(ENOMEM);
        return ret;
    }
	s->sws = sws_alloc_context();
    if (!s->sws) {
		av_log(ctx, AV_LOG_ERROR, "alloc sws context failed\n");
        ret = AVERROR(ENOMEM);
        goto error;
    }
	av_opt_set_int(s->sws, "srcw", frame->width, 0);
    av_opt_set_int(s->sws, "srch", frame->height, 0);
    av_opt_set_int(s->sws, "src_format", frame->format, 0);
    av_opt_set_int(s->sws, "dstw", out_w, 0);
    av_opt_set_int(s->sws, "dsth", out_h, 0);
    av_opt_set_int(s->sws, "dst_format", frame->format, 0);
    av_opt_set_int(s->sws, "sws_flags", SWS_BICUBIC, 0);

    if ((ret = sws_init_context(s->sws, NULL, NULL)) < 0)
        goto error;

    sws_scale(s->sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, out->data, out->linesize);
	//释放资源
	sws_freeContext(s->sws);
	s->sws = NULL;
	//在释放帧数据信息之前，我们需要拷贝一下之前的数据信息
	out->pts = frame->pts;
	out->pkt_dts = frame->pkt_dts;
	av_frame_free(&frame);
	frame=NULL;
	//更新一下
	frame=out;
	av_log(ctx, AV_LOG_TRACE, "out frame:w:%d,h:%d,pts:%lld,dts:%lld\n",frame->width,frame->height,frame->pts,frame->pkt_dts);

    return ff_filter_frame(link->dst->outputs[0], frame);
error:
	av_log(ctx, AV_LOG_ERROR, " error:%d\n",ret);
	if(out){
		av_frame_free(&out);
		out=NULL;
	}
	return ret;	
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    DynamicCropContext *s = ctx->priv;
    int ret;

    if (   !strcmp(cmd, "out_w")  || !strcmp(cmd, "w")
        || !strcmp(cmd, "out_h")  || !strcmp(cmd, "h")
        || !strcmp(cmd, "x")      || !strcmp(cmd, "y")) {

        int old_x = s->x;
        int old_y = s->y;
        int old_w = s->w;
        int old_h = s->h;

        AVFilterLink *outlink = ctx->outputs[0];
        AVFilterLink *inlink  = ctx->inputs[0];

        av_opt_set(s, cmd, args, 0);

        if ((ret = config_input(inlink)) < 0) {
            s->x = old_x;
            s->y = old_y;
            s->w = old_w;
            s->h = old_h;
            return ret;
        }

        ret = config_output(outlink);

    } else
        ret = AVERROR(ENOSYS);

    return ret;
}

#define OFFSET(x) offsetof(DynamicCropContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption crop_dynamic_options[] = {
    { "out_w",       "set the width crop area expression",   OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "w",           "set the width crop area expression",   OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_h",       "set the height crop area expression",  OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "h",           "set the height crop area expression",  OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "x",           "set the x crop area expression",       OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "(in_w-out_w)/2"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "y",           "set the y crop area expression",       OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "(in_h-out_h)/2"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "keep_aspect", "keep aspect ratio",                    OFFSET(keep_aspect), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    { "exact",       "do exact cropping",                    OFFSET(exact),  AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },   
	{ "json_file",   "path to the crop dynamic json file",   OFFSET(json_file), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(crop_dynamic);

static const AVFilterPad avfilter_vf_crop_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_crop_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_vf_crop_dynamic = {
    .name            = "crop_dynamic",
    .description     = NULL_IF_CONFIG_SMALL("Crop the input video with dynamic area for every frame."),
    .priv_size       = sizeof(DynamicCropContext),
    .priv_class      = &crop_dynamic_class,
    .query_formats   = query_formats,
    .uninit          = uninit,
    .inputs          = avfilter_vf_crop_inputs,
    .outputs         = avfilter_vf_crop_outputs,
    .process_command = process_command,
};
