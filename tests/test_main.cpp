#include <iostream>
#include <exception>
#include "image_runtime.h"
namespace cst {
void imageRuntimeSelfTest();
void protocolSelfTest();
void frameCodecSelfTest();
void fileTransferSelfTest();
void endToEndSelfTest();
} // namespace cst
int main() {
    try {
        cst::initializeImageRuntime();
        cst::imageRuntimeSelfTest();
        cst::protocolSelfTest();
        cst::frameCodecSelfTest();
        cst::fileTransferSelfTest();
        cst::endToEndSelfTest();
        std::cout << "All protocol, frame, real-file and end-to-end image tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
