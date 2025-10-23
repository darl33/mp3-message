# include <iostream>
# include <fstream>
# include <string>
# include <vector>
using namespace std;

int main() {
    ifstream mp3File;
    string name;
    cout << "Enter file name: \n";
    cin >> name;
    mp3File.open(name, ios::in | ios::binary);

    if (!mp3File.is_open()) {
        cout << "Error: Could not open file " << name << endl;
        return 1;
    }

    char buffer[128];
    mp3File.read(buffer, sizeof(buffer));

    for (int i = 0; i < 127; ++i) {
        if (buffer[i] == 0xFF && (buffer[i + 1] & 0xE0) == 0xE0) {

            // Found frame header
            break;
        }
    }


    mp3File.close();
    return 0;
}