# MP3 Ancillary Data Tool

A C++ utility for parsing MP3 files and hiding a file in the bytes of each Layer III frame that hold no audio data, so the decoded audio stays bit-identical.


## Acknowledgements
AI has been used to generate comments and fix style, specifically Claude Opus 4.5 with Github Copilot

## Overview

This tool analyzes MP3 frame structure including headers, side information, and ancillary data sections. It can:
- Parse and display detailed MP3 frame information
- Report how many bytes of each frame hold no audio data (ancillary space)
- Write a file into that space (steganography) and read it back

## Features

- **Frame Analysis**: Displays MPEG version, layer, bitrate, sample rate, frame size, side info size
- **VBR Detection**: Identifies VBR headers (Xing, Info, VBRI)
- **Real Ancillary Space**: Parses Layer III side info and follows the bit reservoir to find the bytes no frame's audio data uses
- **Data Embedding**: Writes only those bytes, so the decoded audio is bit-identical; refuses, and writes nothing, if the payload doesn't fit
- **Payload Extraction**: Recovers the payload and checks its CRC-32
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

### Testing

```bash
tests/run_tests.sh        # optional argument: number of fuzz runs (default 300)
```

Needs `g++` (or set `CXX`), `sha256sum` and `ffmpeg` built with libmp3lame. The script builds `mp3read` with AddressSanitizer and UBSan, encodes test signals (CBR, VBR, mono, MPEG-2, MPEG-2.5, ID3-tagged, and libshine if available), and checks for each that:

- an embedded payload comes back byte for byte,
- the decoded PCM of the output is bit-identical to the original (`ffmpeg -f s16le | sha256sum`),
- a payload one byte too big is refused and no file is written.

It also checks the error paths (Layer II input, no payload, a damaged payload, empty and truncated files) and runs a short mutation fuzz of the parser under the sanitizers.

## Usage

### Syntax
```
mp3read <mp3_file> [options]
```

### Options
| Option | Description |
|--------|-------------|
| *(none)* | Parse and display frame information |
| `-e <output_file>` | Extract an embedded payload to file |
| `-w <input_file>` | Embed a file in the ancillary space (Layer III only) |
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
File size: 97010 bytes
Skipped ID3 tag of 45 bytes

=== Frame 1 at position 45 ===
MPEG Version: 1
Layer: III
Bitrate: 128 kbps
Sample Rate: 44100 Hz
Frame Size: 417
Header Size: 4
Side Info Size: 32
Channel Mode: Stereo
Main Data Begin: 0
Main Data Size: 0 bytes
Ancillary Bytes: 0
VBR Header: Yes
Note: This frame contains VBR metadata

=== Frame 2 at position 462 ===
MPEG Version: 1
Layer: III
Bitrate: 128 kbps
Sample Rate: 44100 Hz
Frame Size: 417
Header Size: 4
Side Info Size: 32
Channel Mode: Stereo
Main Data Begin: 0
Main Data Size: 257 bytes
Ancillary Bytes: 0
VBR Header: No
...

Total frames parsed: 232
Total ancillary space: 8336 bytes
```

Frame 2 has 381 bytes after its side info but only 257 bytes of main data, yet 0 ancillary bytes: frame 3's main data starts in frame 2's last 124 bytes (the bit reservoir).

```
```

### 2. Extract Ancillary Data
```bash
mp3read song.mp3 -e extracted.bin
```

Recovers the embedded payload to `extracted.bin` after checking its CRC-32. It exits with an error, and creates no file, if the MP3 holds no payload or the payload is damaged.

### 3. Embed Data into MP3
```bash
mp3read song.mp3 -w secret.txt -o output.mp3
```

Writes `secret.txt` into the ancillary space of `song.mp3` and saves the result as `output.mp3`. Only bytes outside every frame's main data change, so decoding `output.mp3` gives exactly the same samples as `song.mp3`. If the payload plus 13 bytes of header and CRC doesn't fit, it writes nothing and exits with an error.

### 4. Steganography Workflow
```bash
# Step 1: Embed secret message
mp3read original.mp3 -w message.txt -o hidden.mp3

# Step 2: Extract hidden message
mp3read hidden.mp3 -e recovered.txt
```

## Technical Details

### MP3 Frame Structure (Layer III)
```
[Frame Header (4 bytes)]
[CRC (2 bytes, only if the protection bit is clear)]
[Side Information (9-32 bytes)]
[Payload area: main data of this and/or later frames, and ancillary bytes]
```

### Bit Reservoir and Ancillary Data
- A Layer III frame's main data (scale factors and Huffman-coded samples) does not have to sit in its own frame. Laid end to end, the payload areas of the audio frames form one stream, and each frame's main data starts `main_data_begin` bytes before its own payload area in that stream.
- Its length is the sum of `part2_3_length` over the frame's granules and channels, rounded up to whole bytes.
- Bytes after one frame's main data and before the next frame's main data belong to no frame. Those are the ancillary bytes: a decoder never reads them, so changing them cannot change the decoded audio.
- The Xing/Info/VBRI frame is never written to, and decoders skip it, so it is left out of the stream.
- How much space there is depends on the encoder and the audio. On 6-second test signals: LAME at 128 kbps left 8,336 bytes on a pure tone and 967 bytes on pink noise; libshine left none.

### Payload Format
The payload is written across the ancillary bytes, in order:
```
"MP3A" (4 bytes) | format version 1 (1 byte) | payload length (4 bytes, little-endian) | payload | CRC-32 of payload (4 bytes, little-endian)
```

### Supported Formats
- **MPEG Version**: MPEG-1, MPEG-2, MPEG-2.5
- **Layers**: Layer I, II, III (MP3 is Layer III)
- **Bitrates**: All standard bitrates (32-448 kbps)
- **Sample Rates**: 8000-48000 Hz
- **Channel Modes**: Mono, Stereo, Joint Stereo, Dual Channel

### main_data_begin
- MPEG-1: 9-bit value indicating start of main audio data
- MPEG-2/2.5: 8-bit value
- Places each frame's main data in the reservoir stream (see above)

## Limitations

- **Layer III only**: Layers I and II have no bit reservoir, and finding where their audio data ends needs a full bit-allocation parse, so they can be parsed but not written to
- **Capacity varies**: some encoders leave little or no ancillary space; run the tool without options to see a file's capacity
- **Fragile**: re-encoding the MP3, or cutting frames out of it, destroys the payload
- **Old format**: files written by v1.0.0 (which overwrote the last 1/16 of each frame and could damage the audio) can't be read by this version
- **Memory Usage**: Loads entire MP3 file into memory (not suitable for very large files)

## Use Cases

1. **Steganography**: Hide messages in MP3 files
2. **Metadata Storage**: Store custom metadata outside ID3 tags
3. **Digital Watermarking**: Embed invisible watermarks
4. **MP3 Analysis**: Study frame structure and encoding patterns
5. **Research**: Analyze ancillary data usage in different encoders

## Error Handling

- Invalid MP3 files: Skips invalid frames, continues parsing
- Missing files: Displays error and exits
- Insufficient space: Refuses and writes nothing
- Damaged or missing payload: Reports the reason and exits with an error
- Missing arguments: Displays usage information

## Security & Privacy Notice

This tool can embed hidden data in MP3 files. Be aware:
- Easy to detect: encoders usually leave ancillary bytes as zeros, so random-looking ancillary data stands out, even though the decoded audio is unchanged
- Not suitable for encryption (data is not encrypted)
- Use responsibly and ethically

## License

Released under the [MIT License](LICENSE).

## Contributing

Improvements welcome:
- Layer I and II support
- Streaming mode for large files
- Encryption support
- Additional metadata parsing

## Author

Created for MP3 frame analysis and ancillary data manipulation research.
