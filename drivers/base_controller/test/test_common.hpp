#ifndef DDSM_DRIVER_TEST_COMMON_HPP_
#define DDSM_DRIVER_TEST_COMMON_HPP_

#include <cstdlib>
#include <iostream>

#define DDSM_CHECK(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "CHECK failed: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
      std::exit(1); \
    } \
  } while (0)

#endif  // DDSM_DRIVER_TEST_COMMON_HPP_
