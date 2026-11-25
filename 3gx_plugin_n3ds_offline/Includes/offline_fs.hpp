#ifndef OFFLINE_FS_HPP
#define OFFLINE_FS_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>

#ifdef __3DS__
#include <CTRPluginFramework.hpp>
#else
#include <cstdio>
#endif

class OfflineFile {
public:
    OfflineFile() = default;
    ~OfflineFile() { Close(); }

    OfflineFile(const OfflineFile &) = delete;
    OfflineFile &operator=(const OfflineFile &) = delete;

    bool OpenRead(const char *path) {
        Close();
#ifdef __3DS__
        if (CTRPluginFramework::File::Open(file_, path, CTRPluginFramework::File::READ) == CTRPluginFramework::File::SUCCESS) {
            open_ = true;
            return true;
        }
        return false;
#else
        fp_ = std::fopen(path, "rb");
        return fp_ != nullptr;
#endif
    }

    bool OpenAppend(const char *path) {
        Close();
#ifdef __3DS__
        int mode = CTRPluginFramework::File::WRITE | CTRPluginFramework::File::CREATE |
                   CTRPluginFramework::File::APPEND | CTRPluginFramework::File::SYNC;
        if (CTRPluginFramework::File::Open(file_, path, mode) == CTRPluginFramework::File::SUCCESS) {
            open_ = true;
            return true;
        }
        return false;
#else
        fp_ = std::fopen(path, "ab");
        return fp_ != nullptr;
#endif
    }

    void Close() {
#ifdef __3DS__
        if (open_) {
            file_.Close();
            open_ = false;
        }
#else
        if (fp_) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
#endif
    }

    bool IsOpen() const {
#ifdef __3DS__
        return open_;
#else
        return fp_ != nullptr;
#endif
    }

    uint64_t GetSize() const {
#ifdef __3DS__
        if (!open_) return 0;
        s64 sz = file_.GetSize();
        return (sz > 0) ? (uint64_t)sz : 0;
#else
        if (!fp_) return 0;
        long cur = std::ftell(fp_);
        std::fseek(fp_, 0, SEEK_END);
        long end = std::ftell(fp_);
        std::fseek(fp_, cur, SEEK_SET);
        return (end > 0) ? (uint64_t)end : 0;
#endif
    }

    bool SeekSet(uint64_t off) {
#ifdef __3DS__
        if (!open_) return false;
        return file_.Seek((s64)off, CTRPluginFramework::File::SET) == CTRPluginFramework::File::SUCCESS;
#else
        if (!fp_) return false;
        return std::fseek(fp_, (long)off, SEEK_SET) == 0;
#endif
    }

    bool ReadExact(void *buf, size_t len) {
#ifdef __3DS__
        if (!open_) return false;
        uint8_t *dst = (uint8_t *)buf;
        size_t rem = len;
        while (rem > 0) {
            u32 chunk = (rem > 262144) ? 262144u : (u32)rem;
            if (file_.Read(dst, chunk) != CTRPluginFramework::File::SUCCESS) return false;
            dst += chunk;
            rem -= chunk;
        }
        return true;
#else
        if (!fp_) return false;
        return std::fread(buf, 1, len, fp_) == len;
#endif
    }

    bool WriteExact(const void *buf, size_t len) {
#ifdef __3DS__
        if (!open_) return false;
        return file_.Write(buf, (u32)len) == CTRPluginFramework::File::SUCCESS;
#else
        if (!fp_) return false;
        return std::fwrite(buf, 1, len, fp_) == len;
#endif
    }

private:
#ifdef __3DS__
    CTRPluginFramework::File file_;
    bool open_{false};
#else
    std::FILE *fp_{nullptr};
#endif
};

#endif // OFFLINE_FS_HPP
