#include <iostream>
#include "StreamHandle.h"

int main()
{
    StreamInfo infoStream;
    StreamHandle stream;


    stream.ListSupportedHD();
    //infoStream.bSavePic = true;
    infoStream.bSaveVideo = true;
    //infoStream.bSaveVideo = false;
    infoStream.bRtmp = true;
    //infoStream.bRtmp = false;
    //std::string strRtsp = "rtsp://admin:Admin123@192.168.2.64:554";
    infoStream.strInput = "video.mp4";
    //std::string strRtsp = "rtsp://localhost:8554/test";
    //infoStream.strOutput = "test.mp4";
    infoStream.strOutput = "rtmp://127.0.0.1:1935/live/test";
    //infoStream.nHDType = AV_HWDEVICE_TYPE_NONE;
    //infoStream.nHDType = AV_HWDEVICE_TYPE_CUDA;
    infoStream.nHDType = AV_HWDEVICE_TYPE_DXVA2;

    stream.StartDecode(infoStream);


    std::this_thread::sleep_for(std::chrono::milliseconds(200000));
    stream.StopDecode();
    getchar();
    return 0;
}