/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <ctime>
#include <sys/stat.h>
#include "HlsMakerImpSub.h"
#include "Util/util.h"
#include "Util/uv_errno.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

HlsMakerImpSub::HlsMakerImpSub(
    const string &m3u8_file,
                         const string &params,
                         uint32_t bufSize,
                         float seg_duration,
                         uint32_t seg_number,
                         bool seg_keep):HlsMakerSub(seg_duration, seg_number, seg_keep) {
    _path_prefix = m3u8_file.substr(0, m3u8_file.rfind('/'));
    _path_hls = m3u8_file;
    _params = params;
    _buf_size = bufSize;
    _file_buf.reset(new char[bufSize], [](char *ptr) {
        delete[] ptr;
    });

    _info.folder = _path_prefix;
}

HlsMakerImpSub::~HlsMakerImpSub() {
    clearCache(false, true);
}

void HlsMakerImpSub::clearCache() {
    clearCache(true, false);
}

void HlsMakerImpSub::clearCache(bool immediately, bool eof) {
    //录制完了
    flushLastSegment(eof);
    if (!isLive()||isKeep()) {
        return;
    }

    clear();
    _file = nullptr;
    //删除_segment_file_paths路径对应的直播文件
    for (auto it : _segment_file_paths) {
        auto ts_path = it.second;
        File::delete_file(ts_path.data());
    }
    _segment_file_paths.clear();

    //程序异常退出的情况下，直播的8个ts文件还是无法删除，
    //这里先删除掉m3u8文件对应的3个ts文件。还有保留的5个ts文件无法删除，能删除几个是几个吧。
    fstream file(_path_prefix + "/hls.m3u8");
    string data;
    while (getline(file,data)) {
        string ts_path = _path_prefix + "/" + data;
        File::delete_file(ts_path.data());
    }

    //删除缓存的m3u8文件
    File::delete_file((_path_prefix + "/hls.m3u8").data());
}

string HlsMakerImpSub::onOpenSegment(uint64_t index) {
    string segment_name, segment_path;
    
    auto strDate = getTimeStr("%Y-%m-%d");
    auto strHour = getTimeStr("%H");
    auto strTime = getTimeStr("%M-%S");
    segment_name = StrPrinter << strDate + "_" + strHour + "-" + strTime << ".ts";
    segment_path = _path_prefix + "/" + strDate + "/" + strHour + "/" + segment_name;
    if ((!isKeep())) {
        _segment_file_paths.emplace(index, segment_path);
    }
    
    _file = makeFile(segment_path, true);

    //保存本切片的元数据
    _info.start_time = ::time(NULL);
    _info.file_name = segment_name;
    _info.file_path = segment_path;
    _info.url = _info.app + "/" + _info.stream + "/" + segment_name;

    if (!_file) {
        WarnL << "create file failed," << segment_path << " " << get_uv_errmsg();
        return "";
    }
    if (_params.empty()) {
        return strDate + "/" + strHour + "/" + segment_name;
    }
    return strDate + "/" + strHour + "/" + segment_name + "?" + _params;
}

void HlsMakerImpSub::onDelSegment(uint64_t index) {
    auto it = _segment_file_paths.find(index);
    if (it == _segment_file_paths.end()) {
        return;
    }
    File::delete_file(it->second.data());
    _segment_file_paths.erase(it);
}

void HlsMakerImpSub::onWriteSegment(const char *data, size_t len) {
    if (_file) {
        fwrite(data, len, 1, _file.get());
    }
    if (_media_src) {
        _media_src->onSegmentSize(len);
    }
}

void HlsMakerImpSub::onWriteHls(const std::string &data) {
    auto hls = makeFile(_path_hls);
    if (hls) {
        fwrite(data.data(), data.size(), 1, hls.get());
        hls.reset();
        if (_media_src) {
            _media_src->setIndexFile(data);
        }
    } else {
        WarnL << "create hls file failed," << _path_hls << " " << get_uv_errmsg();
    }
    //DebugL << "\r\n"  << string(data,len);
}

void HlsMakerImpSub::onFlushLastSegment(uint64_t duration_ms) {
    //关闭并flush文件到磁盘
    _file = nullptr;

    GET_CONFIG(bool, broadcastRecordTs, Hls::kBroadcastRecordTs);
    if (broadcastRecordTs) {
        _info.time_len = duration_ms / 1000.0f;
        _info.file_size = File::fileSize(_info.file_path.data());
        NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastRecordTs, _info);
    }
}

std::shared_ptr<FILE> HlsMakerImpSub::makeFile(const string &file, bool setbuf) {
    auto file_buf = _file_buf;
    auto ret = shared_ptr<FILE>(File::create_file(file.data(), "wb"), [file_buf](FILE *fp) {
        if (fp) {
            fclose(fp);
        }
    });
    if (ret && setbuf) {
        setvbuf(ret.get(), _file_buf.get(), _IOFBF, _buf_size);
    }
    return ret;
}

void HlsMakerImpSub::setMediaSource(const string &vhost, const string &app, const string &stream_id) {
    _media_src = std::make_shared<HlsMediaSource>(vhost, app, stream_id);
    _info.app = app;
    _info.stream = stream_id;
    _info.vhost = vhost;
}

HlsMediaSource::Ptr HlsMakerImpSub::getMediaSource() const {
    return _media_src;
}

std::string HlsMakerImpSub::getPathPrefix() {
    return _path_prefix;
}

}//namespace mediakit