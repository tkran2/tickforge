#include "tickforge.hpp"
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    using namespace tickforge;
    if (argc != 3 || std::string(argv[1]) != "replay") { std::cerr << "Usage: tickforge replay file.tkf\n"; return 2; }
    std::ifstream input(argv[2], std::ios::binary);
    if (!input) { std::cerr << "Cannot open input\n"; return 2; }
    FeedState state; Frame frame{}; std::uint64_t frames = 0;
    while (input.read(reinterpret_cast<char*>(frame.data()), frame.size())) { state.ingest(frame); ++frames; }
    if (input.gcount()) { std::cerr << "Truncated final frame; replay rejected\n"; return 1; }
    if (!input.eof()) { std::cerr << "Read error\n"; return 1; }
    std::cout << "{\"frames\":" << frames << ",\"publications\":" << state.publications
        << ",\"gaps\":" << state.gaps << ",\"malformed\":" << state.malformed
        << ",\"trusted\":" << (state.trusted() ? "true" : "false")
        << ",\"digest\":" << state.digest() << "}\n";
    return state.gaps || state.malformed || !state.trusted() ? 1 : 0;
}
