// Runs pngToLogo() on every file given and writes <file>.565 (raw 64x64 RGB565) or the error
#include "LogoImage.h"
#include <cstdio>
#include <vector>
#include <fstream>
#include <iterator>
int main(int argc, char** argv){
  for(int i = 1; i < argc; i++){
    std::ifstream f(argv[i], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint16_t> out(LOGO_SIZE * LOGO_SIZE, 0xDEAD);
    const char* err = pngToLogo(d.data(), d.size(), out.data());
    if(err) printf("%s\tERROR\t%s\n", argv[i], err);
    else {
      std::ofstream o(std::string(argv[i]) + ".565", std::ios::binary);
      o.write((const char*)out.data(), out.size() * 2);
      printf("%s\tOK\n", argv[i]);
    }
  }
}
