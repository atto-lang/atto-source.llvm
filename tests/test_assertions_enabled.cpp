#include <cassert>

#ifdef NDEBUG
#error "Native safety tests must retain assertions in every build configuration"
#endif

int main() {
    int executed = 0;
    assert(++executed == 1);
    return executed == 1 ? 0 : 1;
}
