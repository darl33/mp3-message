#include <iostream>
#include <fstream>
#include <string>
#include <cmath> // For floor()



using namespace std;

// Bitrate LUT
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

const int sideInfoSizes[2][2] = {
    {17, 32}, // MPEG 1: {Mono, Stereo}
    {9, 17}   // MPEG 2 & 2.5: {Mono, Stereo}
};

struct MP3FrameInfo {
    int mpegVersionID;
    int layerDesc;
    int bitrateIndex;
    int sampleRateIndex;
    int sampleRate;
    int padding;
    bool isMono;
    int frameSize;
    int sideInfoSize;
    bool isVBR;
    bool valid;
};

int getSampleRate(int mpegVersionID, int sampleRateIndex) {
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
        return -1;
    }
    
    return sampleRates[mpegVersionIdx][sampleRateIndex];
}

int getSideInfoSize(int mpegVersionID, bool isMono) {
    int versionIdx = (mpegVersionID == 3) ? 0 : 1; // 0 for MPEG-1, 1 for MPEG-2/2.5
    int channelIdx = isMono ? 0 : 1; // 0 for mono, 1 for stereo
    return sideInfoSizes[versionIdx][channelIdx];
}

int calculateFrameSize(int mpegVersionID, int layerDesc, int bitrateIndex, int sampleRate, int padding) {
    // Sample Rate Lookup
    const int sampleRates[3][3] = {
        {44100, 48000, 32000}, // MPEG 1
        {22050, 24000, 16000}, // MPEG 2
        {11025, 12000, 8000}   // MPEG 2.5
    };

    int versionIdx = (mpegVersionID == 3) ? 0 : 1;
    int layerIdx;
    if (layerDesc == 3) layerIdx = 0;
    else if (layerDesc == 2) layerIdx = 1;
    else layerIdx = 2;

    if (bitrateIndex == 0 || bitrateIndex > 14 || layerDesc == 0) {
        return -1; // Invalid frame
    }
    
    int bitrate = bitrates[versionIdx][layerIdx][bitrateIndex] * 1000;

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

    return frameSize;
}

bool isVBRHeader(const char* buffer, int frameStart, int sideInfoSize) {
    // VBR header comes after frame header (4 bytes) + side info
    int vbrHeaderOffset = frameStart + 4 + sideInfoSize;
    
    // Check for Xing or Info tag
    if (vbrHeaderOffset + 4 < 8192) { // no out of bounds
        if ((buffer[vbrHeaderOffset] == 'X' && buffer[vbrHeaderOffset + 1] == 'i' &&
             buffer[vbrHeaderOffset + 2] == 'n' && buffer[vbrHeaderOffset + 3] == 'g') ||
            (buffer[vbrHeaderOffset] == 'I' && buffer[vbrHeaderOffset + 1] == 'n' &&
             buffer[vbrHeaderOffset + 2] == 'f' && buffer[vbrHeaderOffset + 3] == 'o')) {
            return true;
        }
    }
    
    // Check for VBRI header
    // VBRI appears at a fixed offset of 36 bytes after frame header
    int vbriOffset = frameStart + 36;
    if (vbriOffset + 4 < 8192) {
        if (buffer[vbriOffset] == 'V' && buffer[vbriOffset + 1] == 'B' &&
            buffer[vbriOffset + 2] == 'R' && buffer[vbriOffset + 3] == 'I') {
            return true;
        }
    }
    
    return false;
}

MP3FrameInfo parseFrameHeader(const char* buffer, int position) {
    MP3FrameInfo info;
    info.valid = false;

    // Check sync word
    if (buffer[position] != (char)0xFF || (buffer[position + 1] & 0xE0) != 0xE0) {
        return info;
    }

    unsigned char byte1 = buffer[position + 1];
    unsigned char byte2 = buffer[position + 2];
    unsigned char byte3 = buffer[position + 3];

    // Extract header fields
    info.mpegVersionID = (byte1 & 0x18) >> 3;
    info.layerDesc = (byte1 & 0x06) >> 1;
    info.bitrateIndex = (byte2 & 0xF0) >> 4;
    info.sampleRateIndex = (byte2 & 0x0C) >> 2;
    info.padding = (byte2 & 0x02) >> 1;
    
    int channelmode = (byte3 & 0xC0) >> 6;
    info.isMono = (channelmode == 3);

    // Get sample rate
    info.sampleRate = getSampleRate(info.mpegVersionID, info.sampleRateIndex);
    if (info.sampleRate == -1) {
        return info;
    }

    // Calculate frame size
    info.frameSize = calculateFrameSize(info.mpegVersionID, info.layerDesc, 
                                        info.bitrateIndex, info.sampleRate, info.padding);
    if (info.frameSize == -1) {
        return info;
    }

    // Get side info size
    info.sideInfoSize = getSideInfoSize(info.mpegVersionID, info.isMono);

    // Check for VBR header
    info.isVBR = isVBRHeader(buffer, position, info.sideInfoSize);

    info.valid = true;
    return info;
}

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
        // Try to parse frame at current position
        MP3FrameInfo frameInfo = parseFrameHeader(buffer, i);
        
        if (!frameInfo.valid) {
            continue; // Not a valid frame, keep searching
        }

        // Header size is always 4 bytes
        const int HEADER_SIZE = 4;

        // Display frame information
        cout << "Frame Size: " << frameInfo.frameSize << endl;
        cout << "Header Size: " << HEADER_SIZE << endl;
        cout << "Side Info Size: " << frameInfo.sideInfoSize << endl;
        cout << "Channel Mode: " << (frameInfo.isMono ? "Mono" : "Stereo") << endl;
        cout << "VBR Header: " << (frameInfo.isVBR ? "Yes" : "No") << endl;

        if (frameInfo.isVBR) {
            cout << "Note: This frame contains VBR metadata, not audio data" << endl;
        }

        break;
    }

    //TODO:
    //      - multiframe handling
    //      - ID3 tag skipping
    //      - full MP3 parsing
    //      - ancillary data extraction
    //

    mp3File.close();
    return 0;
}