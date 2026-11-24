#include <mln/platform/gl_functions.hpp>

#include <mln/util/logging.hpp>
#include <sstream>

namespace mln {
namespace platform {

#ifndef NDEBUG
void glCheckError(const char* cmd, const char* file, int line) {
    if (GLenum err = glGetError()) {
        Log::Warning(Event::GraphicsBackend,
                     "Error" + std::to_string(err) + ": " + cmd + " - " + file + ":" + std::to_string(line));
    }
}
#endif

void glDiscardPendingErrors() {
    static int reported = 0;
    while (GLenum err = glGetError()) {
        if (reported < 20) {
            reported++;
            Log::Warning(Event::GraphicsBackend, "Discarding pending GL error " + std::to_string(err) + " before an allocation");
        }
    }
}

} // namespace platform
} // namespace mln
