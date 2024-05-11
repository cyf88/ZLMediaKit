/*
* Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
*
* This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
*
* Use of this source code is governed by MIT-like license that can be found in the
* LICENSE file in the root of the source tree. All contributing project authors
* may be found in the AUTHORS file in the root of the source tree.
*/

#ifndef ZLMEDIAKIT_SVAC_H
#define ZLMEDIAKIT_SVAC_H

#include "Extension/Frame.h"
#include "Extension/Track.h"

#define SVAC_TYPE(v) ((uint8_t)(v>>2)&0x0F)

namespace mediakit{

void splitSVAC(const char *ptr, size_t len, size_t prefix, const std::function<void(const char *, size_t, size_t)> &cb);


template<typename Parent>
class SVACFrameHelper : public Parent{
public:
   friend class FrameImp;
   friend class toolkit::ResourcePool_l<SVACFrameHelper>;
   using Ptr = std::shared_ptr<SVACFrameHelper>;

   enum {
       NAL_NONE_IDR = 1,
       NAL_IDR = 2,
       NAL_NONE_IDR_SVC = 3,
       NAL_IDR_SVC = 4,
       NAL_SURVEI_UNIT = 5,
       NAL_SEI = 6,
       NAL_SPS = 7,
       NAL_PPS = 8,
       NAL_SES = 9,
       NAL_AUTH = 10,
       NAL_E0STREAM = 11,
       NAL_RESERVED1 = 12,
       NAL_CHAPTER6 = 13,
       NAL_RESERVED2 = 14,
       NAL_SVC_PPS = 15,
   };

   template<typename ...ARGS>
   SVACFrameHelper(ARGS &&...args): Parent(std::forward<ARGS>(args)...) {
       this->_codec_id = CodecSVAC;
   }

   bool keyFrame() const override {
       auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
       return SVAC_TYPE(*nal_ptr) == NAL_IDR && decodeAble();
   }

   bool configFrame() const override {
       auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
       switch (SVAC_TYPE(*nal_ptr)) {
           case NAL_SPS:
           case NAL_PPS: return true;
           default: return false;
       }
   }

   bool dropAble() const override {
       auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
       switch (SVAC_TYPE(*nal_ptr)) {
           case NAL_SEI:
           case NAL_SURVEI_UNIT: return true;
           default: return false;
       }
   }

   bool decodeAble() const override {
       auto nal_ptr = (uint8_t *) this->data() + this->prefixSize();
       auto type = SVAC_TYPE(*nal_ptr);
       //多slice情况下, first_mb_in_slice 表示其为一帧的开始
       return type >= NAL_NONE_IDR && type <= NAL_IDR && (nal_ptr[1] & 0x80);
   }
};

/**
* SVAC帧类
*/
using SVACFrame = SVACFrameHelper<FrameImp>;

/**
* 防止内存拷贝的SVAC类
* 用户可以通过该类型快速把一个指针无拷贝的包装成Frame类
*/
using SVACFrameNoCacheAble = SVACFrameHelper<FrameFromPtr>;

/**
* SVAC视频通道
*/
class SVACTrack : public VideoTrack {
public:
   using Ptr = std::shared_ptr<SVACTrack>;

   /**
    * 不指定sps pps构造SVAC类型的媒体
    * 在随后的inputFrame中获取sps pps
    */
   SVACTrack() = default;

   /**
    * 构造SVAC类型的媒体
    * @param sps sps帧数据
    * @param pps pps帧数据
    * @param sps_prefix_len SVAC头长度，可以为3个或4个字节，一般为0x00 00 00 01
    * @param pps_prefix_len SVAC头长度，可以为3个或4个字节，一般为0x00 00 00 01
    */
   SVACTrack(const std::string &sps, const std::string &pps, int sps_prefix_len = 4, int pps_prefix_len = 4);

   bool ready() const override;
   CodecId getCodecId() const override;
   int getVideoHeight() const override;
   int getVideoWidth() const override;
   float getVideoFps() const override;
   bool inputFrame(const Frame::Ptr &frame) override;
   bool update() override;

private:
   Sdp::Ptr getSdp(uint8_t payload_type) const override;
   Track::Ptr clone() const override;
   bool inputFrame_l(const Frame::Ptr &frame);
   void insertConfigFrame(const Frame::Ptr &frame);

private:
   bool _latest_is_config_frame = false;
   int _width = 0;
   int _height = 0;
   float _fps = 0;
   std::string _sps;
   std::string _pps;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_SVAC_H
