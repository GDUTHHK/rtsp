﻿#include <functional>
#include "pushwork.h"
#include "dlog.h"
#include "avpublishtime.h"

PushWork::PushWork(MessageQueue *msg_queue)
    :msg_queue_(msg_queue)
{

}

PushWork::~PushWork()
{
    // 从源头开始释放资源
    // 先释放音频、视频捕获
    if(audio_capturer_) {
        delete audio_capturer_;
    }
    if(video_capturer_) {
        delete video_capturer_;
    }
    if(audio_encoder_) {
        delete audio_encoder_;
    }
    if(fltp_buf_) {
        av_free(fltp_buf_);
    }
    if(audio_frame_) {
        av_frame_free(&audio_frame_);
    }
    if(video_encoder_) {
        delete video_encoder_;
    }
    if(rtsp_pusher_) {
        delete rtsp_pusher_;
    }
    if(h264_fp_) {
        fclose(h264_fp_);
        h264_fp_ = NULL;
    }
    LogInfo("~PushWork()");
}

RET_CODE PushWork::Init(const Properties &properties)
{
    int ret = 0;
    // 音频test模式
    audio_test_ = properties.GetProperty("audio_test", 0);
    input_pcm_name_ = properties.GetProperty("input_pcm_name", "input_48k_2ch_s16.pcm");

    // 麦克风采样属性
    mic_sample_rate_ = properties.GetProperty("mic_sample_rate", 48000);
    mic_sample_fmt_ = properties.GetProperty("mic_sample_fmt", AV_SAMPLE_FMT_S16);
    mic_channels_ = properties.GetProperty("mic_channels", 2);

    // 音频编码参数
    audio_sample_rate_ = properties.GetProperty("audio_sample_rate", mic_sample_rate_);
    audio_bitrate_ = properties.GetProperty("audio_bitrate", 128*1024);
    audio_channels_ = properties.GetProperty("audio_channels", mic_channels_);
    audio_ch_layout_ = av_get_default_channel_layout(audio_channels_);    // 由audio_channels_决定

    // 视频test模式
    video_test_ = properties.GetProperty("video_test", 0);
    input_yuv_name_ = properties.GetProperty("input_yuv_name", "input_1280_720_420p.yuv");

    // 桌面录制属性
    desktop_x_ = properties.GetProperty("desktop_x", 0);
    desktop_y_ = properties.GetProperty("desktop_y", 0);
    desktop_width_  = properties.GetProperty("desktop_width", 1280);
    desktop_height_ = properties.GetProperty("desktop_height", 720);
    desktop_format_ = properties.GetProperty("desktop_pixel_format", AV_PIX_FMT_YUV420P);
    desktop_fps_ = properties.GetProperty("desktop_fps", 25);

    // 视频编码属性
    video_width_  = properties.GetProperty("video_width", desktop_width_);     // 宽
    video_height_ = properties.GetProperty("video_height", desktop_height_);   // 高
    video_fps_ = properties.GetProperty("video_fps", desktop_fps_);             // 帧率
    video_gop_ = properties.GetProperty("video_gop", video_fps_);
    video_bitrate_ = properties.GetProperty("video_bitrate", 1024*1024);   // 先默认1M fixedme
    video_b_frames_ = properties.GetProperty("video_b_frames", 0);   // b帧数量

    //推流设置
    rtsp_url_       = properties.GetProperty("rtsp_url", "");
    rtsp_transport_ = properties.GetProperty("rtsp_transport", "");
    rtsp_timeout_ = properties.GetProperty("rtsp_timeout", 5000);
    rtsp_max_queue_duration_ = properties.GetProperty("rtsp_max_queue_duration", 500);

    // 初始化publish time
    AVPublishTime::GetInstance()->Rest();   // 推流打时间戳的问题

    // 设置音频编码器，先音频捕获初始化
    audio_encoder_ = new AACEncoder();
    if(!audio_encoder_)
    {
        LogError("new AACEncoder() failed");
        return RET_FAIL;
    }

    Properties  aud_codec_properties;
    aud_codec_properties.SetProperty("sample_rate", audio_sample_rate_);
    aud_codec_properties.SetProperty("channels", audio_channels_);
    aud_codec_properties.SetProperty("bitrate", audio_bitrate_);        // 这里没有去设置采样格式
    // 需要什么样的采样格式是从编码器读取出来的
    if(audio_encoder_->Init(aud_codec_properties) != RET_OK)
    {
        LogError("AACEncoder Init failed");
        return RET_FAIL;
    }

    int frame_bytes2 = 0;
    // 默认读取出来的数据是s16的，编码器需要的是fltp, 需要做重采样
    // 手动把s16转成fltp
    fltp_buf_size_ = av_samples_get_buffer_size(NULL, audio_encoder_->GetChannels(),
                                                audio_encoder_->GetFrameSamples(),
                                                (enum AVSampleFormat)audio_encoder_->GetFormat(), 1);
    fltp_buf_ = (uint8_t *)av_malloc(fltp_buf_size_);
    if(!fltp_buf_) {
        LogError("fltp_buf_ av_malloc failed");
        return RET_ERR_OUTOFMEMORY;
    }

    audio_frame_ = av_frame_alloc();
    audio_frame_->format = audio_encoder_->GetFormat();
    audio_frame_->format = AV_SAMPLE_FMT_FLTP;
    audio_frame_->nb_samples = audio_encoder_->GetFrameSamples();
    audio_frame_->channels = audio_encoder_->GetChannels();
    audio_frame_->channel_layout = audio_encoder_->GetChannelLayout();
    frame_bytes2  = audio_encoder_->GetFrameBytes();
    if(fltp_buf_size_ != frame_bytes2) {
        LogError("frame_bytes1:%d != frame_bytes2:%d", fltp_buf_size_, frame_bytes2);
        return RET_FAIL;
    }
    ret = av_frame_get_buffer(audio_frame_, 0);
    if(ret < 0) {
        LogError("audio_frame_ av_frame_get_buffer failed");
        return RET_FAIL;
    }

    // 初始化视频编码器
    video_encoder_ = new H264Encoder();
    Properties  vid_codec_properties;
    vid_codec_properties.SetProperty("width", video_width_);
    vid_codec_properties.SetProperty("height", video_height_);
    vid_codec_properties.SetProperty("fps", video_fps_);            // 帧率
    vid_codec_properties.SetProperty("b_frames", video_b_frames_);
    vid_codec_properties.SetProperty("bitrate", video_bitrate_);    // 码率
    vid_codec_properties.SetProperty("gop", video_gop_);            // gop
    if(video_encoder_->Init(vid_codec_properties) != RET_OK)
    {
        LogError("H264Encoder Init failed");
        return RET_FAIL;
    }

    // 在音视频编码器初始化完， 音视频捕获前
    rtsp_pusher_ =new RtspPusher(msg_queue_);
    if(!rtsp_pusher_) {
        LogError("new RTSPPusher() failed");
        return RET_FAIL;
    }

    Properties  rtsp_properties;
    rtsp_properties.SetProperty("url", rtsp_url_);//推流地址
    rtsp_properties.SetProperty("timeout", rtsp_timeout_);//超时时长
    rtsp_properties.SetProperty("rtsp_transport", rtsp_transport_);//UDP还是TCP
    rtsp_properties.SetProperty("max_queue_duration", rtsp_max_queue_duration_);//最大帧队列

    int audio_frame_samples=audio_encoder_->GetFrameSamples();
    int audio_sample_rate=audio_encoder_->GetSampleRate();
    int audio_size=audio_encoder_->GetFrameSamples()*1000/audio_encoder_->GetSampleRate();
    printf("audio_size:%d\n",audio_size);
    printf("audio_frame_samples:%d\n",audio_frame_samples);
    printf("audio_sample_rate:%d\n",audio_sample_rate);
    if(audio_encoder_) {
        rtsp_properties.SetProperty("audio_frame_duration",
                                    audio_size);//音频每帧时长
    }
    if(video_encoder_) {
        rtsp_properties.SetProperty("video_frame_duration",
                                    1000/video_encoder_->GetFps());//视频每帧时长
    }

    if(rtsp_pusher_->Init(rtsp_properties) != RET_OK) {
        LogError("rtsp_pusher_->Init failed");
        return RET_FAIL;
    }

    // 创建音频流、音视频流
    if(video_encoder_) {
        if(rtsp_pusher_->ConfigVideoStream(video_encoder_->GetCodecContext()) != RET_OK) {//将视频流与编码器上下文绑定
            LogError("rtsp_pusher ConfigVideoSteam failed");
            return RET_FAIL;
        }
    }
    if(audio_encoder_) {
        if(rtsp_pusher_->ConfigAudioStream(audio_encoder_->GetCodecContext()) != RET_OK) {//将音频流与编码器上下文呢绑定
            LogError("rtsp_pusher ConfigAudioStream failed");
            return RET_FAIL;
        }
    }

    //连接网络
    if(rtsp_pusher_->Connect() != RET_OK) {
        LogError("rtsp_pusher Connect() failed");
        return RET_FAIL;
    }

    // 设置音频捕获
    audio_capturer_ = new AudioCapturer();
    Properties aud_cap_properties;
    aud_cap_properties.SetProperty("audio_test", 1);
    aud_cap_properties.SetProperty("input_pcm_name", input_pcm_name_);
    aud_cap_properties.SetProperty("channels", mic_channels_);
    aud_cap_properties.SetProperty("nb_samples", 1024);     // 由编码器提供 // fix me
    aud_cap_properties.SetProperty("format", mic_sample_fmt_);
    aud_cap_properties.SetProperty("byte_per_sample", 2);   // fix me
    if(audio_capturer_->Init(aud_cap_properties) != RET_OK) //初始化读取音频设备的参数  并且 打开读取音频文件
    {
        LogError("AudioCapturer Init failed");
        return RET_FAIL;
    }

    //音频线程的回调函数
    audio_capturer_->AddCallback(std::bind(&PushWork::PcmCallback, this, std::placeholders::_1,
                                           std::placeholders::_2));

    // if(audio_capturer_->Start()!= RET_OK) {   //开启读取音频线程
    //     LogError("AudioCapturer Start failed");
    //     return RET_FAIL;
    // }

    video_capturer_ = new VideoCapturer();
    Properties  vid_cap_properties;
    vid_cap_properties.SetProperty("video_test", 1);
    vid_cap_properties.SetProperty("input_yuv_name", input_yuv_name_);
    vid_cap_properties.SetProperty("width", desktop_width_);
    vid_cap_properties.SetProperty("height", desktop_height_);
    if(video_capturer_->Init(vid_cap_properties) != RET_OK)//初始化读取视频设备的参数  并且 打开读取视频文件
    {
        LogError("VideoCapturer Init failed");
        return RET_FAIL;
    }
    //    video_nalu_buf = new uint8_t[VIDEO_NALU_BUF_MAX_SIZE];

    //视频线程的回调函数
    video_capturer_->AddCallback1(std::bind(&PushWork::YuvCallback1, this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
    if(video_capturer_->Start()!= RET_OK) {  //开启读取音频线程
        LogError("VideoCapturer Start failed");
        return RET_FAIL;
    }

    return RET_OK;
}

RET_CODE PushWork::DeInit()
{
    if(audio_capturer_) {
        audio_capturer_->Stop();
        delete audio_capturer_;
        audio_capturer_ = NULL;
    }
    if(video_capturer_){
        video_capturer_->Stop();
        delete video_capturer_;
        video_capturer_ = NULL;
    }
}
// 只支持2通道 s16交错模式 -> float planar格式
void s16le_convert_to_fltp(short *s16le, float *fltp, int nb_samples) {
    float *fltp_l = fltp;   // -1~1
    float *fltp_r = fltp + nb_samples;
    for(int i = 0; i < nb_samples; i++) {
        fltp_l[i] = s16le[i*2]/32768.0;     // 0 2 4
        fltp_r[i] = s16le[i*2+1]/32768.0;   // 1 3 5
    }
}
void PushWork::PcmCallback(uint8_t *pcm, int32_t size)
{
    int ret = 0;
    if(!pcm_s16le_fp_)
    {
        pcm_s16le_fp_ = fopen("push_dump_s16le.pcm", "wb");
    }
    if(pcm_s16le_fp_)
    {
        // ffplay -ar 48000 -channels 2 -f s16le  -i push_dump_s16le.pcm
        fwrite(pcm, 1, size, pcm_s16le_fp_);
        fflush(pcm_s16le_fp_);//确保数据立即被写入文件，而不是等到缓冲区满了或者文件被关闭。
    }
    // 这里就约定好，音频捕获的时候，采样点数和编码器需要的点数是一样的
    s16le_convert_to_fltp((short *)pcm, (float *)fltp_buf_, audio_frame_->nb_samples);
    ret = av_frame_make_writable(audio_frame_);
    if(ret < 0) {
        LogError("av_frame_make_writable failed");
        return;
    }
    // 将fltp_buf_写入frame   需要手动释放
    ret = av_samples_fill_arrays(audio_frame_->data,//根据帧格式将fltp_buf缓冲区的数据填入到frame的buf中
                                 audio_frame_->linesize,
                                 fltp_buf_,
                                 audio_frame_->channels,
                                 audio_frame_->nb_samples,
                                 (AVSampleFormat)audio_frame_->format,
                                 0);
    if(ret < 0) {
        LogError("av_samples_fill_arrays failed");
        return;
    }

    int64_t pts = (int64_t)AVPublishTime::GetInstance()->get_audio_pts();
    int pkt_frame = 0;
    RET_CODE encode_ret = RET_OK;
    AVPacket *packet = audio_encoder_->Encode(audio_frame_, pts, 0, &pkt_frame, &encode_ret); //0为是否刷新
    if(encode_ret == RET_OK && packet) {
        if(!aac_fp_) {
            aac_fp_ = fopen("push_dump.aac", "wb");//存储编码好的aac音频文件
            if(!aac_fp_) {
                LogError("fopen push_dump.aac failed");
                return;
            }
        }
        if(aac_fp_) {
            uint8_t adts_header[7];
            if(audio_encoder_->GetAdtsHeader(adts_header, packet->size) != RET_OK) {
                LogError("GetAdtsHeader failed");
                return;
            }
            fwrite(adts_header, 1, 7, aac_fp_);
            fwrite(packet->data, 1, packet->size, aac_fp_);
        }
    }
    //    LogInfo("PcmCallback pts:%ld", pts);
    if(packet) {
//        LogInfo("PcmCallback packet->pts:%ld", packet->pts);
        rtsp_pusher_->Push(packet, E_AUDIO_TYPE);
    }else {
        LogInfo("packet is null");
    }
}

void PushWork::YuvCallback(uint8_t *yuv, int32_t size)
{
    int64_t pts = (int64_t)AVPublishTime::GetInstance()->get_video_pts();
    int pkt_frame = 0;
    RET_CODE encode_ret = RET_OK;
    AVPacket *packet = video_encoder_->Encode(yuv, size, pts, &pkt_frame, &encode_ret);
    if(packet) {
        // 保存H264文件
        if(!h264_fp_) {
            h264_fp_ = fopen("camera_output.h264", "wb");
            if(!h264_fp_) {
                LogError("Failed to open h264 file");
                return;
            }
            // 写入SPS和PPS
            uint8_t start_code[] = {0, 0, 0, 1};
            fwrite(start_code, 1, 4, h264_fp_);
            fwrite(video_encoder_->get_sps_data(), 1, video_encoder_->get_sps_size(), h264_fp_);
            fwrite(start_code, 1, 4, h264_fp_);
            fwrite(video_encoder_->get_pps_data(), 1, video_encoder_->get_pps_size(), h264_fp_);
        }

        // 写入编码后的数据
        uint8_t start_code[] = {0, 0, 0, 1};
        fwrite(start_code, 1, 4, h264_fp_);
        fwrite(packet->data, 1, packet->size, h264_fp_);
        fflush(h264_fp_);

        rtsp_pusher_->Push(packet, E_VIDEO_TYPE);
    }
}
void PushWork::YuvCallback1(AVFrame *frame, int32_t size) {
    int64_t pts = (int64_t)AVPublishTime::GetInstance()->get_video_pts();
    int pkt_frame = 0;
    RET_CODE encode_ret = RET_OK;
    
    if(frame) {
        AVPacket *packet = video_encoder_->Encode1(frame, size, pts, &pkt_frame, &encode_ret);
      if(packet) {
            // 写入编码后的数据
            if(!h264_fp_) {
                h264_fp_ = fopen("camera_output.h264", "wb");
                if(!h264_fp_) {
                    LogError("Failed to open h264 file");
                    return;
                }
                 // 写入SPS和PPS
                uint8_t start_code[] = {0, 0, 0, 1};
                fwrite(start_code, 1, 4, h264_fp_);
                fwrite(video_encoder_->get_sps_data(), 1, video_encoder_->get_sps_size(), h264_fp_);
                fwrite(start_code, 1, 4, h264_fp_);
                fwrite(video_encoder_->get_pps_data(), 1, video_encoder_->get_pps_size(), h264_fp_);
            }else {
                uint8_t start_code[] = {0, 0, 0, 1};
                fwrite(start_code, 1, 4, h264_fp_);
                fwrite(packet->data, 1, packet->size, h264_fp_);
                fflush(h264_fp_);
            }

            rtsp_pusher_->Push(packet, E_VIDEO_TYPE);
        }else {
                LogError("video packet is null");
            }   
    }
}
