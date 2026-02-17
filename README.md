# MP3 Ancillary Data Tool

A C++ utility for parsing MP3 files, extracting ancillary data from frames, and embedding custom data into MP3 ancillary data sections without affecting audio playback.

## Overview

This tool analyzes MP3 frame structure including headers, side information, and ancillary data sections. It can:
- Parse and display detailed MP3 frame information
- Extract ancillary data from MP3 files
- Write custom data to ancillary data sections (steganography)

## Features

- **Frame Analysis**: Displays MPEG version, layer, bitrate, sample rate, frame size, side info size
- **VBR Detection**: Identifies VBR headers (Xing, Info, VBRI)
- **Ancillary Data Extraction**: Extracts hidden data from MP3 ancillary sections
- **Data Embedding**: Writes custom data to ancillary sections without corrupting audio
- **ID3 Tag Handling**: Automatically skips ID3v2 tags
- **Support**: Works with MPEG-1, MPEG-2, and MPEG-2.5 Layer I/II/III files

## Building

### Prerequisites
- C++ compiler with C++11 support (g++, clang++, MSVC)
- Standard C++ libraries

### Compilation

**Using g++:**
```bash
g++ -std=c++11 -o mp3read mp3read.cpp
```

**Using MSYS2 on Windows:**
```bash
C:\msys64\ucrt64\bin\g++.exe -fdiagnostics-color=always -g mp3read.cpp -o mp3read.exe
```

**Using VSCode:** Use the pre-configured build task "C/C++: g++.exe build active file"

## Usage

### Syntax
```
mp3read <mp3_file> [options]
```

### Options
| Option | Description |
|--------|-------------|
| *(none)* | Parse and display frame information |
| `-e <output_file>` | Extract ancillary data to file |
| `-w <input_file>` | Write data from file to ancillary sections |
| `-o <output_mp3>` | Output MP3 file (required with `-w`) |
| `-h, --help` | Display help message |

## Examples

### 1. Parse MP3 Frame Information
```bash
mp3read song.mp3
```

**Output:**
```
File: song.mp3
File size: 4567890 bytes

=== Frame 1 at position 0 ===
Frame Size: 417
Header Size: 4
Side Info Size: 32
Channel Mode: Stereo
Main Data Begin: 0
Ancillary Data Size (est.): 23
VBR Header: No

=== Frame 2 at position 417 ===
...

Total frames parsed: 1234
Total estimated ancillary space: 28422 bytes
```

### 2. Extract Ancillary Data
```bash
mp3read song.mp3 -e extracted.bin
```

Extracts all ancillary data from MP3 frames to `extracted.bin`.

### 3. Embed Data into MP3
```bash
mp3read song.mp3 -w secret.txt -o output.mp3
```

Writes contents of `secret.txt` to the ancillary data sections of `song.mp3` and saves to `output.mp3`. The audio remains fully playable.

### 4. Steganography Workflow
```bash
# Step 1: Embed secret message
mp3read original.mp3 -w message.txt -o hidden.mp3

# Step 2: Extract hidden message
mp3read hidden.mp3 -e recovered.txt
```

## Technical Details

### MP3 Frame Structure
```
[Frame Header (4 bytes)]
[Side Information (9-32 bytes)]
[Main Audio Data (variable)]
[Ancillary Data (variable)]
```

### Ancillary Data
- Located at the end of each MP3 frame after main audio data
- Typically 5-15% of frame size (heuristic estimate)
- Can store arbitrary data without affecting audio playback
- VBR header frames are skipped (no ancillary data written)

### Supported Formats
- **MPEG Version**: MPEG-1, MPEG-2, MPEG-2.5
- **Layers**: Layer I, II, III (MP3 is Layer III)
- **Bitrates**: All standard bitrates (32-448 kbps)
- **Sample Rates**: 8000-48000 Hz
- **Channel Modes**: Mono, Stereo, Joint Stereo, Dual Channel

### main_data_begin
- MPEG-1: 9-bit value indicating start of main audio data
- MPEG-2/2.5: 8-bit value
- Used to calculate frame boundaries and ancillary data offset

## Limitations

- **Estimate-based**: Ancillary data size is estimated (conservative 1/16 of frame)
- **VBR Frames Skipped**: Frames with VBR headers are not used for data storage
- **Memory Usage**: Loads entire MP3 file into memory (not suitable for very large files)
- **No Validation**: Does not verify MP3 integrity after writing
- **Partial Writes**: If data exceeds available space, only partial data is written

## Use Cases

1. **Steganography**: Hide messages in MP3 files
2. **Metadata Storage**: Store custom metadata outside ID3 tags
3. **Digital Watermarking**: Embed invisible watermarks
4. **MP3 Analysis**: Study frame structure and encoding patterns
5. **Research**: Analyze ancillary data usage in different encoders

## Error Handling

- Invalid MP3 files: Skips invalid frames, continues parsing
- Missing files: Displays error and exits
- Insufficient space: Warns and writes partial data
- Missing arguments: Displays usage information

## Security & Privacy Notice

This tool can embed hidden data in MP3 files. Be aware:
- Hidden data may be discoverable by forensic analysis
- Not suitable for encryption (data is not encrypted)
- May be detectable by audio analysis tools
- Use responsibly and ethically

## License

This code is provided as-is for educational and research purposes.

## Contributing

Improvements welcome:
- More accurate ancillary data size calculation
- Streaming mode for large files
- Encryption support
- Better VBR frame handling
- Additional metadata parsing

## Author

Created for MP3 frame analysis and ancillary data manipulation research.
