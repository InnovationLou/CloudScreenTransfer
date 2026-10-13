#include <iostream>
#include <exception>
namespace cst {
void protocolSelfTest();
void frameCodecSelfTest();
void fileTransferSelfTest();
void endToEndSelfTest();
} // namespace cst
int main() {
    try {
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
