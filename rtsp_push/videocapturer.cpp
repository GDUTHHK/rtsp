#include "videocapturer.h"
#include "dlog.h"
#include "timesutil.h"
#include "avpublishtime.h"
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

VideoCapturer::VideoCapturer()
{
    // 注册设备
    avdevice_register_all();
}

VideoCapturer::~VideoCapturer()
{
    CloseCamera();
    if(sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if(frame_) {
        av_frame_free(&frame_);
    }
    if(yuv_frame_) {
        av_frame_free(&yuv_frame_);
    }
}

RET_CODE VideoCapturer::Init(const Properties &properties)
{
    // 获取参数
    device_name_ = properties.GetProperty("device_name", "/dev/video0");
    width_ = properties.GetProperty("width", 1280);
    height_ = properties.GetProperty("height", 720);
    pixel_format_ = properties.GetProperty("pixel_format", AV_PIX_FMT_YUV420P);
    fps_ = properties.GetProperty("fps", 25);
    frame_duration_ = 1000.0 / fps_;

    // 分配缓冲区
    yuv_buf_size_ = width_ * height_ * 3 / 2; // YUV420格式
    yuv_buf_ = new uint8_t[yuv_buf_size_];

    // 打开摄像头
    if(OpenCamera() != RET_OK) {
        LogError("Failed to open camera");
        return RET_FAIL;
    }

    return RET_OK;
}

RET_CODE VideoCapturer::OpenCamera()
{
    int ret;
    AVDictionary *options = nullptr;
    
    // 设置摄像头参数
    char resolution[32];
    snprintf(resolution, sizeof(resolution), "%dx%d", width_, height_);
    av_dict_set(&options, "video_size", resolution, 0);
    av_dict_set(&options, "framerate", std::to_string(fps_).c_str(), 0);
    av_dict_set(&options, "input_format", "yuyv422", 0);

    // 打开摄像头设备
    AVInputFormat *ifmt = av_find_input_format("v4l2");
    if (!ifmt) {
        LogError("Cannot find v4l2 input format");
        return RET_FAIL;
    }

    ret = avformat_open_input(&fmt_ctx_, device_name_.c_str(), ifmt, &options);
    if (ret < 0) {
        LogError("Cannot open camera");
        return RET_FAIL;
    }

    // 获取视频流信息
    ret = avformat_find_stream_info(fmt_ctx_, NULL);
    if (ret < 0) {
        LogError("Could not get stream info");
        return RET_FAIL;
    }

    // 查找视频流
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; i++) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    if (video_stream_idx == -1) {
        LogError("Could not find video stream");
        return RET_FAIL;
    }

    // 获取解码器参数
    AVCodecParameters *codecParams = fmt_ctx_->streams[video_stream_idx]->codecpar;

    // 查找解码器
    const AVCodec *codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        LogError("Could not find codec");
        return RET_FAIL;
    }

    // 创建解码器上下文
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        LogError("Could not allocate video codec context");
        return RET_FAIL;
    }

    // 将解码器参数复制到上下文
    ret = avcodec_parameters_to_context(codec_ctx_, codecParams);
    if (ret < 0) {
        LogError("Could not copy codec params to codec context");
        return RET_FAIL;
    }

    // 打开解码器
    ret = avcodec_open2(codec_ctx_, codec, NULL);
    if (ret < 0) {
        LogError("Could not open codec");
        return RET_FAIL;
    }

    // 分配转换用的Frame
    frame_ = av_frame_alloc();
    yuv_frame_ = av_frame_alloc();
    yuv_frame_->format = AV_PIX_FMT_YUV420P;
    yuv_frame_->width = width_;
    yuv_frame_->height = height_;
    ret = av_frame_get_buffer(yuv_frame_, 0);
    if (ret < 0) {
        LogError("Could not allocate frame data");
        return RET_FAIL;
    }

    // 创建格式转换上下文
    sws_ctx_ = sws_getContext(width_, height_, AV_PIX_FMT_YUYV422,
                             width_, height_, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        LogError("Cannot create sws context");
        return RET_FAIL;
    }

    return RET_OK;
}

void VideoCapturer::CloseCamera()
{
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (yuv_frame_) {
        av_frame_free(&yuv_frame_);
        yuv_frame_ = nullptr;
    }

    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    if (yuv_buf_) {
        delete[] yuv_buf_;
        yuv_buf_ = nullptr;
    }
}

void VideoCapturer::Loop()
{
    AVPacket *packet = av_packet_alloc();
    
    while (!request_abort_) {
        int ret = av_read_frame(fmt_ctx_, packet);
        if (ret < 0) {
            LogError("Failed to read frame");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 解码视频帧
        ret = avcodec_send_packet(codec_ctx_, packet);
        if (ret < 0) {
            LogError("Error sending packet for decoding");
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                LogError("Error during decoding");
                break;
            }
            
            // 转换格式到YUV420P
            sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, height_,
                     yuv_frame_->data, yuv_frame_->linesize);

            // // 回调数据
            // if (callback_) {
            //     callback_(yuv_frame_->data[0], yuv_buf_size_);
            // }

            
            // 回调数据
            if (frame_callback_) {
                frame_callback_(yuv_frame_, yuv_buf_size_);
            }
        }

        av_packet_unref(packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(frame_duration_)));
    }

    av_packet_free(&packet);
}

void VideoCapturer::AddCallback(function<void(uint8_t*, int32_t)> callback)
{
    callback_ = callback;
}

void VideoCapturer::AddCallback1(function<void(AVFrame *, int32_t)> callback)
{
    frame_callback_ = callback; 
}
