# The skb-arrow process host: miint talks to scikit-bio over the gpl-boundary
# transport (pipes, Arrow IPC in POSIX shared memory), so no Python code is
# linked in. Included from gpl_boundary.cmake, so MIINT_ENABLE_GPL_BOUNDARY
# gates both.
list(APPEND MIINT_EXTENSION_SOURCES src/skb_arrow/client.cpp
     src/skb_arrow/install.cpp src/install_skb_arrow.cpp
     src/skb_arrow_functions.cpp)
list(APPEND MIINT_TEST_SOURCES src/skb_arrow/client.cpp
     src/skb_arrow/install.cpp test/cpp/test_skb_arrow_client.cpp)
