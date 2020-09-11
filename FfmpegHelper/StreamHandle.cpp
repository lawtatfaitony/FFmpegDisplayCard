#include "StreamHandle.h"
#include <direct.h>
#include <io.h>
#include <iostream>
#include "Time.h"


static std::string kVidoeType = ".mp4";
static std::string kVideoDir = "video";
static std::string kPictureDir = "picture";

// avformat_open_input/av_read_frame timeout callback
// return: 0(continue original call), other(interrupt original call)
int StreamHandle::read_interrupt_cb(void* pContext)
{
    return 0;
}

enum AVPixelFormat StreamHandle::get_hw_format(AVCodecContext *ctx,
    const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *pPixeFmt;
    for (pPixeFmt = pix_fmts; *pPixeFmt != -1; pPixeFmt++)
    {
        if (*pPixeFmt == ctx->pix_fmt)
            return *pPixeFmt;
    }
    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

int StreamHandle::hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int nCode = 0;
    if ((nCode = av_hwdevice_ctx_create(&m_pHDCtx, type,
        NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return nCode;
    }
    ctx->hw_device_ctx = av_buffer_ref(m_pHDCtx);
    return nCode;
}

StreamHandle::StreamHandle()
    : m_bExit(false)
    , m_pInputAVFormatCtx(nullptr)
    , m_pOutputFileAVFormatCtx(nullptr)
    , m_pOutputStreamAVFormatCtx(nullptr)
    , m_bInputInited(false)
    , m_bOutputInited(false)
    , m_bFirstRun(true)
    , m_pVideoDecoderCtx(nullptr)
    , m_pAudioDecoderCtx(nullptr)
    , m_pHDCtx(nullptr)
{
    create_directory();
    m_poolSavePic.Start();
}

StreamHandle::~StreamHandle()
{
    StopDecode();
}

void StreamHandle::ListSupportedHD()
{
    {
        std::stringstream ss;
        enum AVHWDeviceType nType = AV_HWDEVICE_TYPE_NONE;
        while ((nType = av_hwdevice_iterate_types(nType)) != AV_HWDEVICE_TYPE_NONE)
            ss << av_hwdevice_get_type_name(nType) << ",";
        std::cout << "Suuported hard device: " << ss.str() << std::endl;
    }
}

bool StreamHandle::StartDecode(const StreamInfo& infoStream)
{
    if (infoStream.strInput.empty())
    {
        printf("Invalid stream input\n");
        return false;
    }  
    if (!(infoStream.bRtmp || infoStream.bSavePic || infoStream.bSaveVideo))
    {
        printf("Nothing tod do, save picture, save video of push rtmp\n");
        return false;
    }
    m_infoStream = infoStream;
    if (!open_input_stream()) {
        printf("Can't open input:%s\n", m_infoStream.strInput.c_str());
        return false;
    }
    // save with picture and video or rtmp
    if (m_infoStream.bRtmp) {
        open_output_stream(m_pOutputStreamAVFormatCtx, m_infoStream.bRtmp);
    }
    if (m_infoStream.bSaveVideo) {
        open_output_stream(m_pOutputFileAVFormatCtx);
    }
    m_thHandleFrame = std::thread(std::bind(&StreamHandle::handle_frame, this));
    m_thDecode = std::thread(std::bind(&StreamHandle::do_decode, this));

    return true;
}

void StreamHandle::StopDecode()
{
    m_bExit = true;
    m_cvFrame.notify_one();
    if (m_thDecode.joinable())
        m_thDecode.join();
    if (m_thHandleFrame.joinable())
        m_thHandleFrame.join();
    close_input_stream();
    close_output_stream();
    if (m_pHDCtx != nullptr) {
        av_buffer_unref(&m_pHDCtx);
        m_pHDCtx = nullptr;
    }
    m_poolSavePic.Stop();
    free_frame_convert_info();
}

void StreamHandle::PushFrame(const cv::Mat& frame)
{
    /*{
        std::lock_guard<std::mutex> lock(m_mtFrame);
        m_listFrame.push_back(frame);
    }*/
    //m_poolSavePic.Commit([=]()
    //{
    //    std::string strFilename = generate_filename();
    //    // it costs a little time，15/50
    //    cv::imwrite(strFilename.c_str(), frame);
    //});
}

bool StreamHandle::PopFrame(cv::Mat& frame)
{
    std::lock_guard<std::mutex> lock(m_mtFrame);
    if (m_listFrame.empty()) return false;
    frame = m_listFrame.front();
    m_listFrame.pop_front();
    return true;
}

bool StreamHandle::open_input_stream()
{
    if (m_pInputAVFormatCtx)
    {
        std::string strError = "avformat already exists";
        return false;
    }
    AVDictionary *pDict = NULL;
    m_pInputAVFormatCtx = avformat_alloc_context();
    av_dict_set(&pDict, "rtsp_transport", "tcp", 0);                //采用tcp传输
    av_dict_set(&pDict, "stimeout", "2000000", 0);
    m_pInputAVFormatCtx->flags |= AVFMT_FLAG_NONBLOCK;
    // open input file, and allocate format context
    int nCode = avformat_open_input(&m_pInputAVFormatCtx, m_infoStream.strInput.c_str(), 0, &pDict);
    if (nCode < 0)
    {
        std::string strError = "Can't open input:" + m_infoStream.strInput
            + get_error_msg(nCode);
        return false;
    }
    m_pInputAVFormatCtx->interrupt_callback = { read_interrupt_cb, this };
    // retrieve stream information
    if (avformat_find_stream_info(m_pInputAVFormatCtx, 0) < 0)
    {
        std::string strError = "Can't find stream info";
        return false;
    }
    //手工调试函数，看到pFormatCtx->streams的内容
    av_dump_format(m_pInputAVFormatCtx, 0, m_infoStream.strInput.c_str(), 0);
    // open codec contex for video
    if (open_codec_context(m_infoStream.nVideoIndex, &m_pVideoDecoderCtx, m_pInputAVFormatCtx, AVMEDIA_TYPE_VIDEO)) {
        m_infoStream.nWidth = m_pVideoDecoderCtx->width;
        m_infoStream.nHeight = m_pVideoDecoderCtx->height;
        m_infoStream.nPixFmt = m_pVideoDecoderCtx->pix_fmt;
    }
    else
    {
        printf("Open codec context failed\n");
        return false;
    }
    // open codec contex for audio
    open_codec_context(m_infoStream.nAudioIndex, &m_pAudioDecoderCtx, m_pInputAVFormatCtx, AVMEDIA_TYPE_AUDIO);
    if (kInvalidStreamIndex== m_infoStream.nVideoIndex
        && kInvalidStreamIndex == m_infoStream.nAudioIndex)
    {
        std::string strError = "Can't find audio or video stream in the input";
        return false;
    }
    return true;
}

bool StreamHandle::open_codec_context(int& nStreamIndex,
    AVCodecContext **pDecoderCtx,
    AVFormatContext *pFmtCtx,
    enum AVMediaType nMediaType)
{
    AVStream *pStream = nullptr;
    AVCodec *pDecoder = nullptr;
    AVDictionary *pOptions = nullptr;

    int nCode = av_find_best_stream(pFmtCtx, nMediaType, -1, -1, &pDecoder, 0);
    if (nCode < 0)
    {
        fprintf(stderr, "Could not find %s stream in input\n",
            av_get_media_type_string(nMediaType));
        return false;
    }
    nStreamIndex = nCode;
    AVPixelFormat nPixeFmt = AV_PIX_FMT_NONE;
    if (AVMEDIA_TYPE_VIDEO == nMediaType && m_infoStream.nHDType > AV_HWDEVICE_TYPE_NONE) {
        // get hard device
        for (int i = 0;; i++) {
            const AVCodecHWConfig *pConfig = avcodec_get_hw_config(pDecoder, i);
            if (nullptr == pConfig) {
                continue;
            }
            if (!pConfig) {
                fprintf(stderr, "Decoder %s does not support device type %s.\n",
                pDecoder->name, av_hwdevice_get_type_name(m_infoStream.nHDType));
                return false;
            }
            if (pConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                pConfig->device_type == m_infoStream.nHDType) {
                nPixeFmt = pConfig->pix_fmt;
                break;
            }
        }
    }
    /* Allocate a codec context for the decoder */
    *pDecoderCtx = avcodec_alloc_context3(pDecoder);
    if (!*pDecoderCtx)
    {
        fprintf(stderr, "Failed to allocate the %s codec context\n",
            av_get_media_type_string(nMediaType));
        return false;
    }

    /* Copy codec parameters from input stream to output codec context */
    pStream = m_pInputAVFormatCtx->streams[nStreamIndex];
    if ((nCode = avcodec_parameters_to_context(*pDecoderCtx, pStream->codecpar)) < 0)
    {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
            av_get_media_type_string(nMediaType));
        return false;
    }
    (*pDecoderCtx)->get_format = get_hw_format;
    // init the hard device deecoder
    if (AVMEDIA_TYPE_VIDEO == nMediaType && nPixeFmt != AV_PIX_FMT_NONE) {
        (*pDecoderCtx)->pix_fmt = nPixeFmt;
        hw_decoder_init(*pDecoderCtx, m_infoStream.nHDType);
    }

    /* Init the decoders, with or without reference counting */
    av_dict_set(&pOptions, "refcounted_frames", m_infoStream.nRefCount ? "1" : "0", 0);
    if ((nCode = avcodec_open2(*pDecoderCtx, pDecoder, &pOptions)) < 0)
    {
        fprintf(stderr, "Failed to open %s codec\n",
            av_get_media_type_string(nMediaType));
        return false;
    }
    return true;
}

void StreamHandle::close_input_stream()
{
    if (m_pInputAVFormatCtx)
        avformat_close_input(&m_pInputAVFormatCtx);
}

bool StreamHandle::open_output_stream(AVFormatContext*& pFormatCtx, bool bRtmp)
{
    if (pFormatCtx)
    {
        printf("Already has output avformat \n");
        return false;
    }
    if (m_infoStream.strOutput.empty())
    {
        printf("Invalid output file\n");
        return false;
    }
    std::string strFormatName = bRtmp ? "flv" : "mp4";
    std::string strOutputPath = bRtmp ? m_infoStream.strOutput : generate_filename(kFileTypeVideo);
    int nCode = avformat_alloc_output_context2(&pFormatCtx, NULL, strFormatName.c_str(), strOutputPath.c_str());
    if (nullptr == pFormatCtx)
    {
        printf("Can't alloc output context \n");
        return false;
    }

    pFormatCtx->oformat->audio_codec = AV_CODEC_ID_AAC;     // video编码为AAC
    pFormatCtx->oformat->video_codec = AV_CODEC_ID_H264;
    for (auto nIndex = 0; nIndex < m_pInputAVFormatCtx->nb_streams; ++nIndex)
    {
        AVStream *pInStream = m_pInputAVFormatCtx->streams[nIndex];
        AVStream *pOutStream = avformat_new_stream(pFormatCtx, nullptr);
        if (!pOutStream)
        {
            printf("Can't new out stream");
            return false;
        }
        pOutStream->codecpar->codec_type = pInStream->codecpar->codec_type;
        //copy the encode info to output
        nCode = avcodec_parameters_copy(pOutStream->codecpar, pInStream->codecpar);
        if (nCode < 0)
        {
            std::string strError = "Can't copy context, url: " + m_infoStream.strInput + ",errcode:"
                + std::to_string(nCode) + ",err msg:" + get_error_msg(nCode);
            printf("%s \n", strError.c_str());
            return false;
        }
        pOutStream->codecpar->codec_tag = 0;
    }
    av_dump_format(pFormatCtx, 0, strOutputPath.c_str(), 1);
    if (!(pFormatCtx->oformat->flags & AVFMT_NOFILE))
    {
        nCode = avio_open(&pFormatCtx->pb, strOutputPath.c_str(), AVIO_FLAG_WRITE);
        if (nCode < 0)
        {
            std::string strError = "Can't open output io, file:" + strOutputPath + ",errcode:" + std::to_string(nCode) + ", err msg:"
                + get_error_msg(nCode);
            printf("%s \n", strError.c_str());
            return false;
        }
    }

    nCode = avformat_write_header(pFormatCtx, NULL);
    if (nCode < 0)
    {
        std::string strError = "Can't write outputstream header, URL:" + strOutputPath + ",errcode:" + std::to_string(nCode) + ", err msg:"
            + get_error_msg(nCode);
        printf("%s \n", strError.c_str());
        m_bOutputInited = false;
        return false;
    }
    m_bOutputInited = true;
    return true;
}

void StreamHandle::close_output_stream()
{
    bool bRtmp = m_infoStream.bRtmp;
    bool bSaveVideo = m_infoStream.bSaveVideo;
    release_output_format_context(m_infoStream.bSaveVideo, m_pOutputFileAVFormatCtx);
    release_output_format_context(m_infoStream.bRtmp, m_pOutputStreamAVFormatCtx);
    m_bOutputInited = false;
}

void StreamHandle::do_decode()
{
    // decode stream
    int64_t nFrame = 0;
    AVPacket packet;
    while (!m_bExit)
    {
        int nCode = 0;
        if (nCode = av_read_frame(m_pInputAVFormatCtx, &packet), nCode < 0)
        {
            printf("Read frame failed,%s\n", get_error_msg(nCode).c_str());
            break;
        }
        if (m_infoStream.nVideoIndex == packet.stream_index)
            nCode = decode_video_packet(&packet);
        else if (packet.stream_index == m_infoStream.nAudioIndex) {
            nCode = decode_audio_packet(packet);
        }
        save_stream(m_pOutputFileAVFormatCtx, packet);
        save_stream(m_pOutputStreamAVFormatCtx, packet);
        av_packet_unref(&packet);
    }

    printf("Reading ended, read %lld video frames \n", nFrame);
}

void StreamHandle::push_packet(const AVPacket& packet)
{
    {
        std::lock_guard<std::mutex> lock(m_mtPacket);
        m_listPacket.push_back(packet);
    }
    m_cvFrame.notify_one();
}

void StreamHandle::handle_frame()
{
    // handle frame of decode, save with video and picture
    int nFrame = 0;
    time_t tmStart = time(nullptr);
    bool bSucc = false;
    while (true)
    {
        AVPacket packet;
        {
            std::unique_lock<std::mutex> lock(m_mtPacket);
            m_cvFrame.wait(lock, [=]() {
                return (m_bExit || !m_listPacket.empty());
            });
            if (m_bExit && m_listPacket.empty()) {
                break;
            }
            packet = m_listPacket.front();
            m_listPacket.pop_front();
        }
        if (time(nullptr) - tmStart >= 1)   // check per second
            create_directory();
        if (packet.stream_index == m_infoStream.nVideoIndex) {
            bSucc = decode_video_packet(&packet);
        }
        else if (packet.stream_index == m_infoStream.nAudioIndex) {
            bSucc = decode_audio_packet(packet);
        }
        if (!bSucc) {
            continue;
        }
        // save stream with video, rtmp
        save_stream(m_pOutputFileAVFormatCtx, packet);
        save_stream(m_pOutputStreamAVFormatCtx, packet);
        ++nFrame;
    }
    printf("Save finished, %d frame\n", nFrame);
}

bool StreamHandle::decode_video_packet(AVPacket* packet)
{
    AVFrame *pFrame = nullptr, *pSwapFrame = nullptr;
    AVFrame *pTmpFrame = nullptr;
    int nCode = avcodec_send_packet(m_pVideoDecoderCtx, packet);
    if (nCode < 0) {
        fprintf(stderr, "Error during decoding,%s\n", get_error_msg(nCode).c_str());
        return false;
    }
    while (1) {
        if (!(pFrame = av_frame_alloc()) || !(pSwapFrame = av_frame_alloc()))
        {
            fprintf(stderr, "Can't alloc frame\n");
            nCode = AVERROR(ENOMEM);
            goto fail;
        }

        nCode = avcodec_receive_frame(m_pVideoDecoderCtx, pFrame);
        if (nCode == AVERROR(EAGAIN) || nCode == AVERROR_EOF) {
            av_frame_free(&pFrame);
            av_frame_free(&pSwapFrame);
            return true;
        }
        else if (nCode < 0)
        {
            fprintf(stderr, "Error while decoding,%s\n", get_error_msg(nCode).c_str());
            goto fail;
        }

        if (m_infoStream.nPixFmt > AV_HWDEVICE_TYPE_NONE
            && pFrame->format == m_infoStream.nPixFmt)
        {
            /* retrieve data from GPU to CPU */
            if ((nCode = av_hwframe_transfer_data(pSwapFrame, pFrame, 0)) < 0) {
                fprintf(stderr, "Error transferring the data to system memory\n");
                goto fail;
            }
            pTmpFrame = pSwapFrame;
        }
        else
            pTmpFrame = pFrame;
        m_infoFrameConvert.pCvMat = avframe_to_mat(pTmpFrame);
        PushFrame(m_infoFrameConvert.pCvMat);

    fail:
        av_frame_free(&pFrame);
        av_frame_free(&pSwapFrame);
        av_frame_unref(pTmpFrame);
        if (nCode < 0)
            return false;
    }
}

bool StreamHandle::decode_audio_packet(const AVPacket& packet)
{
    int data_size;

    /* send the packet with the compressed data to the decoder */
   int nCode = avcodec_send_packet(m_pAudioDecoderCtx, &packet);
    if (nCode < 0) {
        fprintf(stderr, "Error submitting the packet to the decoder\n");
        return false;
    }

    /* read all the output frames (in general there may be any number of them */
    while (nCode >= 0) {
        nCode = avcodec_receive_frame(m_pAudioDecoderCtx, m_infoFrameConvert.pFrame);
        if (AVERROR(EAGAIN) == nCode || AVERROR_EOF == nCode)
            return true;
        else if (nCode < 0) {
            fprintf(stderr, "Error during decoding\n");
            return false;
        }
        //data_size = av_get_bytes_per_sample(m_pAudioDecoderCtx->sample_fmt);
        //if (data_size < 0) {
        //    /* This should not occur, checking just for paranoia */
        //    fprintf(stderr, "Failed to calculate data size\n");
        //    return false;
        //}
        //for (int i = 0; i < m_pFrame->nb_samples; i++) {
        //    for (int nChl = 0; nChl < m_pAudioDecoderCtx->channels; nChl++) {
        //        //fwrite(m_pFrame->data[ch] + data_size*i, 1, data_size, outfile);
        //    }
        //}
    }
    return true;
}

void StreamHandle::save_stream(AVFormatContext* pFormatCtx, const AVPacket& packet)
{
    if (!m_bOutputInited || nullptr == packet.buf || 0 == packet.buf->size || nullptr == pFormatCtx) {
        return;
    }
    AVPacket pktFrame /*= packet*/;
    av_init_packet(&pktFrame);
    av_copy_packet(&pktFrame, &packet);
    AVStream *pInStream = m_pInputAVFormatCtx->streams[pktFrame.stream_index];
    AVStream *pOutStream = pFormatCtx->streams[pktFrame.stream_index];
    //转换PTS/DTS时序
    try {
        pktFrame.pts = av_rescale_q_rnd(pktFrame.pts, pInStream->time_base, pOutStream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pktFrame.dts = av_rescale_q_rnd(pktFrame.dts, pInStream->time_base, pOutStream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pktFrame.duration = av_rescale_q(pktFrame.duration, pInStream->time_base, pOutStream->time_base);
        pktFrame.pos = -1;
    }
    catch (const std::exception& e)
    {
        return;
    }
    catch (...)
    {
        printf("Unkonw error\n");
        return;
    }
    switch (pInStream->codecpar->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
    case AVMEDIA_TYPE_VIDEO:
    {
        int nError = av_interleaved_write_frame(pFormatCtx, &pktFrame);
        if (nError != 0)
        {
            printf("Error: %d while writing frame, %s\n", nError, get_error_msg(nError).c_str());
        }
        break;
    }
    default:
        break;
    }
}

void StreamHandle::free_frame_convert_info()
{
    if (m_infoFrameConvert.pFrame != nullptr) {
        av_frame_free(&m_infoFrameConvert.pFrame);
    }
}

void StreamHandle::release_output_format_context(bool& bInited, AVFormatContext*& pFmtContext)
{
    if (pFmtContext)
    {
        if (bInited)
            av_write_trailer(pFmtContext);
        if (!(pFmtContext->oformat->flags & AVFMT_NOFILE))
        {
            if (pFmtContext->pb)
            {
                avio_close(pFmtContext->pb);
            }
        }
        avformat_free_context(pFmtContext);
        pFmtContext = nullptr;
    }
    bInited = false;
}

void StreamHandle::create_directory()
{
    std::string strToday = Time::GetCurrentDate();
    if (strToday == m_strToday)return;
    if (0 == _mkdir(strToday.c_str()) || 0 == _access(strToday.c_str(), 0))
        m_strToday = strToday;
    std::string strVideoDir = m_strToday + "/" + kVideoDir;
    if (0 != _access(strVideoDir.c_str(), 0))
        _mkdir(strVideoDir.c_str());
    std::string strPictureDir = m_strToday + "/" + kPictureDir;
    if (0 != _access(strPictureDir.c_str(), 0))
        _mkdir(strPictureDir.c_str());
}

std::string StreamHandle::generate_filename(int nType)
{
    std::string strMillSecond = std::to_string(Time::GetMilliTimestamp());
    std::string strFilename;
    switch (nType)
    {
    case kFileTypeVideo: // 视频
        strFilename = m_strToday + "/" + kVideoDir + "/" + strMillSecond + ".mp4";
        break;
    case kFileTypeRtmp:
        strFilename = "";
        break;
    default:    // 照片
        strFilename = m_strToday + "/" + kPictureDir + "/" + strMillSecond + ".jpg";
        break;
    }

    return strFilename;
}

std::string StreamHandle::get_current_path()
{
    char szPath[260] = { 0 };
    _getcwd(szPath, 260);

    return szPath;
}

std::string StreamHandle::get_error_msg(int nErrorCode)
{
    char szMsg[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(szMsg, AV_ERROR_MAX_STRING_SIZE, nErrorCode);
    std::stringstream ss;
    {
        ss << "Code[" << nErrorCode << "]:" << szMsg;
    }
    return ss.str();
}

cv::Mat StreamHandle::avframe_to_mat(const AVFrame * frame)
{
    int width = frame->width;
    int height = frame->height;
    cv::Mat image(height, width, CV_8UC3);
    int cvLinesizes[1];
    cvLinesizes[0] = image.step1();
    SwsContext* pSwsCtx = sws_getContext(width, height, (AVPixelFormat)frame->format, width, height, AVPixelFormat::AV_PIX_FMT_BGR24, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    sws_scale(pSwsCtx, frame->data, frame->linesize, 0, height, &image.data, cvLinesizes);
    sws_freeContext(pSwsCtx);
    return image;
}
