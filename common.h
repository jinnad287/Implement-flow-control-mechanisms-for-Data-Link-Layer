#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  Frame layout (see Fig. 1 in the assignment sheet)
//
//  Header : Source MAC(6) + Destination MAC(6) + Length(2) + Seq No(1)
//  Data   : Payload (46 - 1500 bytes, fixed size per run)
//  Trailer: FCS - CRC-32 (4 bytes)
//
// ============================================================

const int PAYLOAD_SIZE   = 46;                 // pick any value 46-1500
const int MAC_LEN        = 6;
const int HEADER_LEN     = MAC_LEN + MAC_LEN + 2 + 1;   // 15 bytes
const int TRAILER_LEN    = 4;                            // CRC-32
const int FRAME_WIRE_LEN = HEADER_LEN + PAYLOAD_SIZE + TRAILER_LEN;

const int RECEIVER_PORT   = 9000;   // Receiver always listens here
const int SENDER_PORT     = 9001;   // Sender listens here for ACKs
const char* const HOST_IP = "127.0.0.1";

const double INITIAL_TIMEOUT_MS = 500.0;
const double MIN_TIMEOUT_MS     = 50.0;
const double MAX_TIMEOUT_MS     = 5000.0;

// How long a receiver keeps listening, after it has locally finished,
// for a duplicate EOF frame the sender might still be retransmitting
// (because our very last ACK got lost in the channel). Without this,
// the receiver could exit while the sender is still stuck resending
// forever. Same idea as TCP's TIME_WAIT state.
const double RECEIVER_LINGER_MS = 3000.0;

// ------------------------------------------------------------
//  Data frame : Sender -> Receiver
// ------------------------------------------------------------
struct DataFrame {
    uint8_t  src[MAC_LEN];
    uint8_t  dst[MAC_LEN];
    uint16_t length;              // valid bytes inside payload (0 = end-of-stream marker)
    uint8_t  seq;                 // frame sequence number
    uint8_t  payload[PAYLOAD_SIZE];
    uint32_t fcs;                 // CRC-32 over header + payload
};

// ------------------------------------------------------------
//  ACK frame : Receiver -> Sender
// ------------------------------------------------------------
struct AckFrame {
    uint8_t  src[MAC_LEN];
    uint8_t  dst[MAC_LEN];
    uint8_t  ack;                 // meaning depends on the protocol using it
    uint32_t fcs;
};
const int ACK_WIRE_LEN = MAC_LEN + MAC_LEN + 1 + 4;

// ---- MAC helper ----
void setMac(uint8_t mac[MAC_LEN], uint8_t lastByte);

// ---- CRC-32 (the "assignment 1" checksum module) ----
uint32_t crc32(const uint8_t* data, size_t len);

// ---- Frame <-> wire-byte conversions ----
std::vector<uint8_t> serializeDataFrame(const DataFrame& f);
DataFrame             deserializeDataFrame(const std::vector<uint8_t>& buf);
bool                  isDataFrameCorrupt(const DataFrame& f, const std::vector<uint8_t>& rawBuf);

std::vector<uint8_t> serializeAckFrame(const AckFrame& f);
AckFrame              deserializeAckFrame(const std::vector<uint8_t>& buf);
bool                  isAckFrameCorrupt(const AckFrame& f, const std::vector<uint8_t>& rawBuf);

// ------------------------------------------------------------
//  Channel(): random delay + random bit error + random loss.
//  Shared because both the sender (outgoing data frames) and the
//  receiver (outgoing ACK frames) push their traffic through it.
// ------------------------------------------------------------
struct ChannelParams {
    double lossProb   = 0.0;   // probability the frame never arrives
    double errorProb  = 0.0;   // probability a single bit gets flipped
    int    minDelayMs = 5;
    int    maxDelayMs = 40;
};

// Sleeps for the simulated propagation delay, then (maybe) corrupts
// `buf` in place. Returns false if the frame should be dropped
// entirely (i.e. never actually put on the wire).
bool applyChannel(std::vector<uint8_t>& buf, const ChannelParams& params);

// ---- Sockets ----
int  createUdpSocket();
bool bindUdpSocket(int sockfd, int port);
bool waitReadable(int sockfd, double timeoutMs);   // select() with a timeout

// ---- Timing ----
double nowMs();

// ------------------------------------------------------------
//  RTOEstimator implements the Sender's Timer()/Timeout() methods:
//  classic Jacobson/Karels smoothing of measured RTT samples.
// ------------------------------------------------------------
class RTOEstimator {
public:
    RTOEstimator();
    void   sample(double sampleRttMs);   // feed a fresh RTT measurement
    double getTimeoutMs() const;
private:
    double estimatedRTT;
    double devRTT;
    double timeoutMs;
    bool   firstSample;
};

// ---- Split a file into fixed-size payload chunks ----
std::vector<std::vector<uint8_t>> chunkFile(const std::string& path, int chunkSize);

// ---- Small logging helper so every program prints in the same format ----
void logLine(const std::string& who, const std::string& msg);

#endif
