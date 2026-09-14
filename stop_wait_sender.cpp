#include "common.h"

#include <cstring>
#include <iostream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>


int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <loss_prob> <error_prob>\n";
        return 1;
    }
    std::string inputFile = argv[1];
    ChannelParams channel;
    channel.lossProb  = std::stod(argv[2]);
    channel.errorProb = std::stod(argv[3]);

    auto chunks = chunkFile(inputFile, PAYLOAD_SIZE);
    logLine("SENDER", "Loaded " + std::to_string(chunks.size()) + " frame(s) from " + inputFile);

    int sock = createUdpSocket();
    if (sock < 0 || !bindUdpSocket(sock, SENDER_PORT)) return 1;

    sockaddr_in receiverAddr{};
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_port   = htons(RECEIVER_PORT);
    inet_pton(AF_INET, HOST_IP, &receiverAddr.sin_addr);

    RTOEstimator rto;
    uint8_t seq = 0;
    int totalTransmissions = 0, retransmissions = 0;
    double startTime = nowMs();

    // Channel() : push a frame onto the (simulated lossy/noisy) wire
    auto putOnWire = [&](const DataFrame& f) {
        std::vector<uint8_t> raw    = serializeDataFrame(f);
        std::vector<uint8_t> onWire = raw;   // channel may corrupt this copy only
        if (applyChannel(onWire, channel))
            sendto(sock, onWire.data(), onWire.size(), 0, (sockaddr*)&receiverAddr, sizeof(receiverAddr));
        else
            logLine("SENDER", "Frame seq=" + std::to_string(f.seq) + " LOST in channel");
    };

    // Send() + Timer() + Timeout() + Recv() rolled into one helper:
    // keeps retransmitting a single frame until its ACK is confirmed.
    auto sendAndWaitForAck = [&](DataFrame& frame) {
        bool acked = false;
        while (!acked) {
            double sentAt = nowMs();
            putOnWire(frame);
            totalTransmissions++;
            logLine("SENDER", "Sent frame seq=" + std::to_string(frame.seq) +
                     " (" + std::to_string(frame.length) + " bytes), timeout=" +
                     std::to_string((int)rto.getTimeoutMs()) + "ms");

            if(waitReadable(sock, rto.getTimeoutMs())){
                std::vector<uint8_t> buf(ACK_WIRE_LEN);
                sockaddr_in from{};
                socklen_t fl = sizeof(from);
                int n = recvfrom(sock, buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
                if (n == (int)ACK_WIRE_LEN) {
                    AckFrame ack = deserializeAckFrame(buf);
                    if (!isAckFrameCorrupt(ack, buf) && ack.ack == frame.seq) {
                        double rtt = nowMs() - sentAt;
                        rto.sample(rtt);
                        logLine("SENDER", "ACK seq=" + std::to_string(ack.ack) +
                                 " OK, RTT=" + std::to_string((int)rtt) +
                                 "ms, new timeout=" + std::to_string((int)rto.getTimeoutMs()) + "ms");
                        acked = true;
                    } else {
                        logLine("SENDER", "Ignoring corrupt or unexpected ACK");
                    }
                }
            } else {
                logLine("SENDER", "TIMEOUT on seq=" + std::to_string(frame.seq) + ", retransmitting");
                retransmissions++;
            }
        }
    };

    for (size_t i = 0; i < chunks.size(); i++) {
        DataFrame frame{};
        setMac(frame.src, 0x01);
        setMac(frame.dst, 0x02);
        frame.length = (uint16_t)chunks[i].size();
        frame.seq    = seq;
        memcpy(frame.payload, chunks[i].data(), chunks[i].size());

        sendAndWaitForAck(frame);
        seq ^= 1;   // flip the alternating bit
    }

    // EOF
    DataFrame eof{};
    setMac(eof.src, 0x01);
    setMac(eof.dst, 0x02);
    eof.length = 0;
    eof.seq    = seq;
    sendAndWaitForAck(eof);

    double totalTime = nowMs() - startTime;
    std::cout << "\n----- Stop-and-Wait summary -----\n";
    std::cout << "Data frames delivered  : " << chunks.size() << "\n";
    std::cout << "Total transmissions    : " << totalTransmissions << " (incl. retransmits + EOF)\n";
    std::cout << "Retransmissions        : " << retransmissions << "\n";
    std::cout << "Total time             : " << totalTime << " ms\n";
    std::cout << "Efficiency (useful/total frames sent): "
              << (double)chunks.size() / totalTransmissions << "\n";

    close(sock);
    return 0;
}
