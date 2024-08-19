/*
* Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
*
* This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
*
* Use of this source code is governed by MIT-like license that can be found in the
* LICENSE file in the root of the source tree. All contributing project authors
* may be found in the AUTHORS file in the root of the source tree.
*/

#include "SVAC.h"
#include "SPSParser.h"
#include "Util/logger.h"
#include "Util/base64.h"
#include "Common/Parser.h"
#include "Common/config.h"
#include "Extension/Factory.h"


using namespace std;
using namespace toolkit;

namespace mediakit {

static bool getAVCInfo(const char *sps, size_t sps_len, int &iVideoWidth, int &iVideoHeight, float &iVideoFps) {
   if (sps_len < 4) {
       return false;
   }
   T_GetBitContext tGetBitBuf;
   T_SVACSPS tSVACSpsInfo;
   memset(&tGetBitBuf, 0, sizeof(tGetBitBuf));
   memset(&tSVACSpsInfo, 0, sizeof(tSVACSpsInfo));
   tGetBitBuf.pu8Buf = (uint8_t *)sps + 1;
   tGetBitBuf.iBufSize = (int)(sps_len - 1);
   if (0 != svacDecSeqParameterSet((void *)&tGetBitBuf, &tSVACSpsInfo)) {
       return false;
   }
   iVideoWidth = tSVACSpsInfo.frame_width_minus_1 + 1;
   iVideoHeight = tSVACSpsInfo.frame_height_minus_1 + 1;
   if (tSVACSpsInfo.frame_rate == 0) {
       iVideoFps = 25;
   } else if (tSVACSpsInfo.frame_rate == 1) {
       iVideoFps = 30;
   } else {
       iVideoFps = tSVACSpsInfo.frame_rate;
   }
   // ErrorL << iVideoWidth << " " << iVideoHeight << " " << iVideoFps;
   return true;
}

static bool getAVCInfo(const string &strSps, int &iVideoWidth, int &iVideoHeight, float &iVideoFps) {
   return getAVCInfo(strSps.data(), strSps.size(), iVideoWidth, iVideoHeight, iVideoFps);
}

static const char *memfind(const char *buf, ssize_t len, const char *subbuf, ssize_t sublen) {
   for (auto i = 0; i < len - sublen; ++i) {
       if (memcmp(buf + i, subbuf, sublen) == 0) {
           return buf + i;
       }
   }
   return NULL;
}

void splitSVAC(
   const char *ptr, size_t len, size_t prefix, const std::function<void(const char *, size_t, size_t)> &cb) {
   auto start = ptr + prefix;
   auto end = ptr + len;
   size_t next_prefix;
   while (true) {
       auto next_start = memfind(start, end - start, "\x00\x00\x01", 3);
       if (next_start) {
           //找到下一帧
           if (*(next_start - 1) == 0x00) {
               //这个是00 00 00 01开头
               next_start -= 1;
               next_prefix = 4;
           } else {
               //这个是00 00 01开头
               next_prefix = 3;
           }
           //记得加上本帧prefix长度
           cb(start - prefix, next_start - start + prefix, prefix);
           //搜索下一帧末尾的起始位置
           start = next_start + next_prefix;
           //记录下一帧的prefi  x长度
           prefix = next_prefix;
           continue;
       }
       //未找到下一帧,这是最后一帧
       cb(start - prefix, end - start + prefix, prefix);
       break;
   }
}

static size_t prefixSize(const char *ptr, size_t len) {
   if (len < 4) {
       return 0;
   }

   if (ptr[0] != 0x00 || ptr[1] != 0x00) {
       //不是0x00 00开头
       return 0;
   }

   if (ptr[2] == 0x00 && ptr[3] == 0x01) {
       //是0x00 00 00 01
       return 4;
   }

   if (ptr[2] == 0x01) {
       //是0x00 00 01
       return 3;
   }
   return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

SVACTrack::SVACTrack(const string &sps, const string &pps, int sps_prefix_len, int pps_prefix_len) {
   _sps = sps.substr(sps_prefix_len);
   _pps = pps.substr(pps_prefix_len);
   update();
}

CodecId SVACTrack::getCodecId() const {
   return CodecSVAC;
}

int SVACTrack::getVideoHeight() const {
   return _height;
}

int SVACTrack::getVideoWidth() const {
   return _width;
}

float SVACTrack::getVideoFps() const {
   return _fps;
}

bool SVACTrack::ready() const {
   return !_sps.empty() && !_pps.empty();
}

static int j = 0;
bool SVACTrack::inputFrame(const Frame::Ptr &frame) {
   using SVACFrameInternal = FrameInternal<SVACFrameNoCacheAble>;
   int type = SVAC_TYPE(frame->data()[frame->prefixSize()]);

   //cyf test  这边输入的是脱去PS头后的帧 config帧和数据帧在一起
//   std::ofstream outfile;
//   outfile.open((std::string("D://svac//read//frame.") + std::to_string(j++)).c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
//   outfile.write((char *)frame->data(), frame->size());
//   outfile.close();
   if ((type == SVACFrame::NAL_NONE_IDR || type == SVACFrame::NAL_IDR) && ready()) {
       return inputFrame_l(frame);
   }

   //非I/B/P帧情况下，split一下，防止多个帧粘合在一起
//   bool ret = false;
//   splitSVAC(frame->data(), frame->size(), frame->prefixSize(), [&](const char *ptr, size_t len, size_t prefix) {
//       SVACFrameInternal::Ptr sub_frame = std::make_shared<SVACFrameInternal>(frame, (char *)ptr, len, prefix);
//       if (inputFrame_l(sub_frame)) {
//           ret = true;
//       }
//   });
//   return ret;
   return inputFrame_l(frame);
}





bool SVACTrack::update() {
       return getAVCInfo(_sps, _width, _height, _fps);
}

Track::Ptr SVACTrack::clone() const {
   return std::make_shared<SVACTrack>(*this);
}

bool SVACTrack::inputFrame_l(const Frame::Ptr &frame) {
   char* buf =  frame->data();
   int type = SVAC_TYPE(frame->data()[frame->prefixSize()]);
   bool ret = true;
   switch (type) {
       case SVACFrame::NAL_SPS: {
           // cyf 新博盒子过来的流不对劲，p帧前面都带了PPS,所以这里走不到defult逻辑，有sps就判断为I帧
           _sps = string(frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize());
           _latest_is_config_frame = true;
           const_cast<Frame::Ptr &>(frame) = std::make_shared<FrameCacheAble>(frame, true);
           ret = VideoTrack::inputFrame(frame);
           break;
       }
       case SVACFrame::NAL_PPS: {
           _pps = string(frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize());
           _latest_is_config_frame = false;
           ret = VideoTrack::inputFrame(frame);
           break;
       }
       default:
           // 避免识别不出关键帧
           if (_latest_is_config_frame && !frame->dropAble()) {
               if (!frame->keyFrame()) {
                   const_cast<Frame::Ptr &>(frame) = std::make_shared<FrameCacheAble>(frame, true);
               }
           }
           // 判断是否是I帧, 并且如果是,那判断前面是否插入过config帧, 如果插入过就不插入了
           if (frame->keyFrame() && !_latest_is_config_frame) {
               insertConfigFrame(frame);
           }
           if(!frame->dropAble()){
               _latest_is_config_frame = false;
           }
           ret = VideoTrack::inputFrame(frame);
           break;
   }

   if (_width == 0 && ready()) {
       update();
   }
   return ret;
}

void SVACTrack::insertConfigFrame(const Frame::Ptr &frame) {
   if (!_sps.empty()) {
       auto spsFrame = FrameImp::create<SVACFrame>();
       spsFrame->_prefix_size = 4;
       spsFrame->_buffer.assign("\x00\x00\x00\x01", 4);
       spsFrame->_buffer.append(_sps);
       spsFrame->_dts = frame->dts();
       spsFrame->setIndex(frame->getIndex());
       VideoTrack::inputFrame(spsFrame);
   }

   if (!_pps.empty()) {
       auto ppsFrame = FrameImp::create<SVACFrame>();
       ppsFrame->_prefix_size = 4;
       ppsFrame->_buffer.assign("\x00\x00\x00\x01", 4);
       ppsFrame->_buffer.append(_pps);
       ppsFrame->_dts = frame->dts();
       ppsFrame->setIndex(frame->getIndex());
       VideoTrack::inputFrame(ppsFrame);
   }
}

class SVACSdp : public Sdp {
public:
   SVACSdp(const string &strSPS, const string &strPPS, int payload_type, int bitrate) : Sdp(90000, payload_type) {
       _printer << "m=video 0 RTP/AVP " << payload_type << "\r\n";
       if (bitrate) {
           _printer << "b=AS:" << bitrate << "\r\n";
       }
       _printer << "a=rtpmap:" << payload_type << " " << getCodecName(CodecSVAC) << "/" << 90000 << "\r\n";

       /**
        Single NAI Unit Mode = 0. // Single NAI mode (Only nals from 1-23 are allowed)
        Non Interleaved Mode = 1，// Non-interleaved Mode: 1-23，24 (STAP-A)，28 (FU-A) are allowed
        Interleaved Mode = 2,  // 25 (STAP-B)，26 (MTAP16)，27 (MTAP24)，28 (EU-A)，and 29 (EU-B) are allowed.
        **/
       GET_CONFIG(bool, h264_stap_a, Rtp::kH264StapA);
       _printer << "a=fmtp:" << payload_type << " packetization-mode=" << h264_stap_a << "; profile-level-id=";

       uint32_t profile_level_id = 0;
       if (strSPS.length() >= 4) { // sanity check
           profile_level_id = (uint8_t(strSPS[1]) << 16) |
               (uint8_t(strSPS[2]) << 8) |
               (uint8_t(strSPS[3])); // profile_idc|constraint_setN_flag|level_idc
       }

       char profile[8];
       snprintf(profile, sizeof(profile), "%06X", profile_level_id);
       _printer << profile;
       _printer << "; sprop-parameter-sets=";
       _printer << encodeBase64(strSPS) << ",";
       _printer << encodeBase64(strPPS) << "\r\n";
   }

   string getSdp() const { return _printer; }

private:
   _StrPrinter _printer;
};

Sdp::Ptr SVACTrack::getSdp(uint8_t payload_type) const {
   if (!ready()) {
       WarnL << getCodecName() << " Track未准备好";
       return nullptr;
   }
   return std::make_shared<SVACSdp>(_sps, _pps, payload_type, getBitRate() / 1024);
}

namespace {

CodecId getCodec() {
   return CodecSVAC;
}

Track::Ptr getTrackByCodecId(int sample_rate, int channels, int sample_bit) {
   return std::make_shared<SVACTrack>();
}

Track::Ptr getTrackBySdp(const SdpTrack::Ptr &track) {
   //a=fmtp:96 packetization-mode=1;profile-level-id=42C01F;sprop-parameter-sets=Z0LAH9oBQBboQAAAAwBAAAAPI8YMqA==,aM48gA==
   auto map = Parser::parseArgs(track->_fmtp, ";", "=");
   auto sps_pps = map["sprop-parameter-sets"];
   string base64_SPS = findSubString(sps_pps.data(), NULL, ",");
   string base64_PPS = findSubString(sps_pps.data(), ",", NULL);
   auto sps = decodeBase64(base64_SPS);
   auto pps = decodeBase64(base64_PPS);
   if (sps.empty() || pps.empty()) {
       //如果sdp里面没有sps/pps,那么可能在后续的rtp里面恢复出sps/pps
       return std::make_shared<SVACTrack>();
   }
   return std::make_shared<SVACTrack>(sps, pps, 0, 0);
}

RtpCodec::Ptr getRtpEncoderByCodecId(uint8_t pt) {
   //return std::make_shared<SVACRtpEncoder>();
    return NULL;
}

RtpCodec::Ptr getRtpDecoderByCodecId() {
   //return std::make_shared<SVACRtpDecoder>();
   return NULL;
}

RtmpCodec::Ptr getRtmpEncoderByTrack(const Track::Ptr &track) {
   //return std::make_shared<SVACRtmpEncoder>(track);
   return NULL;
}

RtmpCodec::Ptr getRtmpDecoderByTrack(const Track::Ptr &track) {
   //return std::make_shared<SVACRtmpDecoder>(track);
   return NULL;
}

Frame::Ptr getFrameFromPtr(const char *data, size_t bytes, uint64_t dts, uint64_t pts) {
   return std::make_shared<SVACFrameNoCacheAble>((char *)data, bytes, dts, pts, prefixSize(data, bytes));
}

} // namespace

CodecPlugin svac_plugin = { getCodec,
                           getTrackByCodecId,
                           getTrackBySdp,
                           getRtpEncoderByCodecId,
                           getRtpDecoderByCodecId,
                           getRtmpEncoderByTrack,
                           getRtmpDecoderByTrack,
                           getFrameFromPtr };

} // namespace mediakit
