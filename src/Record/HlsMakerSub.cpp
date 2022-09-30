/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

 #include "Util/File.h"
#include "HlsMakerSub.h"
#if defined(_WIN32)
#include <io.h>
#define _access access
#else
#include <unistd.h>
#include <dirent.h>
#endif // WIN32


using namespace std;
using namespace toolkit;

namespace mediakit {

HlsMakerSub::HlsMakerSub(float seg_duration, uint32_t seg_number, bool seg_keep) {
    //最小允许设置为0，0个切片代表点播
    _seg_number = seg_number;
    _seg_duration = seg_duration;
    _seg_keep = seg_keep;
    _is_record = false;
}

HlsMakerSub::~HlsMakerSub() {

    _is_close_stream = true;
}

void HlsMakerSub::startRecord(bool isRecord) {
    if (_is_record) { //检测到上一次是在录像，清空_segment_file_paths
        _segment_file_paths.clear();
    }

    if(isRecord) {
        _seg_keep = true;
    }else{
        _seg_keep = false;
        _is_close_stream = true;   
    }
    _is_record = isRecord;
}


void HlsMakerSub::makeIndexFile(bool eof) {
    char file_content[1024];
    int maxSegmentDuration = 0;

    for (auto &tp : _seg_dur_list) {
        int dur = std::get<0>(tp);
        if (dur > maxSegmentDuration) {
            maxSegmentDuration = dur;
        }
    }

    auto sequence = _seg_number ? (_file_index > _seg_number ? _file_index - _seg_number : 0LL) : 0LL;

    string m3u8;
     if (_seg_number == 0) {
        // 录像点播支持时移
        snprintf(file_content, sizeof(file_content),
                 "#EXTM3U\n"
                 "#EXT-X-PLAYLIST-TYPE:EVENT\n"
                 "#EXT-X-VERSION:4\n"
                 "#EXT-X-TARGETDURATION:%u\n"
                 "#EXT-X-MEDIA-SEQUENCE:%llu\n",
                 (maxSegmentDuration + 999) / 1000,
                 sequence);
    } else {
        snprintf(file_content, sizeof(file_content),
                 "#EXTM3U\n"
                 "#EXT-X-VERSION:3\n"
                 "#EXT-X-ALLOW-CACHE:NO\n"
                 "#EXT-X-TARGETDURATION:%u\n"
                 "#EXT-X-MEDIA-SEQUENCE:%llu\n",
                 (maxSegmentDuration + 999) / 1000,
                 sequence);
    }
    
    m3u8.assign(file_content);

    for (auto &tp : _seg_dur_list) {
        snprintf(file_content, sizeof(file_content), "#EXTINF:%.3f,\n%s\n", std::get<0>(tp) / 1000.0, std::get<1>(tp).data());
        m3u8.append(file_content);
    }

    if (eof) {
        snprintf(file_content, sizeof(file_content), "#EXT-X-ENDLIST\n");
        m3u8.append(file_content);
    }
    onWriteHls(m3u8);
}


void HlsMakerSub::inputData(void *data, size_t len, uint64_t timestamp, bool is_idr_fast_packet) {
    if (data && len) {
        if (timestamp < _last_timestamp) {
            //时间戳回退了，切片时长重新计时
            WarnL << "stamp reduce: " << _last_timestamp << " -> " << timestamp;
            _last_seg_timestamp = _last_timestamp = timestamp;
        }
        if (is_idr_fast_packet) {
            //尝试切片ts
            addNewSegment(timestamp);
        }
        if (!_last_file_name.empty()) {
            //存在切片才写入ts数据
            onWriteSegment((char *) data, len);
            _last_timestamp = timestamp;
        }
    } else {
        //resetTracks时触发此逻辑
        flushLastSegment(false);
    }
}

void HlsMakerSub::delOldSegment() {
    if (_seg_number == 0) {
        //如果设置为保留0个切片，则认为是保存为点播
        return;
    }
    //在hls m3u8索引文件中,我们保存的切片个数跟_seg_number相关设置一致
    if (_file_index > _seg_number) {
        _seg_dur_list.pop_front();
    }
    //如果设置为一直保存，就不删除
    if (_seg_keep) {
        return;
    }
    GET_CONFIG(uint32_t, segRetain, Hls::kSegmentRetain);
    //但是实际保存的切片个数比m3u8所述多若干个,这样做的目的是防止播放器在切片删除前能下载完毕
    if (_file_index > _seg_number + segRetain) {
        onDelSegment(_file_index - _seg_number - segRetain - 1);
    }
}

void HlsMakerSub::addNewSegment(uint64_t stamp) {
    if (!_last_file_name.empty() && stamp - _last_seg_timestamp < _seg_duration * 1000) {
        //存在上个切片，并且未到分片时间
        return;
    }

    //关闭并保存上一个切片，如果_seg_number==0,那么是点播。
    flushLastSegment(false);

    //新增切片
    _last_file_name = onOpenSegment(_file_index++);
    //记录本次切片的起始时间戳
    _last_seg_timestamp = _last_timestamp ? _last_timestamp : stamp;

}

void HlsMakerSub::flushLastSegment(bool eof) {
    if (_last_file_name.empty()) {
        //不存在上个切片
        return;
    }
    //文件创建到最后一次数据写入的时间即为切片长度
    auto seg_dur = _last_timestamp - _last_seg_timestamp;
    if (seg_dur <= 0) {
        seg_dur = 100;
    }
    _seg_dur_list.emplace_back(seg_dur, std::move(_last_file_name));
    delOldSegment();
    //先flush ts切片，否则可能存在ts文件未写入完毕就被访问的情况
    onFlushLastSegment(seg_dur);
    //然后写m3u8文件
    makeIndexFile(eof);
    //判断当前是否在录像，正在录像的话，生成录像的m3u8文件
    if (_is_record) {
        createM3u8FileForRecord();
    }
    
}

bool HlsMakerSub::isLive() {
    return _seg_number != 0;
}

bool HlsMakerSub::isKeep() {
    return _seg_keep;
}

void HlsMakerSub::clear() {
    _file_index = 0;
    _last_timestamp = 0;
    _last_seg_timestamp = 0;
    _seg_dur_list.clear();
    _last_file_name.clear();

}

std::string HlsMakerSub::getM3u8TSBody(const std::string &file_content) {
    
    string new_file = file_content;
    if (file_content.find("#EXT-X-ENDLIST") != file_content.npos) {
        //找到了，则去掉"#EXT-X-ENDLIST"
        new_file = file_content.substr(0, file_content.length() - 15);
    }

    string body = new_file.substr(new_file.find_last_of("#"));
    //此时的body为
    //#EXTINF:4.534,
    //2022-09-14/08/2022-09-14_08-35-16.ts
    string extinf = body.substr(0, body.find(",")+2);
    string tsFile = body.substr(body.find_last_of("/") + 1);
    body.append("#EXT-X-ENDLIST\n");

    return extinf + tsFile + "#EXT-X-ENDLIST\n";
}

std::string HlsMakerSub::getTsFile(const std::string &file_content) {
    //  最后一个TS的body为
    //  2022-09-13/13/58-13_43.ts
    //  #EXT-X-ENDLIST
    string body = file_content.substr(file_content.find_last_of(",") + 2);
    string ts_file_name = body.substr(body.find_last_of("/") + 1);
    if (ts_file_name.find("#EXT-X-ENDLIST") == ts_file_name.npos ) {
        ts_file_name = ts_file_name.substr(0, ts_file_name.length() - 4); //没找到，去掉“.ts\n”，只留名字
    } else {
        ts_file_name = ts_file_name.substr(0, ts_file_name.length() - 19); //找到的话，去掉“.ts\n#EXT-X-ENDLIST\n”，只留名字
    }
    
    return ts_file_name;
}

void HlsMakerSub::createM3u8FileForRecord() {
    // 1.读取直播目录下的m3u8文件，获取当前的ts文件以及时长，并生成m3u8文件的路径
    string live_file = File::loadFile((getPathPrefix() + "/hls.m3u8").data());
    if (live_file.empty()) {
        return;
    }

    string body = getM3u8TSBody(live_file);
    string ts_file_name = getTsFile(live_file); // ts_file: 2022-09-14_11-06-03
    string m3u8_file = getPathPrefix() + "/" + ts_file_name.substr(0, 10) + "/" + ts_file_name.substr(11, 2) + "/";

    // 2.判断该目录下有没有m3u8文件，没有的话，生成第一个m3u8文件，有的话，重命名
    int handle = -1;
    DIR *dir_info = opendir(m3u8_file.data());
    struct dirent *dir_entry;
    if (dir_info) {
        while ((dir_entry =readdir(dir_info)) != NULL) {
            if (end_with(dir_entry->d_name, ".m3u8")) {
                handle = 0;
                break;
            }
        }
        closedir(dir_info);
    } else {
        return;
    }

   if (-1 == handle) {//第一次播放流
        _m3u8_file_path = m3u8_file + ts_file_name + ".m3u8";
        _is_close_stream = false;
    } else {//断流过，一次以上播放
       if (_is_close_stream) {
           _m3u8_file_path = m3u8_file + ts_file_name + ".m3u8";
           _is_close_stream = false;
       }
       if (_m3u8_file_path.length() == 0) { //服务重启后，进来，_m3u8_file_path为空
           _m3u8_file_path = m3u8_file + ts_file_name + ".m3u8";
       }
   }

    if (_m3u8_file_path.empty()) {
        WarnL << "create m3u8 file failed, _m3u8_file_path is empty."  ;
        return;
    }

    //3.写m3u8文件
    string m3u8Header = "#EXTM3U\n"
                        "#EXT-X-PLAYLIST-TYPE:EVENT\n"
                        "#EXT-X-VERSION:4\n"
                        "#EXT-X-TARGETDURATION:2\n"
                        "#EXT-X-MEDIA-SEQUENCE:0\n";

    if (access(_m3u8_file_path.data(), 0) != 0) { //文件不存在
        auto file = File::create_file(_m3u8_file_path.data(), "wb");
        if (file) {
            fwrite(m3u8Header.data(), m3u8Header.size(), 1, file);
            fwrite(body.data(), body.size(), 1, file);
            fclose(file);
        }
    } else {
        // 第二次进来，去掉 "#EXT-X-ENDLIST\n"，再重新追加file_content，保存文件
        auto file = File::create_file(_m3u8_file_path.data(), "r+");
        if (file) {
            fseek(file, -15, SEEK_END);
            fwrite(body.data(), body.size(), 1, file);
            fclose(file);
        } 
    }
}

}//namespace mediakit
