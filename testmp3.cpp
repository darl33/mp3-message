#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <vector>

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

const int sampleRates[3][3] = {
    {44100, 48000, 32000}, // MPEG 1
    {22050, 24000, 16000}, // MPEG 2
    {11025, 12000, 8000}   // MPEG 2.5
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

int skipID3Tag(const char* buffer, int bufferSize) {
    // Check if file starts with ID3v2 tag
    if (bufferSize >= 10 && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
        // ID3v2 size is in bytes 6-9 (synchsafe integer)
        int id3Size = ((buffer[6] & 0x7F) << 21) |
                      ((buffer[7] & 0x7F) << 14) |
                      ((buffer[8] & 0x7F) << 7) |
                      (buffer[9] & 0x7F);
        
        // Return position after ID3 header (10 bytes) + tag data
        return 10 + id3Size;
    }
    
    return 0; // No ID3 tag, start at beginning
}

int main() {
    ifstream mp3File;
    string name;
    cin >> name;
    mp3File.open(name, ios::in | ios::binary);

    if (!mp3File.is_open()) {
        return 1;
    }

   //inefficient way to read entire file into memory- but simplest
    mp3File.seekg(0, ios::end);
    streampos fileSize = mp3File.tellg();
    mp3File.seekg(0, ios::beg);
    vector<char> buffer(fileSize);
    mp3File.read(buffer.data(), fileSize);

    cout << "File size: " << fileSize << " bytes" << endl;

    // Skip ID3 tag if present
    int startOffset = skipID3Tag(buffer.data(), buffer.size());
    
    if (startOffset > 0) {
        cout << "Skipped ID3 tag of " << startOffset << " bytes" << endl << endl;
    }

    // Header size is always 4 bytes
    const int HEADER_SIZE = 4;
    int frameCount = 0;

    // Parse frames
    for (int i = startOffset; i < (buffer.size() - 4); ) {
        // Try to parse frame at current position
        MP3FrameInfo frameInfo = parseFrameHeader(buffer.data(), i);
        
        if (!frameInfo.valid) {
            i++; // Move forward one byte and keep searching
            continue;
        }

        frameCount++;
        cout << "=== Frame " << frameCount << " at position " << i << " ===" << endl;
        cout << "Frame Size: " << frameInfo.frameSize << endl;
        cout << "Header Size: " << HEADER_SIZE << endl;
        cout << "Side Info Size: " << frameInfo.sideInfoSize << endl;
        cout << "Channel Mode: " << (frameInfo.isMono ? "Mono" : "Stereo") << endl;
        cout << "VBR Header: " << (frameInfo.isVBR ? "Yes" : "No") << endl;

        if (frameInfo.isVBR) {
            cout << "Note: This frame contains VBR metadata" << endl;
        }
        cout << endl;

        // Jump to next frame
        i += frameInfo.frameSize;
    }

    cout << "Total frames parsed: " << frameCount << endl;

    //TODO:
    //      - Ancillary data extraction
    //      - Write to ancillary data
    //

    mp3File.close();
    return 0;
}