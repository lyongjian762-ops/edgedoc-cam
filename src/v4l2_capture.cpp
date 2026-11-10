#include <linux/videodev2.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

[[noreturn]] void fail_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

int xioctl(int fd, unsigned long request, void* arg) {
    int result;
    do { result = ioctl(fd, request, arg); } while (result < 0 && errno == EINTR);
    return result;
}

std::string fourcc(uint32_t f) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>((f >> (8 * i)) & 0xff);
        s += c >= 32 && c <= 126 ? static_cast<char>(c) : '?';
    }
    return s;
}

std::string json_string(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

const char* timestamp_clock(uint32_t flags) {
    switch (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) {
    case V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC: return "monotonic";
    case V4L2_BUF_FLAG_TIMESTAMP_COPY: return "copy";
    default: return "unknown";
    }
}

const char* timestamp_source(uint32_t flags) {
    switch (flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) {
    case V4L2_BUF_FLAG_TSTAMP_SRC_EOF: return "end_of_frame";
    case V4L2_BUF_FLAG_TSTAMP_SRC_SOE: return "start_of_exposure";
    default: return "unknown";
    }
}

std::optional<int64_t> timestamp_microseconds(const timeval& t) {
    if (t.tv_sec < 0 || t.tv_usec < 0 || t.tv_usec >= 1000000 ||
        static_cast<uint64_t>(t.tv_sec) > static_cast<uint64_t>((INT64_MAX - t.tv_usec) / 1000000))
        return std::nullopt;
    return static_cast<int64_t>(t.tv_sec) * 1000000 + t.tv_usec;
}

std::optional<int64_t> frame_interval_us(std::optional<int64_t> current, uint32_t flags,
                                         std::optional<int64_t> previous, uint32_t previous_flags) {
    const uint32_t mask = V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
    if (!current || !previous || (flags & mask) != (previous_flags & mask) ||
        (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC || *current < *previous)
        return std::nullopt;
    return *current - *previous;
}

bool dequeue_buffer(int fd, v4l2_buffer& b, int (*call)(int, unsigned long, void*) = xioctl) {
    if (call(fd, VIDIOC_DQBUF, &b) == 0) return true;
    if (errno == EAGAIN) return false;
    fail_errno("VIDIOC_DQBUF");
}

bool frame_payload_valid(uint32_t flags, const std::vector<uint32_t>& used,
                         const std::vector<uint32_t>& offsets, const std::vector<size_t>& lengths) {
    if ((flags & V4L2_BUF_FLAG_ERROR) || used.empty() || used.size() != offsets.size() || used.size() != lengths.size())
        return false;
    for (size_t i = 0; i < used.size(); ++i)
        if (offsets[i] >= used[i] || used[i] > lengths[i]) return false;
    return true;
}

// POLLPRI reports V4L2 events, not a captured video buffer. We do not subscribe
// to events here. Fatal flags take precedence even if POLLIN is also set.
bool frame_ready(short revents) {
    if (revents & (POLLERR | POLLNVAL | POLLHUP)) throw std::runtime_error("poll device error");
    if (revents & POLLIN) return true;
    if (revents & POLLPRI) throw std::runtime_error("unexpected priority event; not a video frame");
    return false;
}

void wait_for_frame(int fd, std::chrono::steady_clock::time_point end) {
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= end) throw std::runtime_error("poll timeout");
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count() + 1;
        pollfd pfd{fd, POLLIN, 0};
        int r = poll(&pfd, 1, static_cast<int>(remaining > INT_MAX ? INT_MAX : remaining));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) fail_errno("poll");
        if (r == 0) throw std::runtime_error("poll timeout");
        if (frame_ready(pfd.revents)) return;
    }
}

class TemporaryOutput {
public:
    explicit TemporaryOutput(const std::string& target) {
        std::string pattern = target + ".tmp.XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        int fd = mkstemp(name.data());
        if (fd < 0) fail_errno("create temporary output");
        try { path_ = name.data(); } catch (...) { close(fd); unlink(name.data()); throw; }
        fd_ = fd;
        if (fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) {
            int e = errno; close(fd_); fd_ = -1;
            if (unlink(path_.c_str()) < 0 && errno != ENOENT)
                std::cerr << "cleanup: temporary file remains: " << path_ << ": " << std::strerror(errno) << '\n';
            path_.clear(); errno = e;
            fail_errno("set temporary output close-on-exec");
        }
    }
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;
    ~TemporaryOutput() {
        if (fd_ >= 0 && close(fd_) < 0) std::cerr << "cleanup: temporary close: " << std::strerror(errno) << '\n';
        if (!path_.empty() && unlink(path_.c_str()) < 0 && errno != ENOENT)
            std::cerr << "cleanup: temporary file remains: " << path_ << ": " << std::strerror(errno) << '\n';
    }
    void write_and_close(const void* data, size_t length) {
        const auto* p = static_cast<const uint8_t*>(data);
        while (length) {
            ssize_t n = write(fd_, p, length);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { if (n == 0) errno = EIO; fail_errno("write temporary output"); }
            p += n; length -= static_cast<size_t>(n);
        }
        int rc = close(fd_); fd_ = -1;
        if (rc < 0) fail_errno("close temporary output");
    }
    void publish(const std::string& target) {
        if (fd_ >= 0) throw std::runtime_error("temporary output must be closed before publication");
        if (syscall(SYS_renameat2, AT_FDCWD, path_.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) < 0)
            fail_errno("publish " + target);
        path_.clear();
    }
private:
    int fd_ = -1;
    std::string path_;
};

unsigned parse_number(const char* text, const char* name, unsigned max) {
    if (!text || !*text || *text == '-') throw std::runtime_error(std::string("invalid ") + name);
    char* end = nullptr;
    errno = 0;
    unsigned long n = std::strtoul(text, &end, 10);
    if (errno || *end || n == 0 || n > max) throw std::runtime_error(std::string("invalid ") + name);
    return static_cast<unsigned>(n);
}

struct Options {
    std::string device, output;
    std::string api = "auto";
    unsigned count = 0, save_index = 0, timeout_ms = 5000;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: edgedoc-v4l2-capture --device PATH --count N --save-index N --output PREFIX [--timeout-ms N] [--api auto|single|multi]\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
        const char* value = argv[++i];
        if (arg == "--device") o.device = value;
        else if (arg == "--output") o.output = value;
        else if (arg == "--count") o.count = parse_number(value, "count", 1000000);
        else if (arg == "--save-index") o.save_index = parse_number(value, "save-index", 1000000);
        else if (arg == "--timeout-ms") o.timeout_ms = parse_number(value, "timeout-ms", INT_MAX);
        else if (arg == "--api") o.api = value;
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (o.device.empty() || o.output.empty() || !o.count || !o.save_index || o.save_index > o.count)
        throw std::runtime_error("require --device, --count, --save-index (<= count), and --output");
    if (o.api != "auto" && o.api != "single" && o.api != "multi") throw std::runtime_error("invalid --api");
    return o;
}

struct Mapping { void* ptr = MAP_FAILED; size_t length = 0; };
struct Buffer { std::vector<Mapping> planes; };
struct PlaneFormat { uint32_t bytesperline, sizeimage; };
struct Format {
    uint32_t width = 0, height = 0, pixel_format = 0, field = 0;
    uint32_t colorspace = 0, ycbcr_enc = 0, quantization = 0, xfer_func = 0;
    std::vector<PlaneFormat> planes;
};
struct Snapshot {
    uint32_t sequence = 0, flags = 0;
    int64_t sec = 0, usec = 0;
    std::vector<uint32_t> bytesused, data_offset;
    std::vector<std::vector<uint8_t>> planes;
};

class Capture {
public:
    explicit Capture(const Options& options) : o_(options) {
        fd_ = open(o_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) fail_errno("open device");
    }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture() {
        if (streaming_ && xioctl(fd_, VIDIOC_STREAMOFF, &type_) < 0)
            std::cerr << "cleanup: STREAMOFF: " << std::strerror(errno) << '\n';
        for (auto& buffer : buffers_) for (auto& plane : buffer.planes)
            if (plane.ptr != MAP_FAILED && munmap(plane.ptr, plane.length) < 0)
                std::cerr << "cleanup: munmap: " << std::strerror(errno) << '\n';
        if (fd_ >= 0 && close(fd_) < 0) std::cerr << "cleanup: close: " << std::strerror(errno) << '\n';
    }
    void run() {
        inspect();
        allocate();
        for (unsigned i = 0; i < buffers_.size(); ++i) queue(i);
        if (xioctl(fd_, VIDIOC_STREAMON, &type_) < 0) fail_errno("VIDIOC_STREAMON");
        streaming_ = true;
        unsigned valid = 0, rejected = 0;
        Snapshot snapshot;
        std::optional<int64_t> previous_timestamp;
        uint32_t previous_timestamp_flags = 0;
        while (valid < o_.count) {
            v4l2_buffer b{};
            std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o_.timeout_ms);
            for (;;) {
                wait_for_frame(fd_, deadline);
                b = {}; planes = {};
                prepare(b, planes);
                if (dequeue_buffer(fd_, b)) break;
            }
            if (b.index >= buffers_.size()) throw std::runtime_error("DQBUF returned invalid index");
            const unsigned n = multi_ ? b.length : 1;
            if (n != format_.planes.size() || n != buffers_[b.index].planes.size())
                throw std::runtime_error("DQBUF plane count mismatch");
            std::vector<uint32_t> used, offsets;
            std::vector<size_t> lengths;
            for (unsigned p = 0; p < n; ++p) {
                const uint32_t bytes = multi_ ? planes[p].bytesused : b.bytesused;
                const uint32_t offset = multi_ ? planes[p].data_offset : 0;
                used.push_back(bytes); offsets.push_back(offset);
                lengths.push_back(buffers_[b.index].planes[p].length);
            }
            if (!frame_payload_valid(b.flags, used, offsets, lengths)) {
                std::cerr << "rejected index=" << b.index << " sequence=" << b.sequence
                          << " flags=0x" << std::hex << b.flags << std::dec << " invalid_payload_or_error_flag\n";
                queue(b.index);
                if (++rejected > 100) throw std::runtime_error("too many invalid frames");
                continue;
            }
            if (valid + 1 == o_.save_index) {
                snapshot.sequence = b.sequence; snapshot.flags = b.flags;
                snapshot.sec = b.timestamp.tv_sec; snapshot.usec = b.timestamp.tv_usec;
                snapshot.bytesused = used; snapshot.data_offset = offsets;
                for (unsigned p = 0; p < n; ++p) {
                    auto* begin = static_cast<const uint8_t*>(buffers_[b.index].planes[p].ptr);
                    snapshot.planes.emplace_back(begin + offsets[p], begin + used[p]);
                }
            }
            // The buffer returns to the driver before the iteration is counted successful.
            queue(b.index);
            ++valid;
            auto current_timestamp = timestamp_microseconds(b.timestamp);
            const uint32_t timestamp_flags = b.flags & (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
            std::cout << "frame=" << valid << " index=" << b.index << " sequence=" << b.sequence
                      << " timestamp=" << b.timestamp.tv_sec << '.' << std::setw(6) << std::setfill('0')
                      << b.timestamp.tv_usec << std::setfill(' ') << " flags=0x" << std::hex << b.flags << std::dec
                      << " timestamp_clock=" << timestamp_clock(b.flags) << " timestamp_source=" << timestamp_source(b.flags);
            auto interval = frame_interval_us(current_timestamp, timestamp_flags, previous_timestamp, previous_timestamp_flags);
            if (interval)
                std::cout << " interval_us=" << *interval;
            else std::cout << " interval_us=unavailable";
            previous_timestamp = current_timestamp;
            previous_timestamp_flags = timestamp_flags;
            for (unsigned p = 0; p < n; ++p) std::cout << " plane" << p << "_bytesused=" << used[p] << " plane" << p << "_offset=" << offsets[p];
            std::cout << '\n';
        }
        if (xioctl(fd_, VIDIOC_STREAMOFF, &type_) < 0) fail_errno("VIDIOC_STREAMOFF");
        streaming_ = false;
        save(snapshot);
        std::cout << "completed valid=" << valid << " rejected=" << rejected << " streamoff=ok\n";
    }
private:
    void inspect() {
        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) fail_errno("VIDIOC_QUERYCAP");
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_STREAMING)) throw std::runtime_error("device does not support streaming");
        bool has_single = caps & V4L2_CAP_VIDEO_CAPTURE;
        bool has_multi = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
        if (o_.api == "multi" || (o_.api == "auto" && has_multi)) multi_ = true;
        else if (o_.api == "single" || o_.api == "auto") multi_ = false;
        if ((multi_ && !has_multi) || (!multi_ && !has_single)) throw std::runtime_error("requested capture API unsupported");
        type_ = multi_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4l2_format f{}; f.type = type_;
        if (xioctl(fd_, VIDIOC_G_FMT, &f) < 0) fail_errno("VIDIOC_G_FMT");
        if (multi_) {
            const auto& p = f.fmt.pix_mp;
            if (!p.num_planes || p.num_planes > VIDEO_MAX_PLANES) throw std::runtime_error("invalid format plane count");
            format_.width=p.width; format_.height=p.height; format_.pixel_format=p.pixelformat; format_.field=p.field;
            format_.colorspace=p.colorspace; format_.ycbcr_enc=p.ycbcr_enc; format_.quantization=p.quantization; format_.xfer_func=p.xfer_func;
            for (unsigned i=0; i<p.num_planes; ++i) format_.planes.push_back({p.plane_fmt[i].bytesperline,p.plane_fmt[i].sizeimage});
        } else {
            const auto& p = f.fmt.pix;
            format_.width=p.width; format_.height=p.height; format_.pixel_format=p.pixelformat; format_.field=p.field;
            format_.colorspace=p.colorspace; format_.ycbcr_enc=p.ycbcr_enc; format_.quantization=p.quantization; format_.xfer_func=p.xfer_func;
            format_.planes.push_back({p.bytesperline,p.sizeimage});
        }
        std::cout << "driver=" << reinterpret_cast<const char*>(cap.driver) << " card=" << reinterpret_cast<const char*>(cap.card)
                  << " capabilities=0x" << std::hex << caps << std::dec << " api=" << (multi_ ? "multi" : "single")
                  << " width=" << format_.width << " height=" << format_.height << " fourcc=" << fourcc(format_.pixel_format)
                  << " fourcc_u32=" << format_.pixel_format << " planes=" << format_.planes.size()
                  << " field=" << format_.field << " colorspace=" << format_.colorspace << " ycbcr_enc=" << format_.ycbcr_enc
                  << " quantization=" << format_.quantization << " xfer_func=" << format_.xfer_func << '\n';
        for (size_t i=0; i<format_.planes.size(); ++i)
            std::cout << "format_plane=" << i << " bytesperline=" << format_.planes[i].bytesperline << " sizeimage=" << format_.planes[i].sizeimage << '\n';
    }
    void prepare(v4l2_buffer& b, std::array<v4l2_plane, VIDEO_MAX_PLANES>& planes) {
        b.type = type_; b.memory = V4L2_MEMORY_MMAP;
        if (multi_) { b.m.planes = planes.data(); b.length = static_cast<uint32_t>(format_.planes.size()); }
    }
    void allocate() {
        v4l2_requestbuffers req{}; req.count=4; req.type=type_; req.memory=V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) fail_errno("VIDIOC_REQBUFS");
        if (req.count < 2 || req.count > 1024) throw std::runtime_error("unexpected MMAP buffer count");
        buffers_.resize(req.count);
        for (unsigned i=0; i<req.count; ++i) {
            v4l2_buffer b{}; std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
            prepare(b, planes); b.index=i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) fail_errno("VIDIOC_QUERYBUF");
            unsigned n = multi_ ? b.length : 1;
            if (n != format_.planes.size()) throw std::runtime_error("QUERYBUF plane count mismatch");
            buffers_[i].planes.resize(n);
            for (unsigned p=0; p<n; ++p) {
                size_t len = multi_ ? planes[p].length : b.length;
                off_t offset = multi_ ? planes[p].m.mem_offset : b.m.offset;
                if (!len) throw std::runtime_error("zero-length MMAP plane");
                void* addr = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
                if (addr == MAP_FAILED) fail_errno("mmap");
                buffers_[i].planes[p] = {addr, len};
            }
        }
        std::cout << "mapped_buffers=" << buffers_.size() << '\n';
    }
    void queue(unsigned index) {
        v4l2_buffer b{}; std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
        prepare(b, planes); b.index=index;
        if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) fail_errno("VIDIOC_QBUF");
    }
    void save(const Snapshot& s) {
        if (s.planes.empty()) throw std::runtime_error("snapshot was not copied");
        std::vector<std::unique_ptr<TemporaryOutput>> files;
        std::vector<std::string> targets;
        std::ostringstream json;
        json << "{\n  \"api\": \"" << (multi_ ? "multi" : "single") << "\",\n"
             << "  \"width\": " << format_.width << ", \"height\": " << format_.height << ",\n"
             << "  \"fourcc\": \"" << json_string(fourcc(format_.pixel_format)) << "\", \"fourcc_u32\": " << format_.pixel_format << ",\n"
             << "  \"field\": " << format_.field << ", \"colorspace\": " << format_.colorspace << ", \"ycbcr_enc\": " << format_.ycbcr_enc
             << ", \"quantization\": " << format_.quantization << ", \"xfer_func\": " << format_.xfer_func << ",\n"
             << "  \"sequence\": " << s.sequence << ", \"timestamp_sec\": " << s.sec << ", \"timestamp_usec\": " << s.usec
             << ", \"flags\": " << s.flags << ", \"timestamp_clock\": \"" << timestamp_clock(s.flags)
             << "\", \"timestamp_source\": \"" << timestamp_source(s.flags) << "\",\n  \"planes\": [\n";
        for (size_t i=0; i<s.planes.size(); ++i) {
            targets.push_back(o_.output + ".plane" + std::to_string(i) + ".bin");
            files.push_back(std::make_unique<TemporaryOutput>(targets.back()));
            files.back()->write_and_close(s.planes[i].data(), s.planes[i].size());
            json << "    {\"index\": " << i << ", \"bytesperline\": " << format_.planes[i].bytesperline
                 << ", \"sizeimage\": " << format_.planes[i].sizeimage << ", \"bytesused\": " << s.bytesused[i]
                 << ", \"data_offset\": " << s.data_offset[i] << ", \"saved_bytes\": " << s.planes[i].size() << "}"
                 << (i+1<s.planes.size() ? "," : "") << '\n';
        }
        json << "  ]\n}\n";
        const auto text=json.str();
        targets.push_back(o_.output + ".json");
        files.push_back(std::make_unique<TemporaryOutput>(targets.back()));
        files.back()->write_and_close(text.data(), text.size());
        // Publish metadata last: an absent JSON indicates an incomplete set.
        for (size_t i = 0; i < files.size(); ++i) files[i]->publish(targets[i]);
    }
    Options o_;
    int fd_=-1;
    bool multi_=false, streaming_=false;
    v4l2_buf_type type_{};
    Format format_;
    std::vector<Buffer> buffers_;
};
} // namespace

#ifndef EDGEDOC_V4L2_NO_MAIN
int main(int argc, char** argv) {
    try { auto options=parse_args(argc, argv); Capture capture(options); capture.run(); return 0; }
    catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
#endif
