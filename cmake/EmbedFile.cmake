# Writes OUTPUT: a C++ source whose embeddedPage() returns the bytes of INPUT. Run by CMake at build time, so the executable
# carries the page and needs no files beside it.
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
get_filename_component(folder "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${folder}")
file(WRITE "${OUTPUT}"
"// Generated from ${INPUT} by cmake/EmbedFile.cmake. Do not edit.
#include <string>

#include \"EmbeddedPage.h\"

namespace nofences {

std::string embeddedPage() {
    static const unsigned char bytes[] = {${bytes}};
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

}  // namespace nofences
")
