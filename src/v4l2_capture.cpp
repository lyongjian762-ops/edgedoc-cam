#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
        while (valid < o_.count) {
            v4l2_buffer b{};
            std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o_.timeout_ms);
            for (;;) {
                wait_ready(deadline);
                b = {}; planes = {};
                prepare(b, planes);
                if (xioctl(fd_, VIDIOC_DQBUF, &b) == 0) break;
                if (errno != EAGAIN) fail_errno("VIDIOC_DQBUF");
            }
            if (b.index >= buffers_.size()) throw std::runtime_error("DQBUF returned invalid index");
            const unsigned n = multi_ ? b.length : 1;
            if (n != format_.planes.size() || n != buffers_[b.index].planes.size())
                throw std::runtime_error("DQBUF plane count mismatch");
            std::vector<uint32_t> used, offsets;
            bool valid_sizes = true;
            for (unsigned p = 0; p < n; ++p) {
                const uint32_t bytes = multi_ ? planes[p].bytesused : b.bytesused;
                const uint32_t offset = multi_ ? planes[p].data_offset : 0;
                used.push_back(bytes); offsets.push_back(offset);
                if (offset > bytes || bytes > buffers_[b.index].planes[p].length || bytes == offset) valid_sizes = false;
            }
            const bool error = (b.flags & V4L2_BUF_FLAG_ERROR) != 0 || !valid_sizes;
            if (error) {
                std::cerr << "rejected index=" << b.index << " sequence=" << b.sequence
                          << " flags=0x" << std::hex << b.flags << std::dec
                          << " valid_sizes=" << valid_sizes << '\n';
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
            std::cout << "frame=" << valid << " index=" << b.index << " sequence=" << b.sequence
                      << " timestamp=" << b.timestamp.tv_sec << '.' << std::setw(6) << std::setfill('0')
                      << b.timestamp.tv_usec << std::setfill(' ') << " flags=0x" << std::hex << b.flags << std::dec;
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
    void wait_ready(std::chrono::steady_clock::time_point end) {
        for (;;) {
            auto now=std::chrono::steady_clock::now();
            if (now >= end) throw std::runtime_error("poll timeout");
            auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(end-now).count()+1;
            pollfd pfd{fd_, POLLIN | POLLPRI, 0};
            int r=poll(&pfd, 1, static_cast<int>(remaining > INT_MAX ? INT_MAX : remaining));
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) fail_errno("poll");
            if (r == 0) throw std::runtime_error("poll timeout");
            if (pfd.revents & (POLLNVAL | POLLHUP)) throw std::runtime_error("poll device unavailable");
            if (pfd.revents & (POLLIN | POLLPRI)) return;
            if (pfd.revents & POLLERR) throw std::runtime_error("poll error");
        }
    }
    static void write_new(const std::string& path, const void* data, size_t length) {
        int out=open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (out < 0) fail_errno("create " + path);
        const auto* p=static_cast<const uint8_t*>(data);
        while (length) {
            ssize_t n=write(out,p,length);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { int e=n < 0 ? errno : EIO; close(out); errno=e; fail_errno("write " + path); }
            p+=n; length-=static_cast<size_t>(n);
        }
        if (close(out) < 0) fail_errno("close " + path);
    }
    void save(const Snapshot& s) {
        if (s.planes.empty()) throw std::runtime_error("snapshot was not copied");
        std::ostringstream json;
        json << "{\n  \"api\": \"" << (multi_ ? "multi" : "single") << "\",\n"
             << "  \"width\": " << format_.width << ", \"height\": " << format_.height << ",\n"
             << "  \"fourcc\": \"" << json_string(fourcc(format_.pixel_format)) << "\", \"fourcc_u32\": " << format_.pixel_format << ",\n"
             << "  \"field\": " << format_.field << ", \"colorspace\": " << format_.colorspace << ", \"ycbcr_enc\": " << format_.ycbcr_enc
             << ", \"quantization\": " << format_.quantization << ", \"xfer_func\": " << format_.xfer_func << ",\n"
             << "  \"sequence\": " << s.sequence << ", \"timestamp_sec\": " << s.sec << ", \"timestamp_usec\": " << s.usec
             << ", \"flags\": " << s.flags << ",\n  \"planes\": [\n";
        for (size_t i=0; i<s.planes.size(); ++i) {
            write_new(o_.output + ".plane" + std::to_string(i) + ".bin", s.planes[i].data(), s.planes[i].size());
            json << "    {\"index\": " << i << ", \"bytesperline\": " << format_.planes[i].bytesperline
                 << ", \"sizeimage\": " << format_.planes[i].sizeimage << ", \"bytesused\": " << s.bytesused[i]
                 << ", \"data_offset\": " << s.data_offset[i] << ", \"saved_bytes\": " << s.planes[i].size() << "}"
                 << (i+1<s.planes.size() ? "," : "") << '\n';
        }
        json << "  ]\n}\n";
        const auto text=json.str();
        write_new(o_.output + ".json", text.data(), text.size());
    }
    Options o_;
    int fd_=-1;
    bool multi_=false, streaming_=false;
    v4l2_buf_type type_{};
    Format format_;
    std::vector<Buffer> buffers_;
};
} // namespace

int main(int argc, char** argv) {
    try { auto options=parse_args(argc, argv); Capture capture(options); capture.run(); return 0; }
    catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
