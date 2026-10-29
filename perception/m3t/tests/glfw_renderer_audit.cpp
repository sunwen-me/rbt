#include <GL/gl.h>

#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>

struct GLFWwindow;

namespace {

using MakeContextCurrentFn = void (*)(GLFWwindow*);

MakeContextCurrentFn resolve_next() {
    void* symbol = dlsym(RTLD_NEXT, "glfwMakeContextCurrent");
    if (symbol == nullptr) {
        const char* error = dlerror();
        std::fprintf(stderr, "RBT_GL_AUDIT error=resolve_failed detail=%s\n",
                     error != nullptr ? error : "unknown");
        std::fflush(stderr);
        _exit(127);
    }
    return reinterpret_cast<MakeContextCurrentFn>(symbol);
}

}  // namespace

extern "C" void glfwMakeContextCurrent(GLFWwindow* window) {
    static const MakeContextCurrentFn next = resolve_next();
    next(window);

    if (window == nullptr) {
        return;
    }

    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    if (renderer == nullptr || version == nullptr) {
        return;
    }

    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set(std::memory_order_relaxed)) {
        std::fprintf(stderr, "RBT_GL_AUDIT pid=%ld renderer=%s version=%s\n",
                     static_cast<long>(getpid()),
                     reinterpret_cast<const char*>(renderer),
                     reinterpret_cast<const char*>(version));
        std::fflush(stderr);
    }
}
