//
// EngineLog.mm — see EngineLog.h.
//
#import <Foundation/Foundation.h>
#include "EngineLog.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>

os_log_t wvEngineLog(void)
{
    static os_log_t l = os_log_create("com.writea.viz", "engine");
    return l;
}

void wvLog(const char* fmt, ...)
{
    static FILE* f = nullptr;
    if (!f) {
        NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/MScopes"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString* path = [dir stringByAppendingPathComponent:@"engine.log"];
        NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
        f = fopen(path.fileSystemRepresentation, ([attrs fileSize] > 2 * 1024 * 1024) ? "w" : "a");
        if (!f) return;
        setvbuf(f, nullptr, _IOLBF, 0);
    }
    char ts[32];
    time_t t = time(nullptr);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
}
