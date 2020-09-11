#pragma once
#include <string>
#include <list>
#include "ThreadPool.h"
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avio.h>
#include <libavfilter/avfilter.h>
#include <libavutil/imgutils.h>
}

// rtsp info
struct StreamInfo
{
    int nRefCount = 0;
    std::string strInput;
    std::string strOutput;
    bool bSavePic = false;
    bool bSaveVideo = false;
    bool bRtmp = false;
    int nWidth = 0;
    int nHeight = 0;
    AVPixelFormat nPixFmt = AV_PIX_FMT_NONE;
    AVHWDeviceType nHDType = AV_HWDEVICE_TYPE_NONE;
    int nFrameRate = 25;
    int nVideoIndex = -1;
    int nAudioIndex = -1;
};
// frame convert
struct FrameConvertInfo
{
    AVFrame *pFrame = nullptr;
    cv::Mat pCvMat;
    FrameConvertInfo()
    {
        pFrame = av_frame_alloc();//分配内存
    }
};
enum FileNameType
{
    kFileTypePicture,       // picture
    kFileTypeVideo,         // video
    kFileTypeRtmp,          // rtmp
};
class StreamHandle
{
    const static int kInvalidStreamIndex = -1;

private:
    static int read_interrupt_cb(void* pContext);
    static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
    int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type);
    
public:
    StreamHandle();
    ~StreamHandle();
    void ListSupportedHD();
    bool StartDecode(const StreamInfo& infoStream);
    void StopDecode();

    void GetVideoSize(long & width, long & height)  //获取视频分辨率
    {
        width = m_infoStream.nWidth;
        height = m_infoStream.nHeight;
    }

    void PushFrame(const cv::Mat& frame);
    bool PopFrame(cv::Mat& frame);



private:
    // input
    bool open_input_stream();
    bool open_codec_context(int& nStreamIndex,
        AVCodecContext **dec_ctx,
        AVFormatContext *fmt_ctx,
        enum AVMediaType type);
    void close_input_stream();
    // output
    bool open_output_stream(AVFormatContext*& pFormatCtx, bool bRtmp = false);
    void close_output_stream();
    void do_decode();
    void push_packet(const AVPacket& packet);
    void handle_frame();
    bool decode_video_packet(AVPacket* packet);
    bool decode_audio_packet(const AVPacket& packet);
    void save_stream(AVFormatContext* pFormatCtx, const AVPacket& packet);
    void free_frame_convert_info();
    void release_output_format_context(bool& bInited, AVFormatContext*& pFmtContext);
    void create_directory();
    std::string generate_filename(int nType = kFileTypePicture);
    std::string get_current_path();
    std::string get_error_msg(int nErrorCode);
    cv::Mat avframe_to_mat(const AVFrame * frame);

private:
    bool m_bExit;
    bool m_bFirstRun;
    std::string m_strToday;
    StreamInfo m_infoStream;
    FrameConvertInfo m_infoFrameConvert;
    AVFormatContext* m_pInputAVFormatCtx;
    AVCodecContext* m_pVideoDecoderCtx;
    AVCodecContext* m_pAudioDecoderCtx;
    AVFormatContext* m_pOutputFileAVFormatCtx;
    AVFormatContext* m_pOutputStreamAVFormatCtx;
    AVBufferRef *m_pHDCtx;
    bool m_bInputInited;
    bool m_bOutputInited;

    std::thread m_thDecode;
    std::thread m_thHandleFrame;
    std::mutex m_mtPacket;
    std::condition_variable m_cvFrame;
    std::list<AVPacket> m_listPacket;

    // cache the frame
    std::mutex m_mtFrame;
    std::list<cv::Mat> m_listFrame;
    ThreadPool m_poolSavePic;




};

