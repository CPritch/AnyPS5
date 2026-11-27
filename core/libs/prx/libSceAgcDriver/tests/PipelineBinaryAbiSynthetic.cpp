#include "PipelineBinaryAbi.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    return PipelineBinaryAbiSynthetic(argv[1]);
}
