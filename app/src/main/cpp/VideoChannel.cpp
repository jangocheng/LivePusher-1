//
// Created by 李志丹 on 2018/11/22.
//

#include "VideoChannel.h"

VideoChannel::VideoChannel() {
    pthread_mutex_init(&mutex, 0);
}

VideoChannel::~VideoChannel() {
    pthread_mutex_destroy(&mutex);

    if (videoCodec) {
        x264_encoder_close(videoCodec);
        videoCodec = 0;
    }
    if (pic_in) {
        x264_picture_clean(pic_in);
        DELETE(pic_in);
        pic_in = 0;
    }
}

void VideoChannel::encodeData(int8_t *data) {
    LOGE("encodeData ySize:%d",ySize);
    pthread_mutex_lock(&mutex);
    //y数据
    memcpy(pic_in->img.plane[0], data, static_cast<size_t>(ySize));

    for (int i = 0; i < uvSize; ++i) {
        //u数据
        *(pic_in->img.plane[1] + i) = static_cast<uint8_t>(*(data + ySize + 1 + i * 2));
        //v数据
        *(pic_in->img.plane[2] + i) = static_cast<uint8_t>(*(data + ySize + i * 2));
    }

    pic_in->i_pts = index++;

    x264_nal_t *pp_nal;
    //编码出多少个 nalu(比帧更小的单位:片,NALU其实又是片的一个集合概念)
    int pi_nal = 0;
    x264_picture_t pic_out;
    //编码
    int ret = x264_encoder_encode(videoCodec, &pp_nal, &pi_nal, pic_in, &pic_out);
    if (ret < 0) {
        pthread_mutex_unlock(&mutex);
        return;
    }


    int sps_len = 0;
    int pps_len = 0;
    uint8_t sps[100];
    uint8_t pps[100];
    //sps和pps是指导解码器是要如何解码的
    for (int i = 0; i < pi_nal; ++i) {
        if (pp_nal[i].i_type == NAL_SPS) {
            //间隔符 去掉 00 00 00 01 i_payload是index p_payload是内容
            sps_len = pp_nal[i].i_payload - 4;
            memcpy(sps, pp_nal[i].p_payload + 4, static_cast<size_t>(sps_len));
        } else if (pp_nal[i].i_type == NAL_PPS) {
            pps_len = pp_nal[i].i_payload - 4;
            memcpy(pps, pp_nal[i].p_payload + 4, static_cast<size_t>(pps_len));
            //拿到了pps 就表示sps已经拿到了，发送
            sendSpsPps(sps, pps, sps_len, pps_len);
        } else {
            sendFrame(pp_nal[i].i_type, pp_nal[i].i_payload, pp_nal[i].p_payload);
        }
    }


    pthread_mutex_unlock(&mutex);
}

//发送sps pps
void VideoChannel::sendSpsPps(uint8_t *sps, uint8_t *pps, int sps_len, int pps_len) {
    //13：sps前的一段描述参数，例如版本，兼容性等待，共13个字节
    //3: pps前的一段描述参数  ---  看表
    int bodySize = 13 + sps_len + 3 + pps_len;

    //rtmp包
    RTMPPacket *packet = new RTMPPacket;
    //申请内存
    RTMPPacket_Alloc(packet, bodySize);

    //======填充sps/pps数据（固定格式）
    int i = 0;
    //固定头
    packet->m_body[i++] = 0x17;
    //类型
    packet->m_body[i++] = 0x00;
    //composition time 0x000000
    packet->m_body[i++] = 0x00;
    packet->m_body[i++] = 0x00;
    packet->m_body[i++] = 0x00;

    //版本
    packet->m_body[i++] = 0x01;
    //编码规格
    packet->m_body[i++] = sps[1];
    packet->m_body[i++] = sps[2];
    packet->m_body[i++] = sps[3];
    packet->m_body[i++] = 0xFF;
    //整个sps
    packet->m_body[i++] = 0xE1;
    //sps长度
    packet->m_body[i++] = (sps_len >> 8) & 0xFF;
    packet->m_body[i++] = sps_len & 0xFF;
    memcpy(&packet->m_body[i], sps, sps_len);
    i += sps_len;

    //pps
    packet->m_body[i++] = 0x01;
    packet->m_body[i++] = (pps_len >> 8) & 0xff;
    packet->m_body[i++] = pps_len & 0xff;
    memcpy(&packet->m_body[i], pps, pps_len);
    //======填充sps/pps数据 完



    //视频
    packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
    packet->m_nBodySize = static_cast<uint32_t>(bodySize);
    //随意分配一个管道（尽量避开rtmp.c中使用的）
    packet->m_nChannel = 10;
    //sps/pps没有时间戳
    packet->m_nTimeStamp = 0;
    //不使用绝对时间
    packet->m_hasAbsTimestamp = 0;
    packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
    videoCallback(packet);
}


void VideoChannel::setVideoEncInfo(int width, int height, int fps, int bitrate) {
    pthread_mutex_lock(&mutex);
    mWidth = width;
    mHeight = height;
    mFps = fps;
    mBitrate = bitrate;
    ySize = width * height;
    uvSize = ySize / 4;

    //切换摄像头时，会重复打开编码器。在这里先释放一下
    if (videoCodec) {
        x264_encoder_close(videoCodec);
    }
    if (pic_in) {
        x264_picture_clean(pic_in);
        DELETE(pic_in);
        pic_in = 0;
    }

    //打开x264编码器
    //x264编码器的属性
    x264_param_t param;
    //2:最快 3:无延迟编码 (看源码)  x264_preset_names[0], x264_tune_names[7]
    x264_param_default_preset(&param,"ultrafast", "zerolatency");
    //base_line 3.2 编码规格（3.2支持1280x720 5.2支持2k）
    param.i_level_idc = 32;
    //输入数据格式(这里改成NV21也没用，不能编码)
    param.i_csp = X264_CSP_I420;
    //宽高
    param.i_width = width;
    param.i_height = height;
    //无b帧(双向预测帧)
    param.i_bframe = 0;
    //参数i_rc_method表示码率控制。CQP（恒定质量），CRF（恒定码率），ABR（平均码率）
    param.rc.i_rc_method = X264_RC_ABR;
    //设置码率（单位Kbps）
    param.rc.i_bitrate = bitrate / 1000;
    //瞬时最大码率
    param.rc.i_vbv_max_bitrate = static_cast<int>(bitrate / 1000 * 1.2);
    //设置了i_vbv_max_bitrate必须设置此参数，码率控制区大小，单位Kbps
    param.rc.i_vbv_buffer_size = bitrate / 1000;
    //** 是否复制sps和pps放在每个关键帧的前面，该参数设置是让每个关键帧（I帧）都附带sps/pps
    param.b_repeat_headers = 1;
    //帧率
    param.i_fps_num = static_cast<uint32_t>(fps);
    param.i_fps_den = 1;
    param.i_timebase_den = param.i_fps_num;
    param.i_timebase_num = param.i_fps_den;
    //用fps而不是时间戳来计算帧间距
    param.b_vfr_input = 0;
    //帧距离（关键帧） 设置为2意味着2秒一个关键帧
    param.i_keyint_max = fps * 2;
    //多线程
    param.i_threads = 1;

    //static const char * const x264_profile_names[] = { "baseline", "main", "high", "high10", "high422", "high444", 0 };
    x264_param_apply_profile(&param, x264_profile_names[0]);
    //创建解码器
    videoCodec = x264_encoder_open(&param);
    pic_in = new x264_picture_t;
    x264_picture_alloc(pic_in, X264_CSP_I420, width, height);
    pthread_mutex_unlock(&mutex);
}

void VideoChannel::setVideoCallback(VideoChannel::VideoCallback callback) {
    this->videoCallback = callback;
}

void VideoChannel::sendFrame(int type, int i_payload, uint8_t *p_payload) {
    //确定分隔符长度 既是00 00 00 01还是00 00 01
    if (p_payload[2] == 0x00) {
        i_payload -= 4;//00 00 00 01
        p_payload += 4;
    } else if (p_payload[2] == 0x01) {
        i_payload -= 3;//00 00 01
        p_payload += 3;
    }

    //看表 和sendSpsPps一样
    int bodySize = 9 + i_payload;
    RTMPPacket *packet = new RTMPPacket;
    RTMPPacket_Alloc(packet, bodySize);
    RTMPPacket_Reset(packet);

    //判断关键帧
    packet->m_body[0] = 0x27;//非关键帧
    if (type == NAL_SLICE_IDR) {
        packet->m_body[0] = 0x17;//关键帧
    }
    //类型
    packet->m_body[1] = 0x01;
    //时间戳 3字节 空白
    packet->m_body[2] = 0x00;
    packet->m_body[3] = 0x00;
    packet->m_body[4] = 0x00;
    //数据长度 int 4字节
    packet->m_body[5] = (i_payload >> 24) & 0xff;
    packet->m_body[6] = (i_payload >> 16) & 0xff;
    packet->m_body[7] = (i_payload >> 8) & 0xff;
    packet->m_body[8] = i_payload & 0xff;
    //图片数据
    memcpy(&packet->m_body[9], p_payload, static_cast<size_t>(i_payload));

    packet->m_hasAbsTimestamp = 0;
    packet->m_nBodySize = static_cast<uint32_t>(bodySize);
    packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
    packet->m_nChannel = 0x10;
    packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
    videoCallback(packet);


}







