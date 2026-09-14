#include "common.h"

#include <cstring>
#include <iostream>
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
    int total = (int)chunks.size() + 1;   // +1 for the trailing EOF frame
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
    frames[total - 1].length = 0;                    // EOF marker
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
    int base = 0, nextSeqNum = 0;
    int totalTransmissions = 0, retransmissions = 0;
    double timerStart   = 0;
    bool   timerRunning = false;
    double startTime = nowMs();

    while(base < total){
        // slide new frames into the window while there's room
        while(nextSeqNum < total && nextSeqNum < base + N){
            putOnWire(frames[nextSeqNum]);
            totalTransmissions++;
            logLine("SENDER", "Sent frame seq=" + std::to_string(frames[nextSeqNum].seq));
            // if this is the very first frame in the window (base == nextSeqNum),
            // it starts the timer
            if(base == nextSeqNum){
                timerStart   = nowMs();
                timerRunning = true;
            }
            nextSeqNum++;
        }

        // the code calculates how much time is left before a timeout occurs
        double remaining = timerRunning ? rto.getTimeoutMs() - (nowMs() - timerStart)
                                         : rto.getTimeoutMs();
        if(remaining < 0) remaining = 0;

        if(timerRunning && waitReadable(sock, remaining)){
            std::vector<uint8_t> buf(ACK_WIRE_LEN);
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            int n = recvfrom(sock, buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
            if (n == (int)ACK_WIRE_LEN) {
                AckFrame ack = deserializeAckFrame(buf);
                if (!isAckFrameCorrupt(ack, buf)) {
                    int newBase = ack.ack;

                    if(newBase > base && newBase <= nextSeqNum){
                        double rtt = nowMs() - timerStart;
                        rto.sample(rtt);
                        logLine("SENDER", "Cumulative ACK -> base " + std::to_string(base) +
                                 " => " + std::to_string(newBase) +
                                 ", RTT=" + std::to_string((int)rtt) + "ms");
                        base = newBase;
                        if (base == nextSeqNum) timerRunning = false;   // window fully acked
                        else                    timerStart   = nowMs(); // restart for what's left
                    }
                }
            }
        } else if (timerRunning) {
            logLine("SENDER", "TIMEOUT, retransmitting window [" + std::to_string(base) +
                     "," + std::to_string(nextSeqNum - 1) + "]");
            for (int i = base; i < nextSeqNum; i++) {
                putOnWire(frames[i]);
                totalTransmissions++;
                retransmissions++;
            }
            timerStart = nowMs();
        }
    }

    double totalTime = nowMs() - startTime;
    std::cout << "\n----- Go-Back-N summary (N=" << N << ") -----\n";
    std::cout << "Data frames delivered  : " << chunks.size() << "\n";
    std::cout << "Total transmissions    : " << totalTransmissions << " (incl. retransmits + EOF)\n";
    std::cout << "Retransmissions        : " << retransmissions << "\n";
    std::cout << "Total time             : " << totalTime << " ms\n";
    std::cout << "Efficiency (useful/total frames sent): "
              << (double)chunks.size() / totalTransmissions << "\n";

    close(sock);
    return 0;
}
