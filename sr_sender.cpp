#include "common.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <N> <loss_prob> <error_prob>\n";
        return 1;
    }
    std::string inputFile = argv[1];
    int N = std::stoi(argv[2]);
    ChannelParams channel;
    channel.lossProb  = std::stod(argv[3]);
    channel.errorProb = std::stod(argv[4]);

    auto chunks = chunkFile(inputFile, PAYLOAD_SIZE);
    int total = (int)chunks.size() + 1;   // +1 for trailing EOF frame
    logLine("SENDER", "Loaded " + std::to_string(chunks.size()) +
             " data frame(s), window N=" + std::to_string(N));

    std::vector<DataFrame> frames(total);
    for (int i = 0; i < (int)chunks.size(); i++) {
        setMac(frames[i].src, 0x01);
        setMac(frames[i].dst, 0x02);
        frames[i].length = (uint16_t)chunks[i].size();
        frames[i].seq    = (uint8_t)i;
        memcpy(frames[i].payload, chunks[i].data(), chunks[i].size());
    }
    setMac(frames[total - 1].src, 0x01);
    setMac(frames[total - 1].dst, 0x02);
    frames[total - 1].length = 0;
    frames[total - 1].seq    = (uint8_t)(total - 1);

    int sock = createUdpSocket();
    if (sock < 0 || !bindUdpSocket(sock, SENDER_PORT)) return 1;
    sockaddr_in receiverAddr{};
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_port   = htons(RECEIVER_PORT);
    inet_pton(AF_INET, HOST_IP, &receiverAddr.sin_addr);

    auto putOnWire = [&](const DataFrame& f) {
        std::vector<uint8_t> raw    = serializeDataFrame(f);
        std::vector<uint8_t> onWire = raw;
        if (applyChannel(onWire, channel))
            sendto(sock, onWire.data(), onWire.size(), 0, (sockaddr*)&receiverAddr, sizeof(receiverAddr));
        else
            logLine("SENDER", "Frame seq=" + std::to_string(f.seq) + " LOST in channel");
    };

    RTOEstimator rto;
    std::vector<bool>   sent(total, false), acked(total, false);
    std::vector<double> sendTime(total, 0.0);
    std::vector<int>    retransmitCount(total, 0);

    int base = 0, nextSeqNum = 0;
    int totalTransmissions = 0, retransmissions = 0;
    double startTime = nowMs();

    while(base < total){
        // Fill the window with any not-yet-sent frames.
        while(nextSeqNum < total && nextSeqNum < base + N){
            putOnWire(frames[nextSeqNum]);
            sendTime[nextSeqNum] = nowMs();
            sent[nextSeqNum]     = true;
            totalTransmissions++;
            logLine("SENDER", "Sent frame seq=" + std::to_string(frames[nextSeqNum].seq));
            nextSeqNum++;
        }

        // Find the soonest deadline among frames that are outstanding
        // (sent, not yet acked) - that's what we sleep on.
        double earliestDeadline = std::numeric_limits<double>::max();
        for (int i = base; i < nextSeqNum; i++)
            if (sent[i] && !acked[i])
                earliestDeadline = std::min(earliestDeadline, sendTime[i] + rto.getTimeoutMs());

        double waitMs = earliestDeadline - nowMs();
        if (waitMs < 0) waitMs = 0;

        
        if (waitReadable(sock, waitMs)) {
            std::vector<uint8_t> buf(ACK_WIRE_LEN);
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            int n = recvfrom(sock, buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
            if (n == (int)ACK_WIRE_LEN) {
                AckFrame ack = deserializeAckFrame(buf);
                if (!isAckFrameCorrupt(ack, buf)) {
                    int idx = ack.ack;   // individual ACK, not cumulative
                    if (idx >= base && idx < nextSeqNum && sent[idx] && !acked[idx]) {
                        acked[idx] = true;
                        if (retransmitCount[idx] == 0)   // Karn's algorithm
                            rto.sample(nowMs() - sendTime[idx]);
                        logLine("SENDER", "ACK for seq=" + std::to_string(idx) + " received");
                        while (base < total && acked[base]) base++;   // slide window forward
                    }
                }
            }
        } else {
            double now = nowMs();
            for (int i = base; i < nextSeqNum; i++) {
                if (sent[i] && !acked[i] && sendTime[i] + rto.getTimeoutMs() <= now) {
                    logLine("SENDER", "TIMEOUT, retransmitting seq=" + std::to_string(frames[i].seq));
                    putOnWire(frames[i]);
                    sendTime[i] = now;
                    retransmitCount[i]++;
                    totalTransmissions++;
                    retransmissions++;
                }
            }
        }
    }

    double totalTime = nowMs() - startTime;
    std::cout << "\n----- Selective Repeat summary (N=" << N << ") -----\n";
    std::cout << "Data frames delivered  : " << chunks.size() << "\n";
    std::cout << "Total transmissions    : " << totalTransmissions << " (incl. retransmits + EOF)\n";
    std::cout << "Retransmissions        : " << retransmissions << "\n";
    std::cout << "Total time             : " << totalTime << " ms\n";
    std::cout << "Efficiency (useful/total frames sent): "
              << (double)chunks.size() / totalTransmissions << "\n";

    close(sock);
    return 0;
}
