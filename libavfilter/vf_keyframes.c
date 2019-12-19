/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2010 Baptiste Coudurier
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
 * overlay one video on top of another
 */

#include "avfilter.h"
#include "formats.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "internal.h"
#include "drawutils.h"
#include "framesync.h"
#include "video.h"
//添加一个json解析的，使用cjson这个开源库
#include "cJSON.h"

#include "libavutil/opt.h"
#include "libswscale/swscale.h"
#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/x86/cpu.h"
#include "filters.h"




//本文主要通过overlay和scale,crop滤镜功能进行组合，实现keyframes的动画功能
//其中keyframes中的frames参数通过json配置文件带入，其中keyframes的修改定义和修改并不涉及在本文件中，而上次的接口修改和定义主要通过golang实现
//因为keyframes的keyframe参数会根据应用场景时常动态改变，所以感觉放在此文件中实现，并不妥也不高效
//author:lihaiping1603@aliyun.com
//date-create:2019-12-16


//定义位置信息
typedef struct __tagPosition{
	int nX;
	int nY;
}Position;

//代表矩形框左上右下对角线坐标
typedef struct __tagCropBox{
	Position struLeftTop;//左上
	Position struRightDown;//右下	
	int nW;//宽高
	int nH;
}CropBox;


//从json文件中获取的识别帧信息内容
typedef struct __tagEachFrameInfo {
	int nFrameIndex;
	char*  strIdentfName;
	float   fPercentage;
	Position struOverlayPos;//overlay的位置坐标
	float dfScale;//宿放值
	CropBox struBoxPnt;//裁剪框
}EachFrameInfo;

static const char *const var_names[] = {
    "main_w",    "W", ///< width  of the main    video
    "main_h",    "H", ///< height of the main    video
    "overlay_w", "w", ///< width  of the overlay video
    "overlay_h", "h", ///< height of the overlay video
    "hsub",
    "vsub",
    "x",
    "y",
    "n",            ///< number of frame
    "pos",          ///< position in the file
    "t",            ///< timestamp expressed in seconds
    //新增
    "out_w",
    "out_h",
    NULL
};

enum var_name {
    VAR_MAIN_W,    VAR_MW,
    VAR_MAIN_H,    VAR_MH,
    VAR_OVERLAY_W, VAR_OW,
    VAR_OVERLAY_H, VAR_OH,
    VAR_HSUB,
    VAR_VSUB,
    VAR_X,
    VAR_Y,
    VAR_N,
    VAR_POS,
    VAR_T,
    //新增
    VAR_OUT_W,
    VAR_OUT_H,
    VAR_VARS_NB
};

enum OverlayFormat {
    OVERLAY_FORMAT_YUV420,
    OVERLAY_FORMAT_YUV422,
    OVERLAY_FORMAT_YUV444,
    OVERLAY_FORMAT_RGB,
    OVERLAY_FORMAT_GBRP,
    OVERLAY_FORMAT_AUTO,
    OVERLAY_FORMAT_NB
};

typedef struct KeyframesContext {
    const AVClass *class;
    int x, y;                   ///< position of overlaid picture

    uint8_t main_is_packed_rgb;
    uint8_t main_rgba_map[4];
    uint8_t main_has_alpha;
    uint8_t overlay_is_packed_rgb;
    uint8_t overlay_rgba_map[4];
    uint8_t overlay_has_alpha;
    int format;                 ///< OverlayFormat
    int alpha_format;
    int eval_mode;              ///< EvalMode

    FFFrameSync fs;

    int main_pix_step[4];       ///< steps per pixel for each plane of the main output
    int overlay_pix_step[4];    ///< steps per pixel for each plane of the overlay
    int hsub, vsub;             ///< chroma subsampling values
    const AVPixFmtDescriptor *main_desc; ///< format descriptor for main input

    double var_values[VAR_VARS_NB];
    char *x_expr, *y_expr;

    AVExpr *x_pexpr, *y_pexpr;

    int (*blend_row[4])(uint8_t *d, uint8_t *da, uint8_t *s, uint8_t *a, int w,
                        ptrdiff_t alinesize);
    int (*blend_slice)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);
	//新增变量和函数
	char *frames_file;//记录用户传递进来的用于动态裁剪分析的json数据文件
	char *outw_expr, *outh_expr;
	//AVExpr *outw_pexpr, *outh_pexpr;
	int  out_w; 			///< width of the out area
	int	 out_h;			   ///< height of the out area
	EachFrameInfo* pFrameInfo;
	int  nFrameSize;
	long llFrameIndex;
	struct SwsContext *pSwsCtx;
} KeyframesContext;

typedef struct ThreadData {
    AVFrame *dst, *src;
} ThreadData;

#define MAIN    0
#define OVERLAY 1

#define R 0
#define G 1
#define B 2
#define A 3

#define Y 0
#define U 1
#define V 2

enum EvalMode {
    EVAL_MODE_INIT,
    EVAL_MODE_FRAME,
    EVAL_MODE_NB
};

//声明几个函数
int ff_overlay_row_44_sse4(uint8_t *d, uint8_t *da, uint8_t *s, uint8_t *a,
                           int w, ptrdiff_t alinesize);

int ff_overlay_row_20_sse4(uint8_t *d, uint8_t *da, uint8_t *s, uint8_t *a,
                           int w, ptrdiff_t alinesize);

int ff_overlay_row_22_sse4(uint8_t *d, uint8_t *da, uint8_t *s, uint8_t *a,
                           int w, ptrdiff_t alinesize);

//新增
static av_cold void ff_keyframes_init_x86(KeyframesContext*s, int format, int pix_format,
                                 int alpha_format, int main_has_alpha)
{
    int cpu_flags = av_get_cpu_flags();

    if (EXTERNAL_SSE4(cpu_flags) &&
        (format == OVERLAY_FORMAT_YUV444 ||
         format == OVERLAY_FORMAT_GBRP) &&
        alpha_format == 0 && main_has_alpha == 0) {
        s->blend_row[0] = ff_overlay_row_44_sse4;
        s->blend_row[1] = ff_overlay_row_44_sse4;
        s->blend_row[2] = ff_overlay_row_44_sse4;
    }

    if (EXTERNAL_SSE4(cpu_flags) &&
        (pix_format == AV_PIX_FMT_YUV420P) &&
        (format == OVERLAY_FORMAT_YUV420) &&
        alpha_format == 0 && main_has_alpha == 0) {
        s->blend_row[0] = ff_overlay_row_44_sse4;
        s->blend_row[1] = ff_overlay_row_20_sse4;
        s->blend_row[2] = ff_overlay_row_20_sse4;
    }

    if (EXTERNAL_SSE4(cpu_flags) &&
        (format == OVERLAY_FORMAT_YUV422) &&
        alpha_format == 0 && main_has_alpha == 0) {
        s->blend_row[0] = ff_overlay_row_44_sse4;
        s->blend_row[1] = ff_overlay_row_22_sse4;
        s->blend_row[2] = ff_overlay_row_22_sse4;
    }
}

//增加几个=操作函数
static void PositionAssign(Position *pdstPos, Position srcPos){
	pdstPos->nX = srcPos.nX;
	pdstPos->nY = srcPos.nY;
	return ;
}

static void CropBoxAssign(CropBox* pdstBox, CropBox srcBox){
	PositionAssign(&pdstBox->struLeftTop,srcBox.struLeftTop);
	PositionAssign(&pdstBox->struRightDown,srcBox.struRightDown);
	pdstBox->nW = srcBox.nW;
	pdstBox->nH = srcBox.nH;
	return ;
}


//解析json数据文件格式
static int parse_json_file(KeyframesContext* dcctx,char* file_name){
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
				dcctx->pFrameInfo=av_mallocz_array(root_size+1,sizeof(EachFrameInfo));
				if(!dcctx->pFrameInfo){
					av_log(dcctx, AV_LOG_ERROR,"json eachframeinfo array malloc failed.\n");
					break;
				}
				dcctx->nFrameSize=root_size;
				av_log(dcctx, AV_LOG_TRACE,"the json root array size:%d.\n",root_size);
			}
						
			for(int i=0;i<root_size;i++){
				cJSON *jsonFrame=cJSON_GetArrayItem(json_root,i);
				if(jsonFrame==NULL){
					continue;
				}
				EachFrameInfo *pFrameInfo = &dcctx->pFrameInfo[i];
				pFrameInfo->nFrameIndex=i;
				if(cJSON_IsObject(jsonFrame)&&
					cJSON_HasObjectItem(jsonFrame,"frame_index")&&
					cJSON_HasObjectItem(jsonFrame,"crop_box")&&
					cJSON_HasObjectItem(jsonFrame,"scale")&&
					cJSON_HasObjectItem(jsonFrame,"overlay_position_x")&&
					cJSON_HasObjectItem(jsonFrame,"overlay_position_y")){
					cJSON* jsonFrameIndexObj = cJSON_GetObjectItem(jsonFrame,"frame_index");
					cJSON* jsonCropBox = cJSON_GetObjectItem(jsonFrame,"crop_box");
					cJSON* jsonScale = cJSON_GetObjectItem(jsonFrame,"scale");
					cJSON* jsonOverlayPosX = cJSON_GetObjectItem(jsonFrame,"overlay_position_x");
					cJSON* jsonOverlayPosY = cJSON_GetObjectItem(jsonFrame,"overlay_position_y");
					if(jsonFrameIndexObj&&jsonCropBox&&jsonScale&&jsonOverlayPosX&&jsonOverlayPosY
						&& i == jsonFrameIndexObj->valueint){
						//读取矩形框对角线坐标
						pFrameInfo->struBoxPnt.nW = cJSON_GetArrayItem(jsonCropBox,0)->valueint;
						pFrameInfo->struBoxPnt.nH = cJSON_GetArrayItem(jsonCropBox,1)->valueint;
						pFrameInfo->struBoxPnt.struLeftTop.nX = cJSON_GetArrayItem(jsonCropBox,2)->valueint;
						pFrameInfo->struBoxPnt.struLeftTop.nY = cJSON_GetArrayItem(jsonCropBox,3)->valueint;
						//宿放值
						pFrameInfo->dfScale = jsonScale->valuedouble;
						//overlay的位置坐标信息
						pFrameInfo->struOverlayPos.nX = jsonOverlayPosX->valueint;
						pFrameInfo->struOverlayPos.nY = jsonOverlayPosY->valueint;
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

static av_cold void uninit(AVFilterContext *ctx)
{
    KeyframesContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
    av_expr_free(s->x_pexpr); s->x_pexpr = NULL;
    av_expr_free(s->y_pexpr); s->y_pexpr = NULL;
	
	//释放资源
	//av_expr_free(s->outw_pexpr);s->outw_pexpr=NULL;
	//av_expr_free(s->outh_pexpr);s->outh_pexpr=NULL;
	//释放文件相关的
	if(s->pFrameInfo){
		for(int i=0;i<s->nFrameSize;i++){
				EachFrameInfo *pFrameInfo = &s->pFrameInfo[i];
				if(pFrameInfo&&pFrameInfo->strIdentfName){
					free(pFrameInfo->strIdentfName);
					pFrameInfo->strIdentfName=NULL;
				}
		}
		free(s->pFrameInfo);
	}
	if(s->frames_file){
		av_free(s->frames_file);
		s->frames_file=NULL;
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


static inline int normalize_xy(double d, int chroma_sub)
{
    if (isnan(d))
        return INT_MAX;
    return (int)d & ~((1 << chroma_sub) - 1);
}

static void eval_expr(AVFilterContext *ctx)
{
    KeyframesContext *s = ctx->priv;

    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, NULL);
    /* It is necessary if x is expressed from y  */
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->x = normalize_xy(s->var_values[VAR_X], s->hsub);
    s->y = normalize_xy(s->var_values[VAR_Y], s->vsub);
}

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = NULL;

    if (*pexpr)
        old = *pexpr;
    ret = av_expr_parse(pexpr, expr, var_names,
                        NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when evaluating the expression '%s' for %s\n",
               expr, option);
        *pexpr = old;
        return ret;
    }

    av_expr_free(old);
    return 0;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    KeyframesContext *s = ctx->priv;
    int ret;

    if      (!strcmp(cmd, "x"))
        ret = set_expr(&s->x_pexpr, args, cmd, ctx);
    else if (!strcmp(cmd, "y"))
        ret = set_expr(&s->y_pexpr, args, cmd, ctx);
    else
        ret = AVERROR(ENOSYS);

    if (ret < 0)
        return ret;

    if (s->eval_mode == EVAL_MODE_INIT) {
        eval_expr(ctx);
        av_log(ctx, AV_LOG_VERBOSE, "x:%f xi:%d y:%f yi:%d\n",
               s->var_values[VAR_X], s->x,
               s->var_values[VAR_Y], s->y);
    }
    return ret;
}

static const enum AVPixelFormat alpha_pix_fmts[] = {
    AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_ARGB, AV_PIX_FMT_ABGR, AV_PIX_FMT_RGBA,
    AV_PIX_FMT_BGRA, AV_PIX_FMT_GBRAP, AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    KeyframesContext *s = ctx->priv;

    /* overlay formats contains alpha, for avoiding conversion with alpha information loss */
    static const enum AVPixelFormat main_pix_fmts_yuv420[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_NV12, AV_PIX_FMT_NV21,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat overlay_pix_fmts_yuv420[] = {
        AV_PIX_FMT_YUVA420P, AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat main_pix_fmts_yuv422[] = {
        AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat overlay_pix_fmts_yuv422[] = {
        AV_PIX_FMT_YUVA422P, AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat main_pix_fmts_yuv444[] = {
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVA444P, AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat overlay_pix_fmts_yuv444[] = {
        AV_PIX_FMT_YUVA444P, AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat main_pix_fmts_gbrp[] = {
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRAP, AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat overlay_pix_fmts_gbrp[] = {
        AV_PIX_FMT_GBRAP, AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat main_pix_fmts_rgb[] = {
        AV_PIX_FMT_ARGB,  AV_PIX_FMT_RGBA,
        AV_PIX_FMT_ABGR,  AV_PIX_FMT_BGRA,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat overlay_pix_fmts_rgb[] = {
        AV_PIX_FMT_ARGB,  AV_PIX_FMT_RGBA,
        AV_PIX_FMT_ABGR,  AV_PIX_FMT_BGRA,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *main_formats = NULL;
    AVFilterFormats *overlay_formats = NULL;
    int ret;

    switch (s->format) {
    case OVERLAY_FORMAT_YUV420:
        if (!(main_formats    = ff_make_format_list(main_pix_fmts_yuv420)) ||
            !(overlay_formats = ff_make_format_list(overlay_pix_fmts_yuv420))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case OVERLAY_FORMAT_YUV422:
        if (!(main_formats    = ff_make_format_list(main_pix_fmts_yuv422)) ||
            !(overlay_formats = ff_make_format_list(overlay_pix_fmts_yuv422))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case OVERLAY_FORMAT_YUV444:
        if (!(main_formats    = ff_make_format_list(main_pix_fmts_yuv444)) ||
            !(overlay_formats = ff_make_format_list(overlay_pix_fmts_yuv444))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case OVERLAY_FORMAT_RGB:
        if (!(main_formats    = ff_make_format_list(main_pix_fmts_rgb)) ||
            !(overlay_formats = ff_make_format_list(overlay_pix_fmts_rgb))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case OVERLAY_FORMAT_GBRP:
        if (!(main_formats    = ff_make_format_list(main_pix_fmts_gbrp)) ||
            !(overlay_formats = ff_make_format_list(overlay_pix_fmts_gbrp))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    case OVERLAY_FORMAT_AUTO:
        if (!(main_formats    = ff_make_format_list(alpha_pix_fmts))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        break;
    default:
        av_assert0(0);
    }

    if (s->format == OVERLAY_FORMAT_AUTO) {
        ret = ff_set_common_formats(ctx, main_formats);
        if (ret < 0)
            goto fail;
    } else {
        if ((ret = ff_formats_ref(main_formats   , &ctx->inputs[MAIN]->out_formats   )) < 0 ||
            (ret = ff_formats_ref(overlay_formats, &ctx->inputs[OVERLAY]->out_formats)) < 0 ||
            (ret = ff_formats_ref(main_formats   , &ctx->outputs[MAIN]->in_formats   )) < 0)
                goto fail;
    }

    return 0;
fail:
    if (main_formats)
        av_freep(&main_formats->formats);
    av_freep(&main_formats);
    if (overlay_formats)
        av_freep(&overlay_formats->formats);
    av_freep(&overlay_formats);
    return ret;
}

static int config_input_overlay(AVFilterLink *inlink)
{
//新增局部变量
	const char *expr=NULL;
	double res=0.0;

    AVFilterContext *ctx  = inlink->dst;
    KeyframesContext  *s = inlink->dst->priv;
    int ret;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);

    av_image_fill_max_pixsteps(s->overlay_pix_step, NULL, pix_desc);

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    s->var_values[VAR_MAIN_W   ] = s->var_values[VAR_MW] = ctx->inputs[MAIN   ]->w;
    s->var_values[VAR_MAIN_H   ] = s->var_values[VAR_MH] = ctx->inputs[MAIN   ]->h;
    s->var_values[VAR_OVERLAY_W] = s->var_values[VAR_OW] = ctx->inputs[OVERLAY]->w;
    s->var_values[VAR_OVERLAY_H] = s->var_values[VAR_OH] = ctx->inputs[OVERLAY]->h;
    s->var_values[VAR_HSUB]  = 1<<pix_desc->log2_chroma_w;
    s->var_values[VAR_VSUB]  = 1<<pix_desc->log2_chroma_h;
    s->var_values[VAR_X]     = NAN;
    s->var_values[VAR_Y]     = NAN;
    s->var_values[VAR_N]     = 0;
    s->var_values[VAR_T]     = NAN;
    s->var_values[VAR_POS]   = NAN;

    if ((ret = set_expr(&s->x_pexpr,      s->x_expr,      "x",      ctx)) < 0 ||
        (ret = set_expr(&s->y_pexpr,      s->y_expr,      "y",      ctx)) < 0)
        return ret;

    s->overlay_is_packed_rgb =
        ff_fill_rgba_map(s->overlay_rgba_map, inlink->format) >= 0;
    s->overlay_has_alpha = ff_fmt_is_in(inlink->format, alpha_pix_fmts);

    if (s->eval_mode == EVAL_MODE_INIT) {
        eval_expr(ctx);
        av_log(ctx, AV_LOG_VERBOSE, "x:%f xi:%d y:%f yi:%d\n",
               s->var_values[VAR_X], s->x,
               s->var_values[VAR_Y], s->y);
    }
	//这里添加新增的参数信息解析
	s->var_values[VAR_OUT_H]   = NAN;
	s->var_values[VAR_OUT_W]   = NAN;
	
	/*if ((ret = set_expr(&s->outw_pexpr,      s->outw_expr,      "out_w",      ctx)) < 0 ||
        (ret = set_expr(&s->outh_pexpr,      s->outh_expr,      "out_h",      ctx)) < 0)
        return ret;
    */
	
	if ((ret = av_expr_parse_and_eval(&res, (expr = s->outh_expr),
										 var_names, s->var_values,
										 NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0){
		 goto fail_expr;
	}
	s->var_values[VAR_OUT_H]  = res;
	  
	if ((ret = av_expr_parse_and_eval(&res, (expr = s->outw_expr),
										 var_names, s->var_values,
										 NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0){
		 goto fail_expr;
	}
	
	s->var_values[VAR_OUT_W]  = res;
	if (normalize_double(&s->out_w, s->var_values[VAR_OUT_W]) < 0 ||
	    normalize_double(&s->out_h, s->var_values[VAR_OUT_H]) < 0) {
	   av_log(ctx, AV_LOG_ERROR,
			  "Too big value or invalid expression for out_w/ow or out_h/oh. "
			  "Maybe the expression for out_w:'%s' or for out_h:'%s' is self-referencing.\n",
			  s->outw_expr, s->outh_expr);
	   return AVERROR(EINVAL);
	}
	
	//这里通过解析json文件的数据，获取数据信息
	if(s->frames_file){
		if(parse_json_file(s,s->frames_file)<0){
			goto fail_expr;
		}
	}

    av_log(ctx, AV_LOG_TRACE,
           "main w:%d h:%d fmt:%s overlay w:%d h:%d fmt:%s,out:w:%d,h:%d\n",
           ctx->inputs[MAIN]->w, ctx->inputs[MAIN]->h,
           av_get_pix_fmt_name(ctx->inputs[MAIN]->format),
           ctx->inputs[OVERLAY]->w, ctx->inputs[OVERLAY]->h,
           av_get_pix_fmt_name(ctx->inputs[OVERLAY]->format),
           s->out_w,s->out_h);
	  return 0;
fail_expr:
	av_log(ctx, AV_LOG_ERROR,
			"Too big value or invalid expression for out_w or out_h. "
			"Maybe the expression for out_w:'%s' or for out_h:'%s' is self-referencing.\n",
			s->outw_expr, s->outh_expr);
	return ret;
  
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    KeyframesContext *s = ctx->priv;
    int ret;

    if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
        return ret;

	//修改输出
    //outlink->w = ctx->inputs[MAIN]->w;
    //outlink->h = ctx->inputs[MAIN]->h;
	outlink->w = s->out_w;
    outlink->h = s->out_h;
	
    outlink->time_base = ctx->inputs[MAIN]->time_base;
	
	av_log(ctx, AV_LOG_TRACE,"the out link :wxh:%dx%d\n",outlink->w,outlink->h);

    return ff_framesync_configure(&s->fs);
}

// divide by 255 and round to nearest
// apply a fast variant: (X+127)/255 = ((X+127)*257+257)>>16 = ((X+128)*257)>>16
#define FAST_DIV255(x) ((((x) + 128) * 257) >> 16)

// calculate the unpremultiplied alpha, applying the general equation:
// alpha = alpha_overlay / ( (alpha_main + alpha_overlay) - (alpha_main * alpha_overlay) )
// (((x) << 16) - ((x) << 9) + (x)) is a faster version of: 255 * 255 * x
// ((((x) + (y)) << 8) - ((x) + (y)) - (y) * (x)) is a faster version of: 255 * (x + y)
#define UNPREMULTIPLY_ALPHA(x, y) ((((x) << 16) - ((x) << 9) + (x)) / ((((x) + (y)) << 8) - ((x) + (y)) - (y) * (x)))

/**
 * Blend image in src to destination buffer dst at position (x, y).
 */

static av_always_inline void blend_slice_packed_rgb(AVFilterContext *ctx,
                                   AVFrame *dst, const AVFrame *src,
                                   int main_has_alpha, int x, int y,
                                   int is_straight, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    int i, imax, j, jmax;
    const int src_w = src->width;
    const int src_h = src->height;
    const int dst_w = dst->width;
    const int dst_h = dst->height;
    uint8_t alpha;          ///< the amount of overlay to blend on to main
    const int dr = s->main_rgba_map[R];
    const int dg = s->main_rgba_map[G];
    const int db = s->main_rgba_map[B];
    const int da = s->main_rgba_map[A];
    const int dstep = s->main_pix_step[0];
    const int sr = s->overlay_rgba_map[R];
    const int sg = s->overlay_rgba_map[G];
    const int sb = s->overlay_rgba_map[B];
    const int sa = s->overlay_rgba_map[A];
    const int sstep = s->overlay_pix_step[0];
    int slice_start, slice_end;
    uint8_t *S, *sp, *d, *dp;

    i = FFMAX(-y, 0);
    imax = FFMIN3(-y + dst_h, FFMIN(src_h, dst_h), y + src_h);

    slice_start = i + (imax * jobnr) / nb_jobs;
    slice_end = i + (imax * (jobnr+1)) / nb_jobs;

    sp = src->data[0] + (slice_start)     * src->linesize[0];
    dp = dst->data[0] + (y + slice_start) * dst->linesize[0];

    for (i = slice_start; i < slice_end; i++) {
        j = FFMAX(-x, 0);
        S = sp + j     * sstep;
        d = dp + (x+j) * dstep;

        for (jmax = FFMIN(-x + dst_w, src_w); j < jmax; j++) {
            alpha = S[sa];

            // if the main channel has an alpha channel, alpha has to be calculated
            // to create an un-premultiplied (straight) alpha value
            if (main_has_alpha && alpha != 0 && alpha != 255) {
                uint8_t alpha_d = d[da];
                alpha = UNPREMULTIPLY_ALPHA(alpha, alpha_d);
            }

            switch (alpha) {
            case 0:
                break;
            case 255:
                d[dr] = S[sr];
                d[dg] = S[sg];
                d[db] = S[sb];
                break;
            default:
                // main_value = main_value * (1 - alpha) + overlay_value * alpha
                // since alpha is in the range 0-255, the result must divided by 255
                d[dr] = is_straight ? FAST_DIV255(d[dr] * (255 - alpha) + S[sr] * alpha) :
                        FFMIN(FAST_DIV255(d[dr] * (255 - alpha)) + S[sr], 255);
                d[dg] = is_straight ? FAST_DIV255(d[dg] * (255 - alpha) + S[sg] * alpha) :
                        FFMIN(FAST_DIV255(d[dg] * (255 - alpha)) + S[sg], 255);
                d[db] = is_straight ? FAST_DIV255(d[db] * (255 - alpha) + S[sb] * alpha) :
                        FFMIN(FAST_DIV255(d[db] * (255 - alpha)) + S[sb], 255);
            }
            if (main_has_alpha) {
                switch (alpha) {
                case 0:
                    break;
                case 255:
                    d[da] = S[sa];
                    break;
                default:
                    // apply alpha compositing: main_alpha += (1-main_alpha) * overlay_alpha
                    d[da] += FAST_DIV255((255 - d[da]) * S[sa]);
                }
            }
            d += dstep;
            S += sstep;
        }
        dp += dst->linesize[0];
        sp += src->linesize[0];
    }
}

static av_always_inline void blend_plane(AVFilterContext *ctx,
                                         AVFrame *dst, const AVFrame *src,
                                         int src_w, int src_h,
                                         int dst_w, int dst_h,
                                         int i, int hsub, int vsub,
                                         int x, int y,
                                         int main_has_alpha,
                                         int dst_plane,
                                         int dst_offset,
                                         int dst_step,
                                         int straight,
                                         int yuv,
                                         int jobnr,
                                         int nb_jobs)
{
    KeyframesContext *octx = ctx->priv;
    int src_wp = AV_CEIL_RSHIFT(src_w, hsub);
    int src_hp = AV_CEIL_RSHIFT(src_h, vsub);
    int dst_wp = AV_CEIL_RSHIFT(dst_w, hsub);
    int dst_hp = AV_CEIL_RSHIFT(dst_h, vsub);
    int yp = y>>vsub;
    int xp = x>>hsub;
    uint8_t *s, *sp, *d, *dp, *dap, *a, *da, *ap;
    int jmax, j, k, kmax;
    int slice_start, slice_end;

    j = FFMAX(-yp, 0);
    jmax = FFMIN3(-yp + dst_hp, FFMIN(src_hp, dst_hp), yp + src_hp);

    slice_start = j + (jmax * jobnr) / nb_jobs;
    slice_end = j + (jmax * (jobnr+1)) / nb_jobs;

    sp = src->data[i] + (slice_start) * src->linesize[i];
    dp = dst->data[dst_plane]
                      + (yp + slice_start) * dst->linesize[dst_plane]
                      + dst_offset;
    ap = src->data[3] + (slice_start << vsub) * src->linesize[3];
    dap = dst->data[3] + ((yp + slice_start) << vsub) * dst->linesize[3];

    for (j = slice_start; j < slice_end; j++) {
        k = FFMAX(-xp, 0);
        d = dp + (xp+k) * dst_step;
        s = sp + k;
        a = ap + (k<<hsub);
        da = dap + ((xp+k) << hsub);
        kmax = FFMIN(-xp + dst_wp, src_wp);

        if (((vsub && j+1 < src_hp) || !vsub) && octx->blend_row[i]) {
            int c = octx->blend_row[i](d, da, s, a, kmax - k, src->linesize[3]);

            s += c;
            d += dst_step * c;
            da += (1 << hsub) * c;
            a += (1 << hsub) * c;
            k += c;
        }
        for (; k < kmax; k++) {
            int alpha_v, alpha_h, alpha;

            // average alpha for color components, improve quality
            if (hsub && vsub && j+1 < src_hp && k+1 < src_wp) {
                alpha = (a[0] + a[src->linesize[3]] +
                         a[1] + a[src->linesize[3]+1]) >> 2;
            } else if (hsub || vsub) {
                alpha_h = hsub && k+1 < src_wp ?
                    (a[0] + a[1]) >> 1 : a[0];
                alpha_v = vsub && j+1 < src_hp ?
                    (a[0] + a[src->linesize[3]]) >> 1 : a[0];
                alpha = (alpha_v + alpha_h) >> 1;
            } else
                alpha = a[0];
            // if the main channel has an alpha channel, alpha has to be calculated
            // to create an un-premultiplied (straight) alpha value
            if (main_has_alpha && alpha != 0 && alpha != 255) {
                // average alpha for color components, improve quality
                uint8_t alpha_d;
                if (hsub && vsub && j+1 < src_hp && k+1 < src_wp) {
                    alpha_d = (da[0] + da[dst->linesize[3]] +
                               da[1] + da[dst->linesize[3]+1]) >> 2;
                } else if (hsub || vsub) {
                    alpha_h = hsub && k+1 < src_wp ?
                        (da[0] + da[1]) >> 1 : da[0];
                    alpha_v = vsub && j+1 < src_hp ?
                        (da[0] + da[dst->linesize[3]]) >> 1 : da[0];
                    alpha_d = (alpha_v + alpha_h) >> 1;
                } else
                    alpha_d = da[0];
                alpha = UNPREMULTIPLY_ALPHA(alpha, alpha_d);
            }
            if (straight) {
                *d = FAST_DIV255(*d * (255 - alpha) + *s * alpha);
            } else {
                if (i && yuv)
                    *d = av_clip(FAST_DIV255((*d - 128) * (255 - alpha)) + *s - 128, -128, 128) + 128;
                else
                    *d = FFMIN(FAST_DIV255(*d * (255 - alpha)) + *s, 255);
            }
            s++;
            d += dst_step;
            da += 1 << hsub;
            a += 1 << hsub;
        }
        dp += dst->linesize[dst_plane];
        sp += src->linesize[i];
        ap += (1 << vsub) * src->linesize[3];
        dap += (1 << vsub) * dst->linesize[3];
    }
}

static inline void alpha_composite(const AVFrame *src, const AVFrame *dst,
                                   int src_w, int src_h,
                                   int dst_w, int dst_h,
                                   int x, int y,
                                   int jobnr, int nb_jobs)
{
    uint8_t alpha;          ///< the amount of overlay to blend on to main
    uint8_t *s, *sa, *d, *da;
    int i, imax, j, jmax;
    int slice_start, slice_end;

    imax = FFMIN(-y + dst_h, src_h);
    slice_start = (imax * jobnr) / nb_jobs;
    slice_end = ((imax * (jobnr+1)) / nb_jobs);

    i = FFMAX(-y, 0);
    sa = src->data[3] + (i + slice_start) * src->linesize[3];
    da = dst->data[3] + (y + i + slice_start) * dst->linesize[3];

    for (i = i + slice_start; i < slice_end; i++) {
        j = FFMAX(-x, 0);
        s = sa + j;
        d = da + x+j;

        for (jmax = FFMIN(-x + dst_w, src_w); j < jmax; j++) {
            alpha = *s;
            if (alpha != 0 && alpha != 255) {
                uint8_t alpha_d = *d;
                alpha = UNPREMULTIPLY_ALPHA(alpha, alpha_d);
            }
            switch (alpha) {
            case 0:
                break;
            case 255:
                *d = *s;
                break;
            default:
                // apply alpha compositing: main_alpha += (1-main_alpha) * overlay_alpha
                *d += FAST_DIV255((255 - *d) * *s);
            }
            d += 1;
            s += 1;
        }
        da += dst->linesize[3];
        sa += src->linesize[3];
    }
}

static av_always_inline void blend_slice_yuv(AVFilterContext *ctx,
                                             AVFrame *dst, const AVFrame *src,
                                             int hsub, int vsub,
                                             int main_has_alpha,
                                             int x, int y,
                                             int is_straight,
                                             int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    const int src_w = src->width;
    const int src_h = src->height;
    const int dst_w = dst->width;
    const int dst_h = dst->height;

    blend_plane(ctx, dst, src, src_w, src_h, dst_w, dst_h, 0, 0,       0, x, y, main_has_alpha,
                s->main_desc->comp[0].plane, s->main_desc->comp[0].offset, s->main_desc->comp[0].step, is_straight, 1,
                jobnr, nb_jobs);
    blend_plane(ctx, dst, src, src_w, src_h, dst_w, dst_h, 1, hsub, vsub, x, y, main_has_alpha,
                s->main_desc->comp[1].plane, s->main_desc->comp[1].offset, s->main_desc->comp[1].step, is_straight, 1,
                jobnr, nb_jobs);
    blend_plane(ctx, dst, src, src_w, src_h, dst_w, dst_h, 2, hsub, vsub, x, y, main_has_alpha,
                s->main_desc->comp[2].plane, s->main_desc->comp[2].offset, s->main_desc->comp[2].step, is_straight, 1,
                jobnr, nb_jobs);

    if (main_has_alpha)
        alpha_composite(src, dst, src_w, src_h, dst_w, dst_h, x, y, jobnr, nb_jobs);
}

static av_always_inline void blend_slice_planar_rgb(AVFilterContext *ctx,
                                                    AVFrame *dst, const AVFrame *src,
                                                    int hsub, int vsub,
                                                    int main_has_alpha,
                                                    int x, int y,
                                                    int is_straight,
                                                    int jobnr,
                                                    int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    const int src_w = src->width;
    const int src_h = src->height;
    const int dst_w = dst->width;
    const int dst_h = dst->height;

    blend_plane(ctx, dst, src, src_w, src_h, dst_w, dst_h, 0, 0,       0, x, y, main_has_alpha,
                s->main_desc->comp[1].plane, s->main_desc->comp[1].offset, s->main_desc->comp[1].step, is_straight, 0,
                jobnr, nb_jobs);
    blend_plane(ctx, dst, src, src_w, src_h, dst_w, dst_h, 1, hsub, vsub, x, y, main_has_alpha,
                s->main_desc->comp[2].plane, s->main_desc->comp[2].offset, s->main_desc->comp[2].step, is_straight, 0,
                jobnr, nb_jobs);
    blend_plane(ctx, dst, src, src_w, src_h, dst_w, dst_h, 2, hsub, vsub, x, y, main_has_alpha,
                s->main_desc->comp[0].plane, s->main_desc->comp[0].offset, s->main_desc->comp[0].step, is_straight, 0,
                jobnr, nb_jobs);

    if (main_has_alpha)
        alpha_composite(src, dst, src_w, src_h, dst_w, dst_h, x, y, jobnr, nb_jobs);
}

static int blend_slice_yuv420(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 1, 0, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuva420(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 1, 1, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuv422(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 0, 0, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuva422(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 0, 1, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuv444(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 0, 0, 0, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuva444(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 0, 0, 1, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_gbrp(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_planar_rgb(ctx, td->dst, td->src, 0, 0, 0, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_gbrap(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_planar_rgb(ctx, td->dst, td->src, 0, 0, 1, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuv420_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 1, 0, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuva420_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 1, 1, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuv422_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 0, 0, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuva422_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 1, 0, 1, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuv444_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 0, 0, 0, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_yuva444_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_yuv(ctx, td->dst, td->src, 0, 0, 1, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_gbrp_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_planar_rgb(ctx, td->dst, td->src, 0, 0, 0, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_gbrap_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_planar_rgb(ctx, td->dst, td->src, 0, 0, 1, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_rgb(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_packed_rgb(ctx, td->dst, td->src, 0, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_rgba(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_packed_rgb(ctx, td->dst, td->src, 1, s->x, s->y, 1, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_rgb_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_packed_rgb(ctx, td->dst, td->src, 0, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int blend_slice_rgba_pm(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    KeyframesContext *s = ctx->priv;
    ThreadData *td = arg;
    blend_slice_packed_rgb(ctx, td->dst, td->src, 1, s->x, s->y, 0, jobnr, nb_jobs);
    return 0;
}

static int config_input_main(AVFilterLink *inlink)
{
    KeyframesContext *s = inlink->dst->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);

    av_image_fill_max_pixsteps(s->main_pix_step,    NULL, pix_desc);

    s->hsub = pix_desc->log2_chroma_w;
    s->vsub = pix_desc->log2_chroma_h;

    s->main_desc = pix_desc;

    s->main_is_packed_rgb =
        ff_fill_rgba_map(s->main_rgba_map, inlink->format) >= 0;
    s->main_has_alpha = ff_fmt_is_in(inlink->format, alpha_pix_fmts);
    switch (s->format) {
    case OVERLAY_FORMAT_YUV420:
        s->blend_slice = s->main_has_alpha ? blend_slice_yuva420 : blend_slice_yuv420;
        break;
    case OVERLAY_FORMAT_YUV422:
        s->blend_slice = s->main_has_alpha ? blend_slice_yuva422 : blend_slice_yuv422;
        break;
    case OVERLAY_FORMAT_YUV444:
        s->blend_slice = s->main_has_alpha ? blend_slice_yuva444 : blend_slice_yuv444;
        break;
    case OVERLAY_FORMAT_RGB:
        s->blend_slice = s->main_has_alpha ? blend_slice_rgba : blend_slice_rgb;
        break;
    case OVERLAY_FORMAT_GBRP:
        s->blend_slice = s->main_has_alpha ? blend_slice_gbrap : blend_slice_gbrp;
        break;
    case OVERLAY_FORMAT_AUTO:
        switch (inlink->format) {
        case AV_PIX_FMT_YUVA420P:
            s->blend_slice = blend_slice_yuva420;
            break;
        case AV_PIX_FMT_YUVA422P:
            s->blend_slice = blend_slice_yuva422;
            break;
        case AV_PIX_FMT_YUVA444P:
            s->blend_slice = blend_slice_yuva444;
            break;
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_ABGR:
            s->blend_slice = blend_slice_rgba;
            break;
        case AV_PIX_FMT_GBRAP:
            s->blend_slice = blend_slice_gbrap;
            break;
        default:
            av_assert0(0);
            break;
        }
        break;
    }

    if (!s->alpha_format)
        goto end;

    switch (s->format) {
    case OVERLAY_FORMAT_YUV420:
        s->blend_slice = s->main_has_alpha ? blend_slice_yuva420_pm : blend_slice_yuv420_pm;
        break;
    case OVERLAY_FORMAT_YUV422:
        s->blend_slice = s->main_has_alpha ? blend_slice_yuva422_pm : blend_slice_yuv422_pm;
        break;
    case OVERLAY_FORMAT_YUV444:
        s->blend_slice = s->main_has_alpha ? blend_slice_yuva444_pm : blend_slice_yuv444_pm;
        break;
    case OVERLAY_FORMAT_RGB:
        s->blend_slice = s->main_has_alpha ? blend_slice_rgba_pm : blend_slice_rgb_pm;
        break;
    case OVERLAY_FORMAT_GBRP:
        s->blend_slice = s->main_has_alpha ? blend_slice_gbrap_pm : blend_slice_gbrp_pm;
        break;
    case OVERLAY_FORMAT_AUTO:
        switch (inlink->format) {
        case AV_PIX_FMT_YUVA420P:
            s->blend_slice = blend_slice_yuva420_pm;
            break;
        case AV_PIX_FMT_YUVA422P:
            s->blend_slice = blend_slice_yuva422_pm;
            break;
        case AV_PIX_FMT_YUVA444P:
            s->blend_slice = blend_slice_yuva444_pm;
            break;
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_ABGR:
            s->blend_slice = blend_slice_rgba_pm;
            break;
        case AV_PIX_FMT_GBRAP:
            s->blend_slice = blend_slice_gbrap_pm;
            break;
        default:
            av_assert0(0);
            break;
        }
        break;
    }

end:
    if (ARCH_X86)
        ff_keyframes_init_x86(s, s->format, inlink->format,
                            s->alpha_format, s->main_has_alpha);

    return 0;
}

//对图像进行裁剪
static int CropFuction(AVFilterContext *ctx,AVFilterLink *inlink,AVFrame *frame,CropBox cropBox){
	int ret=0;
	KeyframesContext *s = ctx->priv;
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
	int crop_w=0,crop_h=0;
	int crop_x=0,crop_y=0;
	int max_step[4]={0};
    int hsub = desc->log2_chroma_w;
    int vsub = desc->log2_chroma_h;
	av_image_fill_max_pixsteps(max_step, NULL, desc);
	
	crop_x = cropBox.struLeftTop.nX;
	crop_y = cropBox.struLeftTop.nY;
	crop_w = cropBox.nW;
	crop_h = cropBox.nH;
	
	if (crop_x < 0)
        crop_x = 0;
    if (crop_y < 0)
        crop_y = 0;
    if ((unsigned)crop_x + (unsigned)crop_w > frame->width)
        crop_x = frame->width - crop_w;
    if ((unsigned)crop_y + (unsigned)crop_h > frame->height)
        crop_y = frame->height - crop_h;

	 av_log(ctx, AV_LOG_TRACE, "t:%lld x:%d y:%d x+w:%d y+h:%d\n",
            frame->pts,crop_x, crop_y, crop_x+crop_w, crop_y+crop_h);
	//添加一个打印语句
	 av_log(ctx, AV_LOG_TRACE, "the frame display num:%d,pts:%f,fmt:%d\n",
            frame->display_picture_number,frame->pts == AV_NOPTS_VALUE ?NAN : frame->pts * av_q2d(inlink->time_base),frame->format);
	
	frame->data[0] += crop_y * frame->linesize[0];
    frame->data[0] += crop_x * max_step[0];

    if (!(desc->flags & AV_PIX_FMT_FLAG_PAL || desc->flags & FF_PSEUDOPAL)) {
        for (int i = 1; i < 3; i ++) {
            if (frame->data[i]) {
                frame->data[i] += (crop_y >> vsub) * frame->linesize[i];
                frame->data[i] += (crop_x * max_step[i]) >> hsub;
            }
        }
    }

    /* alpha plane */
    if (frame->data[3]) {
        frame->data[3] += crop_y * frame->linesize[3];
        frame->data[3] += crop_x * max_step[3];
    }
	av_log(ctx, AV_LOG_TRACE, "src:w:%d,h:%d,crop:w:%d,h:%d\n",
		frame->width,frame->height,crop_w,crop_h);
	//更改一下图像的宽高
	frame->width=crop_w;
	frame->height=crop_h;
	
	return ret;
}

//对图像进行宿放
static int ScaleFuction(AVFilterContext *ctx,AVFilterLink *inlink2,AVFrame *src,AVFrame** dest,int destOutW,int destOutH,int destOutFmt){
	int scaleOutW = destOutW;
	int scaleOutH = destOutH;
	AVFrame *scale_out=NULL;
	int ret=0;
	KeyframesContext *s = ctx->priv;
	//先需要分配一帧输出的数据缓存
	scale_out = ff_get_video_buffer(inlink2, scaleOutW, scaleOutH);
	if (!scale_out) {
		ret = AVERROR(ENOMEM);
		return ret;
	}
	s->pSwsCtx= sws_alloc_context();
	if (!s->pSwsCtx) {
		av_log(ctx, AV_LOG_ERROR, "alloc sws context failed\n");
		ret = AVERROR(ENOMEM);
		goto error;
	}
	av_opt_set_int(s->pSwsCtx, "srcw", src->width, 0);
	av_opt_set_int(s->pSwsCtx, "srch", src->height, 0);
	av_opt_set_int(s->pSwsCtx, "src_format", src->format, 0);
	av_opt_set_int(s->pSwsCtx, "dstw", scaleOutW, 0);
	av_opt_set_int(s->pSwsCtx, "dsth", scaleOutH, 0);
	av_opt_set_int(s->pSwsCtx, "dst_format", destOutFmt, 0);
	av_opt_set_int(s->pSwsCtx, "sws_flags", SWS_BICUBIC, 0);
	
	if ((ret = sws_init_context(s->pSwsCtx, NULL, NULL)) < 0)
		goto error;
	
	sws_scale(s->pSwsCtx, (const uint8_t *const *)src->data, src->linesize, 0, src->height, scale_out->data, scale_out->linesize);
	//释放资源
	sws_freeContext(s->pSwsCtx);
	s->pSwsCtx = NULL;
	//
	//在释放帧数据信息之前，我们需要拷贝一下之前的数据信息
	scale_out->pts = src->pts;
	scale_out->pkt_dts = src->pkt_dts;
	av_frame_free(&src);
	src=NULL;
	
	//更新一下
	*dest=scale_out;
	return ret;
error:
	av_log(ctx, AV_LOG_ERROR, " error:%d\n",ret);
	if(scale_out){
		av_frame_free(&scale_out);
		scale_out=NULL;
	}
	return ret;	
}

static int ScaleFuction2(AVFilterContext *ctx,AVFilterLink *inlink2,AVFrame *src,AVFrame** dest,int destOutW,int destOutH,int destOutFmt){
	int scaleOutW = destOutW;
	int scaleOutH = destOutH;
	AVFrame *scale_out=NULL;
	int ret=0;
	KeyframesContext *s = ctx->priv;
	//先需要分配一帧输出的数据缓存
	scale_out = ff_get_video_buffer(inlink2, scaleOutW, scaleOutH);
	if (!scale_out) {
		ret = AVERROR(ENOMEM);
		return ret;
	}
	s->pSwsCtx= sws_alloc_context();
	if (!s->pSwsCtx) {
		av_log(ctx, AV_LOG_ERROR, "alloc sws context failed\n");
		ret = AVERROR(ENOMEM);
		goto error;
	}
	av_opt_set_int(s->pSwsCtx, "srcw", src->width, 0);
	av_opt_set_int(s->pSwsCtx, "srch", src->height, 0);
	av_opt_set_int(s->pSwsCtx, "src_format", src->format, 0);
	av_opt_set_int(s->pSwsCtx, "dstw", scaleOutW, 0);
	av_opt_set_int(s->pSwsCtx, "dsth", scaleOutH, 0);
	av_opt_set_int(s->pSwsCtx, "dst_format", destOutFmt, 0);
	av_opt_set_int(s->pSwsCtx, "sws_flags", SWS_BICUBIC, 0);
	
	if ((ret = sws_init_context(s->pSwsCtx, NULL, NULL)) < 0)
		goto error;
	
	sws_scale(s->pSwsCtx, (const uint8_t *const *)src->data, src->linesize, 0, src->height, scale_out->data, scale_out->linesize);
	//释放资源
	sws_freeContext(s->pSwsCtx);
	s->pSwsCtx = NULL;
	//
	//在释放帧数据信息之前，我们需要拷贝一下之前的数据信息
	scale_out->pts = src->pts;
	scale_out->pkt_dts = src->pkt_dts;
	scale_out->best_effort_timestamp = src->best_effort_timestamp;
	//不需要释放内存
	//av_frame_free(&src);
	//src=NULL;
	//更新一下
	*dest=scale_out;
	return ret;
error:
	av_log(ctx, AV_LOG_ERROR, " error:%d\n",ret);
	if(scale_out){
		av_frame_free(&scale_out);
		scale_out=NULL;
	}
	return ret;	
}


//这里自己写一个帧拷贝函数
static int avframe_copy(AVFilterLink *inlink,AVFrame* in,AVFrame** out){
	int ret =0;
	AVFrame* out2 = av_frame_clone(in);
    if (!out2) {
       return AVERROR(ENOMEM);
    }
	/*ret=av_frame_copy(out2,in);
	if(ret<0){
		return ret;
	}*/
	*out =out2;
	return ret;
}

static int do_blend(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFrame *mainpic=NULL, *second=NULL;
    KeyframesContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    int ret=0;
	//新增几个变量参数用于处理
	float scale =1.0;
	CropBox struCropBox;
	Position struOverlayPos;
	int outW,outH;
	int scaleOutW,scaleOutH;
	AVFilterLink *inlink2 = ctx->inputs[1];
	AVFrame *second_out=NULL;
	AVFrame *scale_out=NULL;
	//这里需要注意，我们获取的两个frame数据，其中second是not writeable的，也就是说底下进行缓存
	//如果我们在上层自己对这个帧数据处理的话，在这里可能会出现一些其他的问题，例如崩溃啥的情况
    ret = ff_framesync_dualinput_get_writable(fs, &mainpic, &second);
    if (ret < 0)
        return ret;
    if (!second)
        return ff_filter_frame(ctx->outputs[0], mainpic);

    if (s->eval_mode == EVAL_MODE_FRAME) {
        int64_t pos = mainpic->pkt_pos;

        s->var_values[VAR_N] = inlink->frame_count_out;
        s->var_values[VAR_T] = mainpic->pts == AV_NOPTS_VALUE ?
            NAN : mainpic->pts * av_q2d(inlink->time_base);
        s->var_values[VAR_POS] = pos == -1 ? NAN : pos;

        s->var_values[VAR_OVERLAY_W] = s->var_values[VAR_OW] = second->width;
        s->var_values[VAR_OVERLAY_H] = s->var_values[VAR_OH] = second->height;
        s->var_values[VAR_MAIN_W   ] = s->var_values[VAR_MW] = mainpic->width;
        s->var_values[VAR_MAIN_H   ] = s->var_values[VAR_MH] = mainpic->height;

        eval_expr(ctx);
        av_log(ctx, AV_LOG_DEBUG, "n:%f t:%f pos:%f x:%f xi:%d y:%f yi:%d\n",
               s->var_values[VAR_N], s->var_values[VAR_T], s->var_values[VAR_POS],
               s->var_values[VAR_X], s->x,
               s->var_values[VAR_Y], s->y);
    }
	//这里需要在overlay之前先处理其他的事情，例如将overlay的视频宿放，然后同时从famesfile中的参数信息中获取新的overlay位置信息
	av_log(ctx, AV_LOG_TRACE,"start blend\n");
	if(s->pFrameInfo){
		long frame_index=s->llFrameIndex;
		if(frame_index>=s->nFrameSize){
			av_log(ctx, AV_LOG_WARNING,"the frame index:%lld > frames file frame size:%d\n",frame_index,s->nFrameSize);
			frame_index=s->nFrameSize-1;
		}
		
		if(frame_index!=s->pFrameInfo[frame_index].nFrameIndex){
			av_log(ctx, AV_LOG_WARNING,"the frame index:%lld,but the frames file frame index:%d\n",frame_index,s->llFrameIndex);
		}
		EachFrameInfo *pEachFrame = &s->pFrameInfo[frame_index];
		PositionAssign(&struOverlayPos,pEachFrame->struOverlayPos);
		CropBoxAssign(&struCropBox,pEachFrame->struBoxPnt);			
		scale = pEachFrame->dfScale;
		av_log(ctx, AV_LOG_TRACE,"the frame index:%d,scale:%f,overlay pos:x:%d,y:%d,crop box:w:%d,h:%d,x:%d,y:%d\n",
			frame_index,scale,struOverlayPos.nX,struOverlayPos.nY,struCropBox.nW,struCropBox.nH,struCropBox.struLeftTop.nX,struCropBox.struLeftTop.nY);
	}
	#if 1
	//根据scale的值对overlay的frame进行scale
	if(scale!=1.0){
		av_log(ctx,AV_LOG_TRACE,"do blend scale\n");
		//算出这次我们在overlay之前需要将原图放大到多大
		scaleOutW = second->width*scale;
		scaleOutH = second->height*scale;
		if(scaleOutW%2!=0){
			av_log(ctx, AV_LOG_WARNING, "second frame width:%d,scale:%f,so scaleout width:%d is odd\n",second->width,scale,scaleOutW);
			scaleOutW+=1;
		}
		if(scaleOutH%2!=0){
			av_log(ctx, AV_LOG_WARNING, "second frame height:%d,scale:%f,so scaleout height:%d is odd\n",second->height,scale,scaleOutH);
			scaleOutH+=1;
		}
		/*
		//在这里我们对这个帧进行一次数据拷贝 
		ret = avframe_copy(inlink2,second,&second_out);
		if (ret < 0) {
			av_log(ctx, AV_LOG_ERROR, "second frame copy failed:%d\n",ret);
		    second = NULL;
		    return ret;
		}	
		
		if((ret=ScaleFuction(ctx,inlink2,second_out,&scale_out,scaleOutW,scaleOutH,second->format))<0){
			av_log(ctx, AV_LOG_ERROR,"frame index:%lld scale failed,scale out:w:%d,h:%d\n",s->llFrameIndex,scaleOutW,scaleOutH);
			goto error;
		}
		//在赋值回去
		second = scale_out;
		second_out = scale_out;//我们开辟的内存需要释放，在处理完以后
		*/

		//如果不进行数据拷贝的话，我们需要修改ScalseFunction,不应该在这个函数里面释放second这个对象的内存，否则会出现崩溃
		if((ret=ScaleFuction2(ctx,inlink2,second,&second_out,scaleOutW,scaleOutH,second->format))<0){
			av_log(ctx, AV_LOG_ERROR,"frame index:%lld scale failed,scale out:w:%d,h:%d\n",s->llFrameIndex,scaleOutW,scaleOutH);
			goto error;
		}
		//赋值
		second = second_out;
		
	}

	s->llFrameIndex++;//用于帧计算用
	#endif
	
	//更新overlay的位置信息
	s->x = struOverlayPos.nX;
	s->y = struOverlayPos.nY;
	
	av_log(ctx,AV_LOG_TRACE,"do blend overlay\n");
	//overlay
    if (s->x < mainpic->width  && s->x + second->width  >= 0 &&
        s->y < mainpic->height && s->y + second->height >= 0) {
        ThreadData td;

        td.dst = mainpic;
        td.src = second;
        ctx->internal->execute(ctx, s->blend_slice, &td, NULL, FFMIN(FFMAX(1, FFMIN3(s->y + second->height, FFMIN(second->height, mainpic->height), mainpic->height - s->y)),
                                                                     ff_filter_get_nb_threads(ctx)));
    }
	av_log(ctx,AV_LOG_TRACE,"do blend crop\n");
	//overlay之后，我们需要进行裁剪，将我们裁剪的可视区域内容
	if((ret=CropFuction(ctx,inlink,mainpic,struCropBox))<0){
			goto error;
	}
	if(mainpic->width != s->out_w||mainpic->height != s->out_h){
		if((ret=ScaleFuction2(ctx,inlink,mainpic,&scale_out,s->out_w,s->out_h,mainpic->format))<0){
			goto error;
		}
		//在外面进行内存的释放
		if(mainpic){
			av_frame_free(&mainpic);
			mainpic=NULL;
		}
		mainpic = scale_out;
	}
	//需要释放拷贝的帧数据
	if(second_out){
		av_frame_free(&second_out);
		second_out=NULL;
	}
	av_log(ctx,AV_LOG_TRACE,"do blend end\n");
    return ff_filter_frame(ctx->outputs[0], mainpic);
error:
	av_log(ctx, AV_LOG_ERROR, " error:%d\n",ret);
	if(second_out){
		av_frame_free(&second_out);
		second_out=NULL;
	}
	return ret;	
}


static av_cold int init(AVFilterContext *ctx)
{
    KeyframesContext *s = ctx->priv;

    s->fs.on_event = do_blend;
    return 0;
}

static int activate(AVFilterContext *ctx)
{
    KeyframesContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

//这个函数根本不会被调用到
static int filter_frame_overlay(AVFilterLink *inlink, AVFrame *in){
	AVFilterContext *ctx = inlink->dst;
    KeyframesContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[1];
	int ret =0;
    AVFrame *out=NULL;
	AVFrame *scale_out=NULL;
    float scale =1.0;
	int scaleOutW=0,scaleOutH=0;
	av_log(ctx, AV_LOG_TRACE,"start processing overlay frame");
	if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }
	//干我们的图像处理
	if(s->pFrameInfo){
		long frame_index=s->llFrameIndex;
		if(frame_index>=s->nFrameSize){
			av_log(ctx, AV_LOG_WARNING,"the frame index:%lld > frames file frame size:%d\n",frame_index,s->nFrameSize);
			frame_index=s->nFrameSize-1;
		}
		
		if(frame_index!=s->pFrameInfo[frame_index].nFrameIndex){
			av_log(ctx, AV_LOG_WARNING,"the frame index:%lld,but the frames file frame index:%d\n",frame_index,s->llFrameIndex);
		}
		EachFrameInfo *pEachFrame = &s->pFrameInfo[frame_index];
		scale = pEachFrame->dfScale;
		av_log(ctx, AV_LOG_TRACE,"the frame index:%d,scale:%f\n",frame_index,scale);
	}
	s->llFrameIndex++;//用于帧计算用
	//根据scale的值对overlay的frame进行scale
	if(scale!=1.0){
		av_log(ctx,AV_LOG_TRACE,"do blend scale\n");
		//算出这次我们在overlay之前需要将原图放大到多大
		scaleOutW = out->width*scale;
		scaleOutH = out->height*scale;
		if(scaleOutW%2!=0){
			av_log(ctx, AV_LOG_WARNING, "the frame width:%d,scale:%f,so scaleout width:%d is odd\n",out->width,scale,scaleOutW);
			scaleOutW+=1;
		}
		if(scaleOutH%2!=0){
			av_log(ctx, AV_LOG_WARNING, "the frame height:%d,scale:%f,so scaleout height:%d is odd\n",out->height,scale,scaleOutH);
			scaleOutH+=1;
		}	
		
		if((ret=ScaleFuction(ctx,outlink,out,&scale_out,scaleOutW,scaleOutH,out->format))<0){
			av_log(ctx, AV_LOG_ERROR,"frame index:%lld scale failed,scale out:w:%d,h:%d\n",s->llFrameIndex,scaleOutW,scaleOutH);
		}
		out = scale_out;
	}

	if (out != in){
        av_frame_free(&in);
		in=NULL;
	}
	av_log(ctx, AV_LOG_TRACE,"end processing overlay frame");
    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(KeyframesContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption keyframes_options[] = {
    { "x", "set the x expression", OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "y", "set the y expression", OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "eof_action", "Action to take when encountering EOF from secondary input ",
        OFFSET(fs.opt_eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },
        EOF_ACTION_REPEAT, EOF_ACTION_PASS, .flags = FLAGS, "eof_action" },
        { "repeat", "Repeat the previous frame.",   0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, "eof_action" },
        { "endall", "End both streams.",            0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, "eof_action" },
        { "pass",   "Pass through the main input.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS },   .flags = FLAGS, "eof_action" },
    { "eval", "specify when to evaluate expressions", OFFSET(eval_mode), AV_OPT_TYPE_INT, {.i64 = EVAL_MODE_FRAME}, 0, EVAL_MODE_NB-1, FLAGS, "eval" },
         { "init",  "eval expressions once during initialization", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_INIT},  .flags = FLAGS, .unit = "eval" },
         { "frame", "eval expressions per-frame",                  0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_FRAME}, .flags = FLAGS, .unit = "eval" },
    { "shortest", "force termination when the shortest input terminates", OFFSET(fs.opt_shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "format", "set output format", OFFSET(format), AV_OPT_TYPE_INT, {.i64=OVERLAY_FORMAT_YUV420}, 0, OVERLAY_FORMAT_NB-1, FLAGS, "format" },
        { "yuv420", "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_YUV420}, .flags = FLAGS, .unit = "format" },
        { "yuv422", "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_YUV422}, .flags = FLAGS, .unit = "format" },
        { "yuv444", "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_YUV444}, .flags = FLAGS, .unit = "format" },
        { "rgb",    "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_RGB},    .flags = FLAGS, .unit = "format" },
        { "gbrp",   "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_GBRP},   .flags = FLAGS, .unit = "format" },
        { "auto",   "", 0, AV_OPT_TYPE_CONST, {.i64=OVERLAY_FORMAT_AUTO},   .flags = FLAGS, .unit = "format" },
    { "repeatlast", "repeat overlay of the last overlay frame", OFFSET(fs.opt_repeatlast), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
    { "alpha", "alpha format", OFFSET(alpha_format), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "alpha_format" },
        { "straight",      "", 0, AV_OPT_TYPE_CONST, {.i64=0}, .flags = FLAGS, .unit = "alpha_format" },
        { "premultiplied", "", 0, AV_OPT_TYPE_CONST, {.i64=1}, .flags = FLAGS, .unit = "alpha_format" },       
	{ "frames_file",   "path to the keyframes json file",   OFFSET(frames_file), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
	{ "out_w",       "set the out width area expression",   OFFSET(outw_expr), AV_OPT_TYPE_STRING, {.str = "main_w"}, CHAR_MIN, CHAR_MAX, FLAGS },
	{ "out_h",       "set the out height area expression",  OFFSET(outh_expr), AV_OPT_TYPE_STRING, {.str = "main_h"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(keyframes, KeyframesContext, fs);

static const AVFilterPad avfilter_vf_keyframes_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_main,
    },
    {
        .name         = "overlay",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_overlay,
        //.filter_frame = filter_frame_overlay,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_keyframes_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_keyframes = {
    .name          = "keyframes",
    .description   = NULL_IF_CONFIG_SMALL("keyframes for video."),
    .preinit       = keyframes_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(KeyframesContext),
    .priv_class    = &keyframes_class,
    .query_formats = query_formats,
    .activate      = activate,
    .process_command = process_command,
    .inputs        = avfilter_vf_keyframes_inputs,
    .outputs       = avfilter_vf_keyframes_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                     AVFILTER_FLAG_SLICE_THREADS,
};
