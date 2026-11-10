#define EDGEDOC_V4L2_NO_MAIN
#include "../src/v4l2_capture.cpp"

#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>

namespace {
void check(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}
template<class F> void expect_error(F f, const char* what) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error(what);
}
int again(int, unsigned long, void*) { errno = EAGAIN; return -1; }
int broken(int, unsigned long, void*) { errno = EIO; return -1; }
int successful(int, unsigned long, void*) { return 0; }
volatile sig_atomic_t signal_seen = 0;
void alarm_handler(int) { signal_seen = 1; }

bool has_temporary(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) fail_errno("opendir test directory");
    bool found = false;
    while (dirent* entry = readdir(d)) if (std::strstr(entry->d_name, ".tmp.")) found = true;
    closedir(d);
    return found;
}
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void run_tests() {
    check(parse_number("300", "count", 1000000) == 300, "valid count");
    expect_error([] { parse_number("0", "count", 100); }, "zero count accepted");
    expect_error([] { parse_number("-1", "count", 100); }, "negative count accepted");
    expect_error([] { parse_number("1x", "count", 100); }, "invalid count accepted");
    char a0[]="program", a1[]="--device", a2[]="/dev/null", a3[]="--count", a4[]="1";
    char* missing[] = {a0, a1, a2, a3, a4};
    expect_error([&] { parse_args(5, missing); }, "missing options accepted");
    check(frame_ready(POLLIN), "POLLIN should indicate frame readiness");
    check(frame_ready(POLLIN | POLLPRI), "POLLIN with event should still indicate a frame");
    check(!frame_ready(0), "empty readiness must not indicate a frame");
    expect_error([] { frame_ready(POLLPRI); }, "POLLPRI alone treated as frame");
    expect_error([] { frame_ready(POLLERR); }, "POLLERR ignored");
    expect_error([] { frame_ready(POLLIN | POLLERR); }, "POLLERR with POLLIN ignored");
    expect_error([] { frame_ready(POLLHUP); }, "POLLHUP ignored");
    v4l2_buffer b{};
    check(!dequeue_buffer(-1, b, again), "EAGAIN should retry");
    check(dequeue_buffer(-1, b, successful), "successful DQBUF should proceed");
    expect_error([&] { dequeue_buffer(-1, b, broken); }, "EIO must fail");
    check(frame_payload_valid(0, {4}, {0}, {4}), "valid payload rejected");
    check(!frame_payload_valid(V4L2_BUF_FLAG_ERROR, {4}, {0}, {4}), "error flag accepted");
    check(!frame_payload_valid(0, {5}, {0}, {4}), "oversized payload accepted");
    check(!frame_payload_valid(0, {4}, {4}, {4}), "empty payload accepted");
    check(!frame_payload_valid(0, {4, 4}, {0}, {4, 4}), "plane count mismatch accepted");
    check(std::string(timestamp_clock(V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)) == "monotonic", "monotonic flag");
    check(std::string(timestamp_clock(V4L2_BUF_FLAG_TIMESTAMP_COPY)) == "copy", "copy flag");
    check(std::string(timestamp_source(V4L2_BUF_FLAG_TSTAMP_SRC_SOE)) == "start_of_exposure", "source flag");
    timeval t{10, 50};
    check(timestamp_microseconds(t) == 10000050, "timestamp conversion");
    check(frame_interval_us(1016667, V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, 1000000, V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) == 16667, "interval");
    check(!frame_interval_us(1000000, V4L2_BUF_FLAG_TIMESTAMP_COPY, 900000, V4L2_BUF_FLAG_TIMESTAMP_COPY), "copy clock interval assumed");
    check(!frame_interval_us(900000, V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, 1000000, V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC), "backwards timestamp accepted");

    int p[2]; if (pipe(p) < 0) fail_errno("pipe");
    struct sigaction sa{}; sa.sa_handler = alarm_handler; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, nullptr) < 0) fail_errno("sigaction");
    itimerval timer{}; timer.it_value.tv_usec = 5000;
    if (setitimer(ITIMER_REAL, &timer, nullptr) < 0) fail_errno("setitimer");
    expect_error([&] { wait_for_frame(p[0], std::chrono::steady_clock::now() + std::chrono::milliseconds(40)); }, "timeout not reported");
    check(signal_seen, "poll EINTR was not triggered");
    check(write(p[1], "x", 1) == 1, "pipe write");
    wait_for_frame(p[0], std::chrono::steady_clock::now() + std::chrono::seconds(1));
    close(p[0]); close(p[1]);

    char dir_template[] = "/tmp/edgedoc-r1-test-XXXXXX";
    char* dir = mkdtemp(dir_template); if (!dir) fail_errno("mkdtemp");
    const std::string root(dir), target = root + "/image";
    {
        TemporaryOutput tmp(target); tmp.write_and_close("payload", 7); tmp.publish(target);
    }
    check(read_file(target) == "payload", "published content mismatch");
    check(!has_temporary(root), "temporary file after successful publish");
    {
        TemporaryOutput tmp(target); tmp.write_and_close("new", 3);
        expect_error([&] { tmp.publish(target); }, "existing target overwritten");
    }
    check(read_file(target) == "payload", "existing target changed");
    check(!has_temporary(root), "temporary file after rename failure");
    expect_error([&] { TemporaryOutput tmp(root + "/missing/file"); }, "missing directory accepted");
    {
        const std::string plane = root + "/partial-plane", metadata = root + "/metadata";
        { TemporaryOutput old(metadata); old.write_and_close("old", 3); old.publish(metadata); }
        {
            TemporaryOutput plane_tmp(plane), metadata_tmp(metadata);
            plane_tmp.write_and_close("plane", 5); metadata_tmp.write_and_close("new", 3);
            plane_tmp.publish(plane);
            expect_error([&] { metadata_tmp.publish(metadata); }, "metadata collision accepted");
        }
        check(read_file(plane) == "plane" && read_file(metadata) == "old", "partial publication policy failed");
        check(!has_temporary(root), "temporary file after partial publication failure");
        check(unlink(plane.c_str()) == 0 && unlink(metadata.c_str()) == 0, "remove owned partial-publication fixtures");
    }
    pid_t child = fork(); if (child < 0) fail_errno("fork");
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN); rlimit lim{0, 0};
        if (setrlimit(RLIMIT_FSIZE, &lim) < 0) _exit(2);
        try { TemporaryOutput tmp(root + "/write-failure"); tmp.write_and_close("x", 1); _exit(3); }
        catch (const std::exception&) { _exit(0); }
    }
    int status = 0; if (waitpid(child, &status, 0) < 0) fail_errno("waitpid");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "controlled write failure did not fail correctly");
    check(!has_temporary(root), "temporary file after write failure");
    check(access((root + "/write-failure").c_str(), F_OK) < 0 && errno == ENOENT, "failed output was published");
    check(unlink(target.c_str()) == 0, "remove owned test file");
    check(rmdir(root.c_str()) == 0, "remove owned test directory");
}
} // namespace

int main() {
    try { run_tests(); std::cout << "PASS: arguments, poll flags, EINTR/timeout, EAGAIN, payloads, timestamps, atomic output failures\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
