#include "novacache/version.hpp"

#include <gtest/gtest.h>

TEST(VersionTest, ReturnsProjectVersion) { EXPECT_EQ(novacache::version(), "0.1.0"); }

TEST(VersionTest, IsNotEmpty) { EXPECT_FALSE(novacache::version().empty()); }
