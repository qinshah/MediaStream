#ifndef MEDIA_STREAM_LOGGER_H
#define MEDIA_STREAM_LOGGER_H

// hilog 封装：统一 tag，便于日志过滤
#include <hilog/log.h>

namespace media_stream {

constexpr const char *kLogTag = "MediaStream";
constexpr unsigned int kLogDomain = 0xD003;

} // namespace media_stream

#define MS_LOGD(fmt, ...) OH_LOG_DEBUG(LOG_APP, fmt, ##__VA_ARGS__)
#define MS_LOGI(fmt, ...) OH_LOG_INFO(LOG_APP, fmt, ##__VA_ARGS__)
#define MS_LOGW(fmt, ...) OH_LOG_WARN(LOG_APP, fmt, ##__VA_ARGS__)
#define MS_LOGE(fmt, ...) OH_LOG_ERROR(LOG_APP, fmt, ##__VA_ARGS__)

// hilog 回调宏需要 fmt 为字面量；使用固定 tag/domain 的便捷宏
#define MS_LOG_DEBUG(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_DEBUG, media_stream::kLogDomain, media_stream::kLogTag, fmt, ##__VA_ARGS__)
#define MS_LOG_INFO(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_INFO, media_stream::kLogDomain, media_stream::kLogTag, fmt, ##__VA_ARGS__)
#define MS_LOG_WARN(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_WARN, media_stream::kLogDomain, media_stream::kLogTag, fmt, ##__VA_ARGS__)
#define MS_LOG_ERROR(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_ERROR, media_stream::kLogDomain, media_stream::kLogTag, fmt, ##__VA_ARGS__)

#endif // MEDIA_STREAM_LOGGER_H
