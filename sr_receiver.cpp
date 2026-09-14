#include "common.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <output_file> <N> <loss_prob> <error_prob>\n";
        return 1;
    }
    std::string outFile = argv[1];
    int N = std::stoi(argv[2]);
    ChannelParams channel;
    channel.lossProb  = std::stod(argv[3]);
    channel.errorProb = std::stod(argv[4]);

    int sock = createUdpSocket();
    if (sock < 0 || !bindUdpSocket(sock, RECEIVER_PORT)) return 1;
    std::ofstream out(outFile, std::ios::binary);

    int  base     = 0;
    bool finished = false;

    // seq -> (payload bytes, isEofMarker)
    std::map<int, std::pair<std::vector<uint8_t>, bool>> buffer;

    logLine("RECEIVER", "Ready (Selective Repeat), window N=" + std::to_string(N));


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
        if(isDataFrameCorrupt(frame, buf)){
            logLine("RECEIVER", "Frame seq=" + std::to_string(frame.seq) + " CORRUPT, discarding");
            continue;
        }

        int  seq          = frame.seq;
        bool withinWindow = (seq >= base && seq < base + N);
        bool isOld        = (seq < base);   // already delivered in an earlier round

        if(withinWindow){
            if(!buffer.count(seq)){
                std::vector<uint8_t> data(frame.payload, frame.payload + frame.length);
                buffer[seq] = { data, frame.length == 0 };
                logLine("RECEIVER", "Buffered frame seq=" + std::to_string(seq) +
                         (seq == base ? " (in order)" : " (out of order)"));
            } else {
                logLine("RECEIVER", "Duplicate frame seq=" + std::to_string(seq));
            }

            // Flush every contiguous frame starting at the window's left edge.
            while(buffer.count(base)){
                auto& entry = buffer[base];
                if(entry.second){// EOF or not
                    finished = true;
                } else {
                    out.write((const char*)entry.first.data(), entry.first.size());
                }
                buffer.erase(base);
                base++;
                if (finished) break;
            }
            
        } else if(isOld){
            logLine("RECEIVER", "Duplicate (already delivered) frame seq=" + std::to_string(seq));
        } else {
            logLine("RECEIVER", "Frame seq=" + std::to_string(seq) + " outside receive window, discarding");
        }

        // ACK every frame that passed the CRC check and was in (or behind) our window.
        if(withinWindow || isOld) {
            AckFrame ack{};
            setMac(ack.src, 0x02);
            setMac(ack.dst, 0x01);
            ack.ack = (uint8_t)seq;   // individual ACK
            std::vector<uint8_t> rawAck = serializeAckFrame(ack);
            std::vector<uint8_t> onWire = rawAck;
            if (applyChannel(onWire, channel))
                sendto(sock, onWire.data(), onWire.size(), 0, (sockaddr*)&from, fl);
            else
                logLine("RECEIVER", "ACK " + std::to_string(seq) + " LOST in channel");
        }
    }

    out.close();
    close(sock);
    logLine("RECEIVER", "Output written to " + outFile);
    return 0;
}
