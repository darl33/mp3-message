#include <iostream>
#include <fstream>
#include <string>
#include <cmath> // For floor()


//psa: ai used for comments here

using namespace std;

int main() {
    ifstream mp3File;
    string name;
    cin >> name;
    mp3File.open(name, ios::in | ios::binary);

    if (!mp3File.is_open()) {
        return 1;
    }

    // Buffer to read the start of the file
    char buffer[8192];
    mp3File.read(buffer, sizeof(buffer));

    for (int i = 0; i < (sizeof(buffer) - 4); ++i) {
        // Find the 11-bit sync word
        if (buffer[i] == (char)0xFF && (buffer[i + 1] & 0xE0) == 0xE0) {
            unsigned char byte1 = buffer[i + 1];
            unsigned char byte2 = buffer[i + 2];

            int mpegVersionID = (byte1 & 0x18) >> 3;
            int layerDesc = (byte1 & 0x06) >> 1;
            int bitrateIndex = (byte2 & 0xF0) >> 4;
            int sampleRateIndex = (byte2 & 0x0C) >> 2;
            int padding = (byte2 & 0x02) >> 1;

            // Sample Rate Lookup
            const int sampleRates[3][3] = {
                {44100, 48000, 32000}, // MPEG 1
                {22050, 24000, 16000}, // MPEG 2
                {11025, 12000, 8000}   // MPEG 2.5
            };

            int mpegVersionIdx;
            if (mpegVersionID == 3) mpegVersionIdx = 0;
            else if (mpegVersionID == 2) mpegVersionIdx = 1;
            else mpegVersionIdx = 2;

            if (sampleRateIndex > 2) {
                continue;
            }
            int sampleRate = sampleRates[mpegVersionIdx][sampleRateIndex];
            if (sampleRate == 0) {
                 continue;
            }

            // Bitrate Lookup
            const int bitrates[2][3][15] = {
                { // MPEG 1
                    {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448}, // Layer I
                    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384}, // Layer II
                    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}  // Layer III
                },
                { // MPEG 2 & 2.5
                    {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, // Layer I
                    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, // Layer II
                    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}  // Layer III
                }
            };
            
            int versionIdx = (mpegVersionID == 3) ? 0 : 1;
            int layerIdx;
            if (layerDesc == 3) layerIdx = 0;
            else if (layerDesc == 2) layerIdx = 1;
            else layerIdx = 2;

            if (bitrateIndex == 0 || bitrateIndex > 14 || layerDesc == 0) {
                continue;
            }
            int bitrate = bitrates[versionIdx][layerIdx][bitrateIndex] * 1000;

            // Calculate Frame Size
            int frameSize = 0;
            double dBitrate = (double)bitrate;
            double dSampleRate = (double)sampleRate;

            if (layerDesc == 3) { // Layer I
                frameSize = floor( (12 * dBitrate / dSampleRate) ) * 4;
                if(padding) {
                    frameSize += 4;
                }
            } else { // Layer II or III
                int coefficient = (mpegVersionID == 3) ? 144 : 72;
                frameSize = floor( (coefficient * dBitrate / dSampleRate) ) + padding;
            }

            cout << frameSize << endl;
            break;
        }
    }

    mp3File.close();
    return 0;
}