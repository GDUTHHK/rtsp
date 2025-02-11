#include "h264encoder.h"
#include "dlog.h"
#include <cstdio>

H264Encoder::H264Encoder()
{

}

H264Encoder::~H264Encoder()
{
    if(ctx_) {
        avcodec_free_context(&ctx_);
    }
    if(frame_) {
        av_frame_free(&frame_);
    }
    if(h264_fp_) {
        fclose(h264_fp_);
        h264_fp_ = nullptr;
    }
}

int H264Encoder::Init(const Properties &properties)
{
    int ret = 0;
    width_ = properties.GetProperty("width", 0);
    if(width_ ==0 || width_%2 != 0) {
        LogError("width:%d", width_);
        return RET_ERR_NOT_SUPPORT;
    }

    height_ = properties.GetProperty("height", 0);
    if(height_ ==0 || height_%2 != 0) {
        LogError("height:%d", height_);
        return RET_ERR_NOT_SUPPORT;
    }

    fps_ = properties.GetProperty("fps", 25);
    b_frames_ = properties.GetProperty("b_frames", 0);
    bitrate_ = properties.GetProperty("bitrate", 500*1024);
    gop_ = properties.GetProperty("gop", fps_);
    pix_fmt_ = properties.GetProperty("pix_fmt", AV_PIX_FMT_YUV420P);

    codec_name_ = properties.GetProperty("codec_name", "default");
    // 查找H264编码器 确定是否存在
    if(codec_name_ == "default") {
        LogInfo("use default encoder");
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
    } else {
        LogInfo("use %s encoder", codec_name_.c_str());
        codec_ = avcodec_find_encoder_by_name(codec_name_.c_str());
    }
    if(!codec_) {
        LogError("can't find encoder");
        return RET_FAIL;
    }
    ctx_ = avcodec_alloc_context3(codec_);
    if(!ctx_) {
        LogError("ctx_ h264 avcodec_alloc_context3 failed");
        return RET_FAIL;
    }
    // 宽高
    ctx_->width = width_;
    ctx_->height = height_;

    // 码率
    ctx_->bit_rate = bitrate_;
    // gop
    ctx_->gop_size = gop_;
    // 帧率
    ctx_->framerate.num = fps_;        // 分子
    ctx_->framerate.den = 1;            // 分母
    // time_base
    ctx_->time_base.num = 1;            // 分子
    ctx_->time_base.den = fps_;          // 分母

    // 像素格式
    ctx_->pix_fmt = (AVPixelFormat)pix_fmt_;

    ctx_->codec_type = AVMEDIA_TYPE_VIDEO;

    ctx_->max_b_frames = b_frames_;

    // 设置编码参数
    av_dict_set(&dict_, "preset", "ultrafast", 0);  // 最快速度
    av_dict_set(&dict_, "tune", "zerolatency", 0);  // 最低延迟
    av_dict_set(&dict_, "profile", "baseline", 0);  // 使用基准配置，兼容性更好

    // 设置关键编码参数
    ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ctx_->flags2 |= AV_CODEC_FLAG2_LOCAL_HEADER;
    ctx_->max_b_frames = 0;  // 禁用B帧
    ctx_->gop_size = gop_;   // GOP大小
    ctx_->qmin = 10;
    ctx_->qmax = 51;

    // 初始化音视频编码器
    ret = avcodec_open2(ctx_, codec_, &dict_);//编码器会根据其配置生成SPS和PPS，并填充到AVCodecContext的extradata字段中
    if(ret < 0) {
        char buf[1024] = { 0 };
        av_strerror(ret, buf, sizeof(buf) - 1);
        LogError("avcodec_open2 failed:%s", buf);
        return RET_FAIL;
    }

    // 从extradata读取sps pps
    if(ctx_->extradata) {
        LogInfo("extradata_size:%d", ctx_->extradata_size);
        // 第一个为sps 7
        // 第二个为pps 8

        uint8_t *sps = ctx_->extradata + 4;    // 直接跳到SPS数据    +4跳过0x00 00 00 01
        int sps_len = 0;
        uint8_t *pps = NULL;                    // 直接跳到PPS数据
        int pps_len = 0;
        uint8_t *data = ctx_->extradata + 4;
        for (int i = 0; i < ctx_->extradata_size - 4; ++i)
        {
            if (0 == data[i] && 0 == data[i + 1] && 0 == data[i + 2] && 1 == data[i + 3])
            {
                pps = &data[i+4];
                break;
            }
        }
        sps_len = int(pps - sps) - 4;   // 4是00 00 00 01占用的字节
        pps_len = ctx_->extradata_size - 4*2 - sps_len;
        //将pps和sps分别存储
        sps_.append(sps, sps + sps_len);
        pps_.append(pps, pps + pps_len);
    }

    frame_ = av_frame_alloc(); //分配了一个 AVFrame 结构体，但帧数据的缓冲区尚未分配。
    frame_->width = width_;
    frame_->height = height_;
    frame_->format = ctx_->pix_fmt;
    ret = av_frame_get_buffer(frame_, 0);   //为 AVFrame 的数据缓冲区分配空间

    // 获取是否需要保存H264文件的配置
    save_h264_ = properties.GetProperty("save_h264", 0);
    h264_filename_ = properties.GetProperty("h264_filename", "output.h264");
    
    // 如果需要保存H264文件，打开文件
    if(save_h264_) {
        h264_fp_ = fopen(h264_filename_.c_str(), "wb");
        if(!h264_fp_) {
            LogError("Failed to open h264 file: %s", h264_filename_.c_str());
            return RET_FAIL;
        }
        
        // 写入SPS和PPS
        uint8_t start_code[] = {0, 0, 0, 1};
        fwrite(start_code, 1, 4, h264_fp_);
        fwrite(sps_.c_str(), 1, sps_.size(), h264_fp_);
        fwrite(start_code, 1, 4, h264_fp_);
        fwrite(pps_.c_str(), 1, pps_.size(), h264_fp_);
        fflush(h264_fp_);
    }

    return RET_OK;
}

AVPacket *H264Encoder::Encode(uint8_t *yuv, int size, int64_t pts, int *pkt_frame,RET_CODE *ret)
{
    int ret1 = 0;
    *ret = RET_OK;
    *pkt_frame = 0;

    if(yuv) {
        int need_size = 0;
        need_size = av_image_fill_arrays(frame_->data, frame_->linesize, yuv,
                                         (AVPixelFormat)frame_->format,
                                         frame_->width, frame_->height, 1);
        if(need_size != size)  {
            LogError("need_size:%d != size:%d", need_size, size);
            *ret = RET_FAIL;
            return NULL;
        }
        frame_->pts = pts;
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
        ret1 = avcodec_send_frame(ctx_, frame_);
    } else {
        ret1 = avcodec_send_frame(ctx_, NULL);
    }


    if(ret1 < 0) {  // <0 不能正常处理该frame
        char buf[1024] = { 0 };
        av_strerror(ret1, buf, sizeof(buf) - 1);
        LogError("avcodec_send_frame failed:%s", buf);
        *pkt_frame = 1;
        if(ret1 == AVERROR(EAGAIN)) {       // 你赶紧读取packet，我frame send不进去了
            *ret = RET_ERR_EAGAIN;
            return NULL;
        } else if(ret1 == AVERROR_EOF) {
            *ret = RET_ERR_EOF;
            return NULL;
        } else {
            *ret = RET_FAIL;            // 真正报错，这个encoder就只能销毁了
            return NULL;
        }
    }
    AVPacket *packet = av_packet_alloc();
    ret1 = avcodec_receive_packet(ctx_, packet);
    if(ret1 < 0) {
        LogError("AAC: avcodec_receive_packet ret:%d", ret1);
        av_packet_free(&packet);
        *pkt_frame = 0;
        if(ret1 == AVERROR(EAGAIN)) {       // 需要继续发送frame我们才有packet读取
            *ret = RET_ERR_EAGAIN;
            return NULL;
        }else if(ret1 == AVERROR_EOF) {
            *ret = RET_ERR_EOF;             // 不能在读取出来packet来了
            return NULL;
        } else {
            *ret = RET_FAIL;            // 真正报错，这个encoder就只能销毁了
            return NULL;
        }
    }else {
        *ret = RET_OK;
        if(*ret == RET_OK && packet) {
            // 如果需要保存H264文件，写入编码后的数据
            if(save_h264_ && h264_fp_) {
                uint8_t start_code[] = {0, 0, 0, 1};
                fwrite(start_code, 1, 4, h264_fp_);
                fwrite(packet->data, 1, packet->size, h264_fp_);
                fflush(h264_fp_);
            }
        }
        return packet;
    }
}
AVPacket *H264Encoder::Encode1(AVFrame *yuv_frame_, int size, int64_t pts, int *pkt_frame,RET_CODE *ret)
{
    int ret1 = 0;
    *ret = RET_OK;
    *pkt_frame = 0;

    if(yuv_frame_) {
        yuv_frame_->pts = pts;
        yuv_frame_->pict_type = AV_PICTURE_TYPE_NONE;
        ret1 = avcodec_send_frame(ctx_, yuv_frame_);
    } else {
        ret1 = avcodec_send_frame(ctx_, NULL);
    }


    if(ret1 < 0) {  // <0 不能正常处理该frame
        char buf[1024] = { 0 };
        av_strerror(ret1, buf, sizeof(buf) - 1);
        LogError("avcodec_send_frame failed:%s", buf);
        *pkt_frame = 1;
        if(ret1 == AVERROR(EAGAIN)) {       // 你赶紧读取packet，我frame send不进去了
            *ret = RET_ERR_EAGAIN;
            return NULL;
        } else if(ret1 == AVERROR_EOF) {
            *ret = RET_ERR_EOF;
            return NULL;
        } else {
            *ret = RET_FAIL;            // 真正报错，这个encoder就只能销毁了
            return NULL;
        }
    }
    
    AVPacket *packet = av_packet_alloc();
    ret1 = avcodec_receive_packet(ctx_, packet);
    if(ret1 < 0) {
        LogError("H264: avcodec_receive_packet ret:%d", ret1);
        av_packet_free(&packet);
        *pkt_frame = 0;
        if(ret1 == AVERROR(EAGAIN)) {       // 需要继续发送frame我们才有packet读取
            *ret = RET_ERR_EAGAIN;
            return NULL;
        }else if(ret1 == AVERROR_EOF) {
            *ret = RET_ERR_EOF;             // 不能在读取出来packet来了
            return NULL;
        } else {
            *ret = RET_FAIL;            // 真正报错，这个encoder就只能销毁了
            return NULL;
        }
    }else {
        *ret = RET_OK;
        if(*ret == RET_OK && packet) {
            // 如果需要保存H264文件，写入编码后的数据
            if(save_h264_ && h264_fp_) {
                uint8_t start_code[] = {0, 0, 0, 1};
                fwrite(start_code, 1, 4, h264_fp_);
                fwrite(packet->data, 1, packet->size, h264_fp_);
                fflush(h264_fp_);
            }
        }
        
        return packet;
    }
}
