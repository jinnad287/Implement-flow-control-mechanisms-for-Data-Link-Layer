#include "common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// ================= MAC helper =================
void setMac(uint8_t mac[MAC_LEN], uint8_t lastByte) {
    for (int i = 0; i < MAC_LEN - 1; i++) mac[i] = 0xAA;
    mac[MAC_LEN - 1] = lastByte;
}

// ============================ CRC-32 ================================
static uint32_t crcTable[256];
static bool     crcTableReady = false;

static void buildCrcTable() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTable[i] = c;
    }
    crcTableReady = true;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    if (!crcTableReady) buildCrcTable();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ================= Data frame wire format =================
// Layout on the wire: src(6) dst(6) length(2, big-endian) seq(1) payload(N) fcs(4, big-endian)
std::vector<uint8_t> serializeDataFrame(const DataFrame& f) {
    std::vector<uint8_t> buf(FRAME_WIRE_LEN);
    size_t p = 0;
    memcpy(&buf[p], f.src, MAC_LEN); p += MAC_LEN;
    memcpy(&buf[p], f.dst, MAC_LEN); p += MAC_LEN;
    buf[p++] = (f.length >> 8) & 0xFF;
    buf[p++] = f.length & 0xFF;
    buf[p++] = f.seq;
    memcpy(&buf[p], f.payload, PAYLOAD_SIZE); p += PAYLOAD_SIZE;

    uint32_t fcs = crc32(buf.data(), p);   // CRC over header + payload only
    buf[p++] = (fcs >> 24) & 0xFF;
    buf[p++] = (fcs >> 16) & 0xFF;
    buf[p++] = (fcs >> 8)  & 0xFF;
    buf[p++] =  fcs        & 0xFF;
    return buf;
}

DataFrame deserializeDataFrame(const std::vector<uint8_t>& buf) {
    DataFrame f{};
    size_t p = 0;
    memcpy(f.src, &buf[p], MAC_LEN); p += MAC_LEN;
    memcpy(f.dst, &buf[p], MAC_LEN); p += MAC_LEN;
    f.length = (uint16_t)((buf[p] << 8) | buf[p + 1]); p += 2;
    f.seq = buf[p++];
    memcpy(f.payload, &buf[p], PAYLOAD_SIZE); p += PAYLOAD_SIZE;
    f.fcs = ((uint32_t)buf[p] << 24) | ((uint32_t)buf[p+1] << 16) |
            ((uint32_t)buf[p+2] << 8) | (uint32_t)buf[p+3];
    return f;
}

bool isDataFrameCorrupt(const DataFrame& f, const std::vector<uint8_t>& rawBuf) {
    uint32_t computed = crc32(rawBuf.data(), HEADER_LEN + PAYLOAD_SIZE);
    return computed != f.fcs;
}

// ================= ACK frame wire format =================
std::vector<uint8_t> serializeAckFrame(const AckFrame& f) {
    std::vector<uint8_t> buf(ACK_WIRE_LEN);
    size_t p = 0;
    memcpy(&buf[p], f.src, MAC_LEN); p += MAC_LEN;
    memcpy(&buf[p], f.dst, MAC_LEN); p += MAC_LEN;
    buf[p++] = f.ack;

    uint32_t fcs = crc32(buf.data(), p);
    buf[p++] = (fcs >> 24) & 0xFF;
    buf[p++] = (fcs >> 16) & 0xFF;
    buf[p++] = (fcs >> 8)  & 0xFF;
    buf[p++] =  fcs        & 0xFF;
    return buf;
}

AckFrame deserializeAckFrame(const std::vector<uint8_t>& buf) {
    AckFrame f{};
    size_t p = 0;
    memcpy(f.src, &buf[p], MAC_LEN); p += MAC_LEN;
    memcpy(f.dst, &buf[p], MAC_LEN); p += MAC_LEN;
    f.ack = buf[p++];
    f.fcs = ((uint32_t)buf[p] << 24) | ((uint32_t)buf[p+1] << 16) |
            ((uint32_t)buf[p+2] << 8) | (uint32_t)buf[p+3];
    return f;
}

bool isAckFrameCorrupt(const AckFrame& f, const std::vector<uint8_t>& rawBuf) {
    uint32_t computed = crc32(rawBuf.data(), MAC_LEN * 2 + 1);
    return computed != f.fcs;
}

// ================= Channel() =================
static std::mt19937 rng(std::random_device{}());

bool applyChannel(std::vector<uint8_t>& buf, const ChannelParams& params) {
    std::uniform_real_distribution<double> prob(0.0, 1.0);
    std::uniform_int_distribution<int> delayDist(params.minDelayMs, params.maxDelayMs);

    // Every frame picks up some propagation/processing delay.
    std::this_thread::sleep_for(std::chrono::milliseconds(delayDist(rng)));

    // Random loss - the frame just never makes it.
    if (prob(rng) < params.lossProb) return false;

    // Random single-bit error somewhere in the frame.
    if (prob(rng) < params.errorProb) {
        std::uniform_int_distribution<size_t> byteDist(0, buf.size() - 1);
        std::uniform_int_distribution<int> bitDist(0, 7);
        size_t byteIdx = byteDist(rng);
        int    bitIdx  = bitDist(rng);
        buf[byteIdx] ^= (uint8_t)(1 << bitIdx);
    }
    return true;
}

// ================= Sockets =================
int createUdpSocket() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) perror("socket");
    return s;
}

bool bindUdpSocket(int sockfd, int port) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return false;
    }
    return true;
}

//to monitor the socket. This function pauses the program
//until either an ACK arrives on the socket or the remaining time runs out
bool waitReadable(int sockfd, double timeoutMs) {
    if (timeoutMs < 0) timeoutMs = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);
    timeval tv;
    tv.tv_sec  = (long)(timeoutMs / 1000);
    tv.tv_usec = (long)((timeoutMs - tv.tv_sec * 1000) * 1000);
    int ret = select(sockfd + 1, &fds, nullptr, nullptr, &tv);
    return ret > 0 && FD_ISSET(sockfd, &fds);
}

// ================= Timing =================
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ================= RTOEstimator (Timer()/Timeout()) =================
RTOEstimator::RTOEstimator()
    : estimatedRTT(0), devRTT(0), timeoutMs(INITIAL_TIMEOUT_MS), firstSample(true) {}

void RTOEstimator::sample(double sampleRttMs) {
    const double ALPHA = 0.125, BETA = 0.25;   // standard TCP smoothing constants
    if(firstSample){
        estimatedRTT = sampleRttMs;
        devRTT       = sampleRttMs / 2.0;
        firstSample  = false;
    }
    else{
        devRTT       = (1 - BETA)  * devRTT       + BETA  * std::fabs(estimatedRTT - sampleRttMs);
        estimatedRTT = (1 - ALPHA) * estimatedRTT + ALPHA * sampleRttMs;
    }
    timeoutMs = estimatedRTT + 4 * devRTT;
    timeoutMs = std::max(MIN_TIMEOUT_MS, std::min(MAX_TIMEOUT_MS, timeoutMs));
}

double RTOEstimator::getTimeoutMs() const { return timeoutMs; }

// ================= File chunking =================
std::vector<std::vector<uint8_t>> chunkFile(const std::string& path, int chunkSize) {
    std::vector<std::vector<uint8_t>> chunks;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Cannot open input file: " << path << "\n";
        return chunks;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t i = 0; i < buf.size(); i += chunkSize) {
        size_t end = std::min(buf.size(), i + (size_t)chunkSize);
        chunks.emplace_back(buf.begin() + i, buf.begin() + end);
    }
    return chunks;
}

// ================= Logging =================
void logLine(const std::string& who, const std::string& msg) {
    std::cout << "[" << who << "] " << msg << std::endl;
}
