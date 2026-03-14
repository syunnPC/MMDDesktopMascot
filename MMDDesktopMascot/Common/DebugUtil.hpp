#pragma once

#include <cstring>

#ifndef FILENAME
#define FILENAME (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#endif