#include "common.h"

#include <cstring>
#include <fstream>
#include <iostream>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <output_file> <loss_prob> <error_prob>\n";
        return 1;
    }
    std::string outFile = argv[1];
    ChannelParams channel;
    channel.lossProb  = std::stod(argv[2]);
    channel.errorProb = std::stod(argv[3]);

    int sock = createUdpSocket();
    if (sock < 0 || !bindUdpSocket(sock, RECEIVER_PORT)) return 1;

    std::ofstream out(outFile, std::ios::binary);
    uint8_t expectedSeq = 0;
    bool finished = false;

    logLine("RECEIVER", "Ready (Stop-and-Wait), waiting for frames...");


    while (true) {
        if (!waitReadable(sock, finished ? RECEIVER_LINGER_MS : 24.0 * 3600 * 1000)) {
            if (finished) break;   // quiet channel after finishing -> safe to exit
            continue;
        }
        std::vector<uint8_t> buf(FRAME_WIRE_LEN);
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
        if (n != (int)FRAME_WIRE_LEN) continue;   // ignore garbage-sized packets

        // Check(): CRC/checksum check
        DataFrame frame = deserializeDataFrame(buf);
        if (isDataFrameCorrupt(frame, buf)) {
            logLine("RECEIVER", "Frame seq=" + std::to_string(frame.seq) + " CORRUPT, discarding, no ACK");
            continue;
        }

        if (frame.seq == expectedSeq) {
            if (frame.length == 0) {
                logLine("RECEIVER", "EOF frame received, transmission complete");
                finished = true;
            } else {
                out.write((const char*)frame.payload, frame.length);
                logLine("RECEIVER", "Accepted frame seq=" + std::to_string(frame.seq));
            }
            expectedSeq ^= 1;
        } else {
            logLine("RECEIVER", "Duplicate frame seq=" + std::to_string(frame.seq) + ", re-ACKing");
        }

        // Send(): always ACK whichever seq we just processed (new or duplicate)
        AckFrame ack{};
        setMac(ack.src, 0x02);
        setMac(ack.dst, 0x01);
        ack.ack = frame.seq;
        std::vector<uint8_t> rawAck = serializeAckFrame(ack);
        std::vector<uint8_t> onWire = rawAck;
        if (applyChannel(onWire, channel))
            sendto(sock, onWire.data(), onWire.size(), 0, (sockaddr*)&from, fl);
        else
            logLine("RECEIVER", "ACK for seq=" + std::to_string(ack.ack) + " LOST in channel");
    }

    out.close();
    close(sock);
    logLine("RECEIVER", "Output written to " + outFile);
    return 0;
}
