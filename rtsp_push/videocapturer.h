#ifndef VIDEOCAPTURER_H
#define VIDEOCAPTURER_H

#include <functional>
#include "commonlooper.h"
#include "mediabase.h"
extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

using std::function;

class VideoCapturer: public CommonLooper
{
public:
    VideoCapturer();
    virtual ~VideoCapturer();
    /**
     * @brief Init
     * @param "device_name", 设备名称，如"/dev/video0"
     *          "width", 宽度，缺省为1280
     *          "height", 高度，缺省为720
     *          "format", 像素格式，AVPixelFormat对应的值，缺省为AV_PIX_FMT_YUV420P
     *          "fps", 帧数，缺省为25
     * @return
     */
    RET_CODE Init(const Properties& properties);
    virtual void Loop();
    void AddCallback(function<void(uint8_t*, int32_t)> callback);
    void AddCallback1(function<void(AVFrame*, int32_t)> callback);
private:
    RET_CODE OpenCamera();
    void CloseCamera();
    
    std::string device_name_;
    int width_ = 1280;
    int height_ = 720;
    int pixel_format_ = AV_PIX_FMT_YUV420P;
    int fps_ = 25;
    double frame_duration_ = 40;

    AVFormatContext *fmt_ctx_ = nullptr;
    AVCodecContext *codec_ctx_ = nullptr;
    AVPacket *packet_ = nullptr;
    uint8_t *yuv_buf_ = nullptr;
    int yuv_buf_size_ = 0;

    function<void(uint8_t*, int32_t)> callback_ = nullptr;
    function<void(AVFrame *, int32_t)> frame_callback_ = nullptr;
    bool is_first_frame_ = false;

    // 添加格式转换相关成员
    SwsContext *sws_ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVFrame *yuv_frame_ = nullptr;
};

#endif // VIDEOCAPTURER_H
