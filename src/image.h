#pragma once
// Image has been superseded by StaticImageSource. This alias keeps existing
// call sites compiling during the migration. Remove once all uses are updated.
#include "static_image_source.h"
namespace baktsiu { using Image = StaticImageSource; }
