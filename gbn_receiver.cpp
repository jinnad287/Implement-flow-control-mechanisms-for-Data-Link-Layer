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

    int  expected = 0;    // next frame index we're willing to accept; also our next ACK value
    bool finished = false;

    logLine("RECEIVER", "Ready (Go-Back-N), waiting for frames...");


    while (true) {
        if (!waitReadable(sock, finished ? RECEIVER_LINGER_MS : 24.0 * 3600 * 1000)) {
            if (finished) break;
            continue;
        }
        std::vector<uint8_t> buf(FRAME_WIRE_LEN);
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
        if (n != (int)FRAME_WIRE_LEN) continue;

        DataFrame frame = deserializeDataFrame(buf);
        if (isDataFrameCorrupt(frame, buf)) {
            logLine("RECEIVER", "Frame seq=" + std::to_string(frame.seq) + " CORRUPT, discarding");
            continue;   // no ACK for corrupted frames
        }

        if (frame.seq == (uint8_t)(expected % 256)) {
            if (frame.length == 0) {
                logLine("RECEIVER", "EOF frame received");
                finished = true;
            } else {
                out.write((const char*)frame.payload, frame.length);
                logLine("RECEIVER", "In-order frame seq=" + std::to_string(frame.seq) + " accepted");
            }
            expected++;
        } else {
            logLine("RECEIVER", "Out-of-order frame seq=" + std::to_string(frame.seq) +
                     " (expected " + std::to_string(expected % 256) + "), discarding");
            // fall through so we still re-send the cumulative ACK below
        }

        AckFrame ack{};
        setMac(ack.src, 0x02);
        setMac(ack.dst, 0x01);
        ack.ack = (uint8_t)expected;   // cumulative "next expected" value
        std::vector<uint8_t> rawAck = serializeAckFrame(ack);
        std::vector<uint8_t> onWire = rawAck;
        if (applyChannel(onWire, channel))
            sendto(sock, onWire.data(), onWire.size(), 0, (sockaddr*)&from, fl);
        else
            logLine("RECEIVER", "ACK " + std::to_string(ack.ack) + " LOST in channel");
    }

    out.close();
    close(sock);
    logLine("RECEIVER", "Output written to " + outFile);
    return 0;
}
