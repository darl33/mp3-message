#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdint>

using namespace std;

// Mode flags
enum Mode { MODE_PARSE, MODE_EXTRACT, MODE_WRITE };

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
    bool hasCRC;            // Protection bit clear: a 16-bit CRC follows the header
    int frameSize;
    int sideInfoSize;       // Layer III only; 0 for Layers I and II
    bool isVBR;
    bool valid;
    int position;           // Position in file
    int mainDataBegin;      // main_data_begin from side info
    int mainDataBits;       // Layer III: sum of part2_3_length over granules and channels
    int payloadOffset;      // File offset of the area after header, CRC and side info
    int payloadSize;        // Size of that area
    int ancillaryBytes;     // Layer III: bytes after this frame's main data that no frame uses
};

// A run of bytes in the file
struct Region {
    int offset;
    int size;
};

// Forward declarations
int parseMainDataBegin(const char* buffer, size_t bufferSize, int sideInfoStart, int mpegVersionID);
bool parseSideInfo(const unsigned char* sideInfo, MP3FrameInfo& info);

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

bool isVBRHeader(const char* buffer, size_t bufferSize, int frameStart, int sideInfoSize) {
    // VBR header comes after frame header (4 bytes) + side info
    int vbrHeaderOffset = frameStart + 4 + sideInfoSize;
    
    // Check for Xing or Info tag
    if ((size_t)vbrHeaderOffset + 4 <= bufferSize) { // no out of bounds
        if ((buffer[vbrHeaderOffset] == 'X' && buffer[vbrHeaderOffset + 1] == 'i' &&
             buffer[vbrHeaderOffset + 2] == 'n' && buffer[vbrHeaderOffset + 3] == 'g') ||
            (buffer[vbrHeaderOffset] == 'I' && buffer[vbrHeaderOffset + 1] == 'n' &&
             buffer[vbrHeaderOffset + 2] == 'f' && buffer[vbrHeaderOffset + 3] == 'o')) {
            return true;
        }
    }
    
    // Check for VBRI header
    int vbriOffset = frameStart + 36;
    if ((size_t)vbriOffset + 4 <= bufferSize) {
        if (buffer[vbriOffset] == 'V' && buffer[vbriOffset + 1] == 'B' &&
            buffer[vbriOffset + 2] == 'R' && buffer[vbriOffset + 3] == 'I') {
            return true;
        }
    }
    
    return false;
}

MP3FrameInfo parseFrameHeader(const char* buffer, size_t bufferSize, int position) {
    MP3FrameInfo info = MP3FrameInfo();
    info.valid = false;

    // The 4-byte header must be inside the buffer
    if (position < 0 || (size_t)position + 4 > bufferSize) {
        return info;
    }

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
    info.hasCRC = (byte1 & 0x01) == 0;
    
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

    // Side info exists only in Layer III (layerDesc 1)
    info.sideInfoSize = (info.layerDesc == 1) ? getSideInfoSize(info.mpegVersionID, info.isMono) : 0;
    int crcSize = info.hasCRC ? 2 : 0;
    int sideInfoStart = position + 4 + crcSize; // After header and CRC

    // The whole frame must be inside the buffer, and big enough for its side info
    info.payloadOffset = sideInfoStart + info.sideInfoSize;
    info.payloadSize = info.frameSize - (4 + crcSize + info.sideInfoSize);
    if (info.payloadSize < 0 || (size_t)position + info.frameSize > bufferSize) {
        return info;
    }

    // Xing/Info/VBRI tags are only looked for in the first frame (see main)
    info.isVBR = false;

    if (info.layerDesc == 1) {
        // Parse main_data_begin and part2_3_length from side info; reject impossible values
        if (!parseSideInfo(reinterpret_cast<const unsigned char*>(buffer) + sideInfoStart, info)) {
            return info;
        }
    } else {
        info.mainDataBegin = 0;
        info.mainDataBits = 0;
    }
    
    info.position = position;

    info.valid = true;
    return info;
}

int skipID3Tag(const char* buffer, size_t bufferSize) {
    // Check if file starts with ID3v2 tag
    if (bufferSize >= 10 && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
        // ID3v2 size is in bytes 6-9 (synchsafe integer: the top bit of each byte is 0)
        for (int b = 6; b <= 9; b++) {
            if (buffer[b] & 0x80) {
                return 0; // Not a valid ID3v2 header
            }
        }
        long id3Size = ((long)(buffer[6] & 0x7F) << 21) |
                       ((buffer[7] & 0x7F) << 14) |
                       ((buffer[8] & 0x7F) << 7) |
                       (buffer[9] & 0x7F);

        // Header (10 bytes) + tag data, plus a 10-byte footer if flag bit 4 is set
        long end = 10 + id3Size + ((buffer[5] & 0x10) ? 10 : 0);

        // A tag that claims to run past the end of the file leaves nothing to parse
        if ((size_t)end > bufferSize) {
            return (int)bufferSize;
        }
        return (int)end;
    }
    
    return 0; // No ID3 tag, start at beginning
}

// Parse main_data_begin from side info (first 9 bits for MPEG1, first 8 bits for MPEG2/2.5)
int parseMainDataBegin(const char* buffer, size_t bufferSize, int sideInfoStart, int mpegVersionID) {
    if ((size_t)sideInfoStart + 2 > bufferSize) {
        return -1;
    }
    unsigned char byte1 = buffer[sideInfoStart];
    unsigned char byte2 = buffer[sideInfoStart + 1];
    
    if (mpegVersionID == 3) { // MPEG 1
        // main_data_begin is 9 bits
        return ((byte1 << 1) | (byte2 >> 7)) & 0x1FF;
    } else { // MPEG 2/2.5
        // main_data_begin is 8 bits
        return byte1;
    }
}

// Reads big-endian bit fields, most significant bit first
struct BitReader {
    const unsigned char* data;
    int bitPos;
    unsigned read(int bits) {
        unsigned value = 0;
        for (int i = 0; i < bits; i++) {
            value = (value << 1) | ((data[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
            bitPos++;
        }
        return value;
    }
    void skip(int bits) { bitPos += bits; }
};

// Parse Layer III side info (ISO 11172-3 2.4.1.7, ISO 13818-3 2.4.1.7).
// Sets mainDataBegin and mainDataBits; returns false if a field is impossible,
// which usually means a false sync rather than a real frame.
bool parseSideInfo(const unsigned char* sideInfo, MP3FrameInfo& info) {
    bool mpeg1 = (info.mpegVersionID == 3);
    int channels = info.isMono ? 1 : 2;
    BitReader br = { sideInfo, 0 };

    info.mainDataBegin = br.read(mpeg1 ? 9 : 8);
    br.skip(mpeg1 ? (channels == 1 ? 5 : 3) : (channels == 1 ? 1 : 2)); // private bits
    if (mpeg1) {
        br.skip(4 * channels); // scfsi
    }

    int granules = mpeg1 ? 2 : 1;
    int bits = 0;
    for (int gr = 0; gr < granules; gr++) {
        for (int ch = 0; ch < channels; ch++) {
            int part23Length = br.read(12);
            int bigValues = br.read(9);
            br.skip(8);                     // global_gain
            br.skip(mpeg1 ? 4 : 9);         // scalefac_compress
            int windowSwitching = br.read(1);
            if (windowSwitching) {
                int blockType = br.read(2);
                br.skip(1 + 10 + 9);        // mixed_block_flag, table_select[2], subblock_gain[3]
                if (blockType == 0) {
                    return false;           // forbidden with window switching
                }
            } else {
                br.skip(15 + 4 + 3);        // table_select[3], region0_count, region1_count
            }
            br.skip(mpeg1 ? 3 : 2);         // preflag (MPEG-1 only), scalefac_scale, count1table_select
            if (bigValues > 288) {
                return false;               // more than 576 spectral values
            }
            bits += part23Length;
        }
    }
    info.mainDataBits = bits;
    return true;
}

// Find the bytes in Layer III frames that hold no audio data.
//
// Layer III has a bit reservoir: a frame's main data (scale factors and
// Huffman-coded samples) may start in the payload of earlier frames. The
// payload areas (everything after header, CRC and side info) of the audio
// frames, in file order, form one stream. Frame N's main data starts
// main_data_begin bytes before its own payload area in that stream and runs
// for ceil(sum of part2_3_length / 8) bytes. Bytes after the end of one
// frame's main data and before the start of the next frame's main data belong
// to no frame: they are ancillary data, and a decoder never reads them, so
// changing them cannot change the decoded audio.
//
// The Xing/Info/VBRI frame is left out of the stream because decoders skip it.
// Layers I and II have no reservoir and are not handled. Sets each frame's
// ancillaryBytes and returns the free regions in stream order.
vector<Region> findAncillaryRegions(vector<MP3FrameInfo>& frames) {
    vector<int> audio;               // indices of Layer III audio frames
    vector<long long> payloadStart;  // where each one's payload starts in the stream
    long long total = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        frames[i].ancillaryBytes = 0;
        if (frames[i].layerDesc == 1 && !frames[i].isVBR) {
            audio.push_back((int)i);
            payloadStart.push_back(total);
            total += frames[i].payloadSize;
        }
    }

    size_t n = audio.size();
    vector<long long> mainStart(n), mainEnd(n);
    for (size_t k = 0; k < n; k++) {
        const MP3FrameInfo& f = frames[audio[k]];
        mainStart[k] = payloadStart[k] - f.mainDataBegin;
        mainEnd[k] = mainStart[k] + (f.mainDataBits + 7) / 8;
        if (mainStart[k] < 0) {
            mainStart[k] = 0; // points before the first frame: nothing there to protect
        }
    }

    // A byte is free if every earlier frame's main data ends before it and
    // every later frame's main data starts after it.
    vector<long long> laterStart(n + 1);
    laterStart[n] = total;
    for (size_t k = n; k-- > 0; ) {
        laterStart[k] = min(laterStart[k + 1], mainStart[k]);
    }

    vector<Region> regions;
    long long usedUpTo = 0;
    size_t j = 0; // reservoir frame whose payload holds the current position
    for (size_t k = 0; k < n; k++) {
        usedUpTo = max(usedUpTo, mainEnd[k]);
        long long from = usedUpTo, to = laterStart[k + 1];
        if (to <= from) {
            continue;
        }
        frames[audio[k]].ancillaryBytes = (int)(to - from);

        // Map the stream range [from, to) back to file offsets
        while (j < n && payloadStart[j] + frames[audio[j]].payloadSize <= from) {
            j++;
        }
        for (long long pos = from; pos < to && j < n; ) {
            const MP3FrameInfo& f = frames[audio[j]];
            long long end = min(to, payloadStart[j] + f.payloadSize);
            Region r;
            r.offset = f.payloadOffset + (int)(pos - payloadStart[j]);
            r.size = (int)(end - pos);
            regions.push_back(r);
            pos = end;
            if (pos >= payloadStart[j] + f.payloadSize) {
                j++;
            }
        }
    }
    return regions;
}

// The payload is written across the ancillary regions, in order, as:
//   "MP3A" | version (1 byte) | payload length (4 bytes, little-endian) |
//   payload | CRC-32 of the payload (4 bytes, little-endian)
const char PAYLOAD_MAGIC[4] = { 'M', 'P', '3', 'A' };
const unsigned char PAYLOAD_VERSION = 1;
const size_t PAYLOAD_HEADER = 4 + 1 + 4;
const size_t PAYLOAD_OVERHEAD = PAYLOAD_HEADER + 4;

// CRC-32 (IEEE 802.3, as used by zip and PNG)
uint32_t crc32(const unsigned char* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void putLE32(vector<unsigned char>& out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out.push_back((unsigned char)((value >> (8 * i)) & 0xFF));
    }
}

uint32_t getLE32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read up to `limit` bytes from the regions, in order
vector<unsigned char> readRegions(const vector<char>& buffer, const vector<Region>& regions, size_t limit) {
    vector<unsigned char> out;
    for (size_t r = 0; r < regions.size() && out.size() < limit; r++) {
        for (int k = 0; k < regions[r].size && out.size() < limit; k++) {
            out.push_back((unsigned char)buffer[regions[r].offset + k]);
        }
    }
    return out;
}

// Write bytes into the regions, in order. The caller checks they fit.
void writeRegions(vector<char>& buffer, const vector<Region>& regions, const vector<unsigned char>& bytes) {
    size_t next = 0;
    for (size_t r = 0; r < regions.size() && next < bytes.size(); r++) {
        for (int k = 0; k < regions[r].size && next < bytes.size(); k++) {
            buffer[regions[r].offset + k] = (char)bytes[next++];
        }
    }
}

// Wrap a payload in the container format
vector<unsigned char> buildContainer(const vector<unsigned char>& payload) {
    vector<unsigned char> out(PAYLOAD_MAGIC, PAYLOAD_MAGIC + 4);
    out.push_back(PAYLOAD_VERSION);
    putLE32(out, (uint32_t)payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    putLE32(out, crc32(payload.data(), payload.size()));
    return out;
}

// Read a payload back from the regions. Returns false, with a reason, if
// there is no payload or it is damaged.
bool extractPayload(const vector<char>& buffer, const vector<Region>& regions,
                    long long capacity, vector<unsigned char>& payload, string& error) {
    vector<unsigned char> header = readRegions(buffer, regions, PAYLOAD_HEADER);
    if (header.size() < PAYLOAD_HEADER || memcmp(header.data(), PAYLOAD_MAGIC, 4) != 0) {
        error = "no embedded payload found";
        return false;
    }
    if (header[4] != PAYLOAD_VERSION) {
        error = "payload format version " + to_string((int)header[4]) + " is not supported";
        return false;
    }
    uint32_t length = getLE32(&header[5]);
    if ((long long)length + (long long)PAYLOAD_OVERHEAD > capacity) {
        error = "payload length field is larger than the ancillary space";
        return false;
    }
    vector<unsigned char> all = readRegions(buffer, regions, PAYLOAD_OVERHEAD + length);
    payload.assign(all.begin() + PAYLOAD_HEADER, all.begin() + PAYLOAD_HEADER + length);
    if (getLE32(&all[PAYLOAD_HEADER + length]) != crc32(payload.data(), payload.size())) {
        error = "payload CRC-32 does not match (the file was changed after embedding)";
        return false;
    }
    return true;
}

// Load data from file for writing to ancillary sections
vector<unsigned char> loadDataFile(const string& inputPath) {
    ifstream inFile(inputPath, ios::binary);
    if (!inFile.is_open()) {
        cerr << "Error: Could not open data file '" << inputPath << "'" << endl;
        return {};
    }
    
    inFile.seekg(0, ios::end);
    streampos fileSize = inFile.tellg();
    inFile.seekg(0, ios::beg);
    if (fileSize <= 0) {
        return {};
    }
    
    vector<unsigned char> data(fileSize);
    inFile.read(reinterpret_cast<char*>(data.data()), fileSize);
    inFile.close();
    
    return data;
}

void printUsage(const char* programName) {
    cout << "Usage: " << programName << " <mp3_file> [options]" << endl;
    cout << endl;
    cout << "Options:" << endl;
    cout << "  (no options)           Parse and display frame information" << endl;
    cout << "  -e <output_file>       Extract an embedded payload to file" << endl;
    cout << "  -w <input_file>        Embed a file in the ancillary space (Layer III only)" << endl;
    cout << "  -o <output_mp3>        Output MP3 file (required with -w)" << endl;
    cout << endl;
    cout << "Examples:" << endl;
    cout << "  " << programName << " song.mp3" << endl;
    cout << "  " << programName << " song.mp3 -e extracted.bin" << endl;
    cout << "  " << programName << " song.mp3 -w secret.txt -o output.mp3" << endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Parse command line arguments
    string mp3Path = argv[1];
    Mode mode = MODE_PARSE;
    string extractPath = "";
    string writePath = "";
    string outputPath = "";
    
    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-e" && i + 1 < argc) {
            mode = MODE_EXTRACT;
            extractPath = argv[++i];
        } else if (arg == "-w" && i + 1 < argc) {
            mode = MODE_WRITE;
            writePath = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }
    
    // Validate arguments for write mode
    if (mode == MODE_WRITE && outputPath.empty()) {
        cerr << "Error: -o <output_mp3> is required when using -w" << endl;
        return 1;
    }

    ifstream mp3File;
    mp3File.open(mp3Path, ios::in | ios::binary);

    if (!mp3File.is_open()) {
        cerr << "Error: Could not open file '" << mp3Path << "'" << endl;
        return 1;
    }

   //inefficient way to read entire file into memory- but simplest
    mp3File.seekg(0, ios::end);
    streampos fileSize = mp3File.tellg();
    mp3File.seekg(0, ios::beg);
    if (fileSize < 0) {
        cerr << "Error: Could not read file '" << mp3Path << "'" << endl;
        return 1;
    }
    vector<char> buffer(fileSize);
    mp3File.read(buffer.data(), fileSize);
    mp3File.close();

    cout << "File: " << mp3Path << endl;
    cout << "File size: " << fileSize << " bytes" << endl;

    // Skip ID3 tag if present
    int startOffset = skipID3Tag(buffer.data(), buffer.size());
    
    if (startOffset > 0) {
        cout << "Skipped ID3 tag of " << startOffset << " bytes" << endl << endl;
    }

    // Header size is always 4 bytes
    const int HEADER_SIZE = 4;
    int frameCount = 0;
    vector<MP3FrameInfo> allFrames;

    // Parse frames
    // i + 4 <= size: a header needs 4 bytes (buffer.size() - 4 would wrap for files under 4 bytes)
    int expectedNext = startOffset;
    for (int i = startOffset; (size_t)i + 4 <= buffer.size(); ) {
        // Try to parse frame at current position
        MP3FrameInfo frameInfo = parseFrameHeader(buffer.data(), buffer.size(), i);
        
        if (!frameInfo.valid) {
            i++; // Move forward one byte and keep searching
            continue;
        }

        // Off the expected position (after junk, or at the start), accept a
        // header only if another matching header follows it, so that sync-like
        // bytes inside junk or tags don't count as frames.
        if (i != expectedNext) {
            int next = i + frameInfo.frameSize;
            if ((size_t)next + 4 <= buffer.size()) {
                MP3FrameInfo nextInfo = parseFrameHeader(buffer.data(), buffer.size(), next);
                if (!nextInfo.valid || nextInfo.mpegVersionID != frameInfo.mpegVersionID ||
                    nextInfo.layerDesc != frameInfo.layerDesc ||
                    nextInfo.sampleRateIndex != frameInfo.sampleRateIndex) {
                    i++;
                    continue;
                }
            }
        }

        // Encoders put a Xing/Info/VBRI tag only in the first frame; looking
        // anywhere else risks mistaking audio data for a tag.
        if (frameCount == 0) {
            frameInfo.isVBR = isVBRHeader(buffer.data(), buffer.size(), i,
                                          (frameInfo.hasCRC ? 2 : 0) + frameInfo.sideInfoSize);
        }

        frameCount++;
        allFrames.push_back(frameInfo);

        // Jump to next frame
        i += frameInfo.frameSize;
        expectedNext = i;
    }

    // Work out which Layer III bytes hold no audio data
    vector<Region> ancillaryRegions = findAncillaryRegions(allFrames);
    long long trueAncillarySpace = 0;
    for (size_t r = 0; r < ancillaryRegions.size(); r++) {
        trueAncillarySpace += ancillaryRegions[r].size;
    }

    // Print frame info in parse mode
    if (mode == MODE_PARSE) {
        const char* layerNames[4] = { "reserved", "III", "II", "I" };
        const char* versionNames[4] = { "2.5", "reserved", "2", "1" };
        for (size_t f = 0; f < allFrames.size(); f++) {
            const MP3FrameInfo& frameInfo = allFrames[f];
            cout << "=== Frame " << (f + 1) << " at position " << frameInfo.position << " ===" << endl;
            int versionIdx = (frameInfo.mpegVersionID == 3) ? 0 : 1;
            int layerIdx = 3 - frameInfo.layerDesc; // Layer I = 0, II = 1, III = 2
            cout << "MPEG Version: " << versionNames[frameInfo.mpegVersionID] << endl;
            cout << "Layer: " << layerNames[frameInfo.layerDesc] << endl;
            cout << "Bitrate: " << bitrates[versionIdx][layerIdx][frameInfo.bitrateIndex] << " kbps" << endl;
            cout << "Sample Rate: " << frameInfo.sampleRate << " Hz" << endl;
            cout << "Frame Size: " << frameInfo.frameSize << endl;
            cout << "Header Size: " << HEADER_SIZE << (frameInfo.hasCRC ? " (+2 CRC)" : "") << endl;
            cout << "Side Info Size: " << frameInfo.sideInfoSize << endl;
            cout << "Channel Mode: " << (frameInfo.isMono ? "Mono" : "Stereo") << endl;
            if (frameInfo.layerDesc == 1) {
                cout << "Main Data Begin: " << frameInfo.mainDataBegin << endl;
                cout << "Main Data Size: " << (frameInfo.mainDataBits + 7) / 8 << " bytes" << endl;
                cout << "Ancillary Bytes: " << frameInfo.ancillaryBytes << endl;
            }
            cout << "VBR Header: " << (frameInfo.isVBR ? "Yes" : "No") << endl;

            if (frameInfo.isVBR) {
                cout << "Note: This frame contains VBR metadata" << endl;
            }
            cout << endl;
        }
    }

    cout << "Total frames parsed: " << frameCount << endl;
    cout << "Total ancillary space: " << trueAncillarySpace << " bytes" << endl;
    
    int layer3Frames = 0;
    for (size_t f = 0; f < allFrames.size(); f++) {
        if (allFrames[f].layerDesc == 1) layer3Frames++;
    }
    if (mode != MODE_PARSE && layer3Frames == 0) {
        cerr << "Error: no Layer III frames found; only Layer III (MP3) files can hold a payload" << endl;
        return 1;
    }

    // Handle extract mode
    if (mode == MODE_EXTRACT) {
        vector<unsigned char> payload;
        string error;
        if (!extractPayload(buffer, ancillaryRegions, trueAncillarySpace, payload, error)) {
            cerr << "Error: " << error << endl;
            return 1;
        }
        ofstream outFile(extractPath, ios::binary);
        if (!outFile.is_open()) {
            cerr << "Error: Could not create output file '" << extractPath << "'" << endl;
            return 1;
        }
        outFile.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        outFile.close();
        cout << "Recovered " << payload.size() << " bytes (CRC-32 OK) to '" << extractPath << "'" << endl;
    }
    
    // Handle write mode
    if (mode == MODE_WRITE) {
        vector<unsigned char> dataToWrite = loadDataFile(writePath);
        if (dataToWrite.empty()) {
            cerr << "Error: No data to write or could not read input file" << endl;
            return 1;
        }
        
        cout << "Data to write: " << dataToWrite.size() << " bytes" << endl;
        
        // A partial payload can't be recovered, so write all of it or nothing
        long long needed = (long long)dataToWrite.size() + (long long)PAYLOAD_OVERHEAD;
        if (needed > trueAncillarySpace) {
            cerr << "Error: the payload needs " << needed << " bytes (" << dataToWrite.size()
                 << " + " << PAYLOAD_OVERHEAD << " for header and CRC), but this file has "
                 << trueAncillarySpace << " bytes of ancillary space. Nothing was written." << endl;
            return 1;
        }
        
        // Write the payload into the ancillary regions, then read it back
        writeRegions(buffer, ancillaryRegions, buildContainer(dataToWrite));
        vector<unsigned char> check;
        string error;
        if (!extractPayload(buffer, ancillaryRegions, trueAncillarySpace, check, error) || check != dataToWrite) {
            cerr << "Error: read-back check failed (" << error << "). Nothing was written." << endl;
            return 1;
        }
        
        // Save modified MP3
        ofstream outFile(outputPath, ios::binary);
        if (!outFile.is_open()) {
            cerr << "Error: Could not create output file '" << outputPath << "'" << endl;
            return 1;
        }
        outFile.write(buffer.data(), buffer.size());
        outFile.close();
        cout << "Wrote " << dataToWrite.size() << " bytes (+ " << PAYLOAD_OVERHEAD
             << " header and CRC) into " << trueAncillarySpace << " bytes of ancillary space" << endl;
        cout << "Saved modified MP3 to '" << outputPath << "'" << endl;
    }

    return 0;
}