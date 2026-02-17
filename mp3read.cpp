#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <vector>
#include <cstring>

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
    int frameSize;
    int sideInfoSize;
    bool isVBR;
    bool valid;
    int position;           // Position in file
    int mainDataBegin;      // main_data_begin from side info
    int ancillaryDataSize;  // Estimated ancillary data size
    int ancillaryDataOffset; // Offset where ancillary data starts
};

struct AncillaryData {
    int frameNumber;
    int offset;
    int size;
    vector<unsigned char> data;
};

// Forward declarations
int parseMainDataBegin(const char* buffer, int sideInfoStart, int mpegVersionID);

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

    // Parse main_data_begin from side info
    int sideInfoStart = position + 4; // After header
    info.mainDataBegin = parseMainDataBegin(buffer, sideInfoStart, info.mpegVersionID);
    
    // Estimate ancillary data size
    // Ancillary data is at the end of the frame after the main audio data
    // For a conservative estimate, we use the last portion of the frame
    // A more accurate calculation would require parsing the full granule info
    int headerAndSideInfo = 4 + info.sideInfoSize;
    int mainDataArea = info.frameSize - headerAndSideInfo;
    
    // Estimate ~5-15% of frame might be ancillary data in typical MP3s
    // This is a heuristic; actual size depends on encoder and bitrate
    info.ancillaryDataSize = max(0, mainDataArea / 16); // Conservative estimate
    info.ancillaryDataOffset = position + info.frameSize - info.ancillaryDataSize;
    
    info.position = position;

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

// Parse main_data_begin from side info (first 9 bits for MPEG1, first 8 bits for MPEG2/2.5)
int parseMainDataBegin(const char* buffer, int sideInfoStart, int mpegVersionID) {
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

// Extract ancillary data from a frame
// Returns estimated ancillary data (bytes after main audio data)
AncillaryData extractAncillaryData(const vector<char>& buffer, const MP3FrameInfo& frame) {
    AncillaryData ancData;
    ancData.frameNumber = 0;
    ancData.offset = frame.ancillaryDataOffset;
    ancData.size = frame.ancillaryDataSize;
    
    if (frame.ancillaryDataSize > 0 && frame.ancillaryDataOffset + frame.ancillaryDataSize <= buffer.size()) {
        ancData.data.resize(frame.ancillaryDataSize);
        for (int i = 0; i < frame.ancillaryDataSize; i++) {
            ancData.data[i] = buffer[frame.ancillaryDataOffset + i];
        }
    }
    
    return ancData;
}

// Write data to ancillary section of frames
bool writeAncillaryData(vector<char>& buffer, const vector<MP3FrameInfo>& frames, 
                        const vector<unsigned char>& dataToWrite) {
    int dataIndex = 0;
    int bytesWritten = 0;
    
    for (const auto& frame : frames) {
        if (frame.ancillaryDataSize <= 0 || !frame.valid || frame.isVBR) {
            continue;
        }
        
        int spaceAvailable = frame.ancillaryDataSize;
        int offset = frame.ancillaryDataOffset;
        
        for (int i = 0; i < spaceAvailable && dataIndex < dataToWrite.size(); i++) {
            if (offset + i < buffer.size()) {
                buffer[offset + i] = dataToWrite[dataIndex++];
                bytesWritten++;
            }
        }
        
        if (dataIndex >= dataToWrite.size()) {
            break;
        }
    }
    
    cout << "Wrote " << bytesWritten << " bytes to ancillary data sections" << endl;
    return dataIndex >= dataToWrite.size();
}

// Save extracted ancillary data to file
void saveAncillaryData(const string& outputPath, const vector<AncillaryData>& allAncData) {
    ofstream outFile(outputPath, ios::binary);
    if (!outFile.is_open()) {
        cerr << "Error: Could not create output file '" << outputPath << "'" << endl;
        return;
    }
    
    int totalBytes = 0;
    for (const auto& ancData : allAncData) {
        if (!ancData.data.empty()) {
            outFile.write(reinterpret_cast<const char*>(ancData.data.data()), ancData.data.size());
            totalBytes += ancData.data.size();
        }
    }
    
    outFile.close();
    cout << "Saved " << totalBytes << " bytes of ancillary data to '" << outputPath << "'" << endl;
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
    cout << "  -e <output_file>       Extract ancillary data to file" << endl;
    cout << "  -w <input_file>        Write data from file to ancillary sections" << endl;
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
    vector<AncillaryData> allAncData;
    int totalAncillarySpace = 0;

    // Parse frames
    for (int i = startOffset; i < (buffer.size() - 4); ) {
        // Try to parse frame at current position
        MP3FrameInfo frameInfo = parseFrameHeader(buffer.data(), i);
        
        if (!frameInfo.valid) {
            i++; // Move forward one byte and keep searching
            continue;
        }

        frameCount++;
        allFrames.push_back(frameInfo);
        
        if (!frameInfo.isVBR) {
            totalAncillarySpace += frameInfo.ancillaryDataSize;
        }
        
        // Extract ancillary data if in extract mode
        if (mode == MODE_EXTRACT && !frameInfo.isVBR) {
            AncillaryData ancData = extractAncillaryData(buffer, frameInfo);
            ancData.frameNumber = frameCount;
            allAncData.push_back(ancData);
        }
        
        // Print frame info in parse mode
        if (mode == MODE_PARSE) {
            cout << "=== Frame " << frameCount << " at position " << i << " ===" << endl;
            cout << "Frame Size: " << frameInfo.frameSize << endl;
            cout << "Header Size: " << HEADER_SIZE << endl;
            cout << "Side Info Size: " << frameInfo.sideInfoSize << endl;
            cout << "Channel Mode: " << (frameInfo.isMono ? "Mono" : "Stereo") << endl;
            cout << "Main Data Begin: " << frameInfo.mainDataBegin << endl;
            cout << "Ancillary Data Size (est.): " << frameInfo.ancillaryDataSize << endl;
            cout << "VBR Header: " << (frameInfo.isVBR ? "Yes" : "No") << endl;

            if (frameInfo.isVBR) {
                cout << "Note: This frame contains VBR metadata" << endl;
            }
            cout << endl;
        }

        // Jump to next frame
        i += frameInfo.frameSize;
    }

    cout << "Total frames parsed: " << frameCount << endl;
    cout << "Total estimated ancillary space: " << totalAncillarySpace << " bytes" << endl;
    
    // Handle extract mode
    if (mode == MODE_EXTRACT) {
        saveAncillaryData(extractPath, allAncData);
    }
    
    // Handle write mode
    if (mode == MODE_WRITE) {
        vector<unsigned char> dataToWrite = loadDataFile(writePath);
        if (dataToWrite.empty()) {
            cerr << "Error: No data to write or could not read input file" << endl;
            return 1;
        }
        
        cout << "Data to write: " << dataToWrite.size() << " bytes" << endl;
        
        if (dataToWrite.size() > totalAncillarySpace) {
            cerr << "Warning: Data size (" << dataToWrite.size() << " bytes) exceeds available ancillary space (" 
                 << totalAncillarySpace << " bytes)" << endl;
            cerr << "Only partial data will be written." << endl;
        }
        
        // Write data to ancillary sections
        writeAncillaryData(buffer, allFrames, dataToWrite);
        
        // Save modified MP3
        ofstream outFile(outputPath, ios::binary);
        if (!outFile.is_open()) {
            cerr << "Error: Could not create output file '" << outputPath << "'" << endl;
            return 1;
        }
        outFile.write(buffer.data(), buffer.size());
        outFile.close();
        cout << "Saved modified MP3 to '" << outputPath << "'" << endl;
    }

    return 0;
}