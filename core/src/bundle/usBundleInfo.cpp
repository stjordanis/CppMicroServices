/*=============================================================================

  Library: CppMicroServices

  Copyright (c) The CppMicroServices developers. See the COPYRIGHT
  file at the top-level directory of this distribution and at
  https://github.com/saschazelzer/CppMicroServices/COPYRIGHT .

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

=============================================================================*/


#include "usBundleInfo.h"

#include <stdexcept> // std::runtime_error
#include <limits.h>  // PATH_MAX
#include <stdlib.h>  // realpath
namespace us {

BundleInfo::BundleInfo(const std::string& location, const std::string& name)
: name(name)
, id(0)
{
  if (location.empty())
  {
    throw std::runtime_error("Empty bundle path");
  }
#if defined(US_PLATFORM_WINDOWS)
  this->location = location;
#else
  // resolve any symlinks. 
  char filePath[PATH_MAX];
  char* resultPath = ::realpath(location.c_str(), filePath);
  if (resultPath)
  {
    this->location = std::string(filePath);
  }
  else
  {
    throw std::runtime_error("Invalid bundle path");
  }
#endif
}

}
