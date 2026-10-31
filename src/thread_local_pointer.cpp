#include "thread_local_pointer.hpp"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace bg::util {

thread_local_pointer::thread_local_pointer() {
#if defined(_WIN32)
    const DWORD key = TlsAlloc();
    if (key == TLS_OUT_OF_INDEXES) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "failed to allocate thread-local storage"};
    }
#else
    pthread_key_t key;
    const int error = pthread_key_create(&key, nullptr);
    if (error != 0) {
        throw std::system_error{error, std::system_category(), "failed to allocate thread-local storage"};
    }
#endif

    key_ = static_cast<std::uintptr_t>(key);
}

thread_local_pointer::~thread_local_pointer() {
#if defined(_WIN32)
    TlsFree(static_cast<DWORD>(key_));
#else
    pthread_key_delete(static_cast<pthread_key_t>(key_));
#endif
}

void *thread_local_pointer::get() const noexcept {
#if defined(_WIN32)
    return TlsGetValue(static_cast<DWORD>(key_));
#else
    return pthread_getspecific(static_cast<pthread_key_t>(key_));
#endif
}

void thread_local_pointer::set(void *value) noexcept {
#if defined(_WIN32)
    if (TlsSetValue(static_cast<DWORD>(key_), value) == 0) {
        std::abort();
    }
#else
    if (pthread_setspecific(static_cast<pthread_key_t>(key_), value) != 0) {
        std::abort();
    }
#endif
}

} // namespace bg::util
