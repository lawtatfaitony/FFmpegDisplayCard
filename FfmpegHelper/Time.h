#pragma once
#include <string>
#include <chrono>

class Time
{
public:
static std::string GetCurrentDate()
{
    time_t ltime = time(NULL);
    std::tm ltm = { 0 };
    localtime_s(&ltm, &ltime);
    char buffer[128] = { 0 };
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &ltm);
    return buffer;
}

static std::string GetCurrentSystemTime()
{
    time_t ltime = time(NULL);
    std::tm ltm = { 0 };
    localtime_s(&ltm, &ltime);
    char buffer[128] = { 0 };
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &ltm);
    return buffer;
}

static int64_t GetMilliTimestamp()
{
    auto curr = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        curr.time_since_epoch());
    return ms.count();
}
};

